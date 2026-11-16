#!/usr/bin/env python3
"""Order gateway for Binance USD-M futures: engine order lines in, execution reports out.

    mkfifo /tmp/amm_orders
    python3 feeds/binance_ws.py --perp BTCUSDT ETHUSDT \\
      | python3 feeds/binance_gateway.py --orders /tmp/amm_orders --perp BTCUSDT ETHUSDT \\
      | ./build/amm_run --config config/binance_perp.ini --orders /tmp/amm_orders
    (scripts/live_gateway.sh does this, plus the drop copy and reconciler)

The gateway is the only writer of the engine's event stream: market-data lines
arrive on stdin and are passed through unchanged, and execution reports
(A/F/D/P/G lines, see include/amm/live/events.hpp) are added between them, so
no two processes ever interleave inside a line.

Orders from the engine (N, X, K lines on --orders) are sent as signed REST
calls from a small worker pool; fills and order states come back on the
account's user-data websocket. Defaults to the TESTNET (testnet.binancefuture.com).
Mainnet needs --mainnet AND AMM_ALLOW_MAINNET=MAINNET_REAL_MONEY.

Startup: sync clock, read tick/step filters, cancel every open order on the
configured symbols, open the user-data stream, report positions (P), then
report ready (G). Shutdown (engine exits, Ctrl-C, SIGTERM) cancels every open
order. Losing the user-data stream reports the gateway down (G,down), which
stops the engine: without execution reports it cannot know its position.

--dry-run needs no keys and sends nothing anywhere: orders are acknowledged and
cancelled locally (never filled), to try the whole pipeline on live data.
"""
import argparse
import concurrent.futures
import fcntl
import json
import os
import selectors
import signal
import sys
import threading
import time

import binance_trade as bt
import ws

PREFIX = "BINANCE-PERP:"


class DryVenue:
    """Loopback venue: accepts and cancels, never fills. Reports through the same path as Binance."""

    def __init__(self, deliver):
        self.deliver = deliver  # callable(user-data message dict)
        self.live = {}
        self.lock = threading.Lock()

    def _event(self, sym, cid, x, status):
        self.deliver({"e": "ORDER_TRADE_UPDATE", "o": {"s": sym, "c": cid, "x": x, "X": status}})

    def new_order(self, p):
        with self.lock:
            self.live[p["newClientOrderId"]] = p["symbol"]
        if p["timeInForce"] == "IOC":  # nothing to take in a loopback: expires at once
            with self.lock:
                self.live.pop(p["newClientOrderId"], None)
            self._event(p["symbol"], p["newClientOrderId"], "EXPIRED", "EXPIRED")
        else:
            self._event(p["symbol"], p["newClientOrderId"], "NEW", "NEW")
        return {"clientOrderId": p["newClientOrderId"]}

    def cancel_order(self, sym, cid):
        with self.lock:
            had = self.live.pop(cid, None)
        if had is None:
            raise bt.ApiError(400, -2011, "Unknown order sent.")
        self._event(sym, cid, "CANCELED", "CANCELED")

    def query_order(self, sym, cid):
        with self.lock:
            live = cid in self.live
        if not live:
            raise bt.ApiError(400, -2013, "Order does not exist.")
        return {"status": "NEW"}

    def cancel_all(self, sym):
        with self.lock:
            gone = [c for c, s in self.live.items() if s == sym]
            for c in gone:
                del self.live[c]
        for c in gone:
            self._event(sym, c, "CANCELED", "CANCELED")

    def positions(self):
        return {}


class Gateway:
    """Order state and the translation between engine lines and venue calls.

    venue: object with new_order(params), cancel_order(sym, cid), query_order(sym, cid),
    cancel_all(sym) -- bt.Rest or DryVenue. emit(line) writes one report line.
    """

    def __init__(self, venue, symbols, filters, tag, emit, workers=4, clock=time.time_ns, synchronous=False):
        self.venue, self.symbols, self.filters, self.tag = venue, set(symbols), filters, tag
        self.emit_line, self.clock = emit, clock
        self.orders = {}  # engine id -> {"sym", "cid", "state": sending|live, "cancel": bool}
        self.lock = threading.Lock()
        self.killed = False
        self.paused_until = 0.0
        self.sync = synchronous
        self.pool = None if synchronous else concurrent.futures.ThreadPoolExecutor(max_workers=workers)
        self.stats = {"sent": 0, "rejected": 0, "cancels": 0, "errors": 0}

    def emit(self, *r):
        self.emit_line(bt.report_line(self.clock(), r))

    def _run(self, fn, *a):
        if self.sync:
            fn(*a)
        else:
            self.pool.submit(fn, *a)

    # ---- engine -> venue
    def on_order_line(self, line):
        f = line.strip().split(",")
        if not f or not f[0]:
            return
        if f[0] == "N" and len(f) == 7:
            self.new(int(f[1]), f[2], f[3], float(f[4]), float(f[5]), f[6])
        elif f[0] == "X" and len(f) >= 2:
            self.cancel(int(f[1]))
        elif f[0] == "K":
            self.kill(f[1] if len(f) > 1 else "engine")
        else:
            print("gateway: ignored order line %.120r" % line, file=sys.stderr, flush=True)

    def new(self, eid, feed_sym, side, px, qty, tif):
        sym = feed_sym[len(PREFIX):] if feed_sym.startswith(PREFIX) else feed_sym
        if self.killed:
            return self.emit("D", eid, "rejected killed")
        if time.time() < self.paused_until:
            return self.emit("D", eid, "rejected rate limited")
        if sym not in self.symbols:
            return self.emit("D", eid, "rejected unknown symbol")
        cid = bt.client_id(self.tag, eid)
        try:
            params = bt.order_params(sym, side, px, qty, tif, cid, self.filters)
        except ValueError as e:
            return self.emit("D", eid, "rejected %s" % e)
        with self.lock:
            self.orders[eid] = {"sym": sym, "cid": cid, "state": "sending", "cancel": False}
        self.stats["sent"] += 1
        self._run(self._send_new, eid, params)

    def _send_new(self, eid, params):
        try:
            self.venue.new_order(params)
        except bt.ApiError as e:
            self.stats["rejected"] += 1
            self._forget(eid)
            if e.status in (418, 429):  # rate limited or banned: stop sending new orders for a while
                retry = float(e.headers.get("retry-after", 10) or 10)
                self.paused_until = time.time() + retry
                print("gateway: venue rate limit (%s), pausing new orders %.0fs" % (e, retry), file=sys.stderr, flush=True)
            return self.emit("D", eid, "rejected %s" % (e.code if e.code is not None else e.status))
        except OSError as e:  # unknown outcome: ask the venue what happened
            self.stats["errors"] += 1
            if not self._resolve_unknown(eid, params["symbol"], params["newClientOrderId"], e):
                return
        with self.lock:
            o = self.orders.get(eid)
            if o is None:
                return  # already reported done by the stream
            o["state"] = "live"
            want_cancel = o["cancel"]
        if want_cancel:
            self._send_cancel(eid)

    def _resolve_unknown(self, eid, sym, cid, err):
        for attempt in range(3):
            time.sleep(0.2 * (attempt + 1))
            try:
                self.venue.query_order(sym, cid)
                return True  # it exists: treat as live; the stream reports its state
            except bt.ApiError as e:
                if e.code == -2013:  # does not exist: it never got there
                    self._forget(eid)
                    self.emit("D", eid, "rejected not received")
                    return False
            except OSError:
                continue
        print("gateway: order %s state unknown after %s; stopping" % (cid, err), file=sys.stderr, flush=True)
        self.emit("G", "down")
        return False

    def cancel(self, eid):
        with self.lock:
            o = self.orders.get(eid)
            if o is None:
                return  # unknown or already done: nothing to cancel
            if o["state"] == "sending":
                o["cancel"] = True  # cancel once the venue has the order, never before
                return
        self._run(self._send_cancel, eid)

    def _send_cancel(self, eid):
        with self.lock:
            o = self.orders.get(eid)
        if o is None:
            return
        self.stats["cancels"] += 1
        try:
            self.venue.cancel_order(o["sym"], o["cid"])
        except bt.ApiError as e:
            if e.code == -2011:  # unknown order: it has just filled or expired; the stream will say
                return
            print("gateway: cancel %s failed: %s" % (o["cid"], e), file=sys.stderr, flush=True)
        except OSError as e:
            print("gateway: cancel %s failed: %s; cancelling all on %s" % (o["cid"], e, o["sym"]), file=sys.stderr, flush=True)
            self._run(self._cancel_all, o["sym"])

    def kill(self, why):
        if self.killed:
            return
        self.killed = True
        print("gateway: kill (%s): cancelling all open orders" % why, file=sys.stderr, flush=True)
        for s in sorted(self.symbols):
            self._run(self._cancel_all, s)

    def _cancel_all(self, sym):
        try:
            self.venue.cancel_all(sym)
        except (bt.ApiError, OSError) as e:
            print("gateway: cancel-all %s failed: %s" % (sym, e), file=sys.stderr, flush=True)

    def _forget(self, eid):
        with self.lock:
            self.orders.pop(eid, None)

    # ---- venue -> engine
    def on_user_event(self, msg):
        for r in bt.reports_from_user_event(msg, self.tag, PREFIX):
            if r[0] == "D":
                self._forget(r[1])
            if r[0] == "P" and r[1][len(PREFIX):] not in self.symbols:
                continue
            self.emit(*r)

    def shutdown(self):
        self.kill("shutdown")
        if self.pool:
            self.pool.shutdown(wait=True)


class LineWriter:
    """Whole lines to a file descriptor, from any thread, never interleaved."""

    def __init__(self, fd):
        self.fd, self.lock = fd, threading.Lock()

    def write(self, text):
        data = text.encode()
        with self.lock:
            while data:
                n = os.write(self.fd, data)
                data = data[n:]


def open_fifo(path):
    # O_RDWR keeps the pipe open with no writer yet, so reads block instead of
    # returning end-of-file until the engine connects (works on Linux and macOS).
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    return fd


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--orders", required=True, help="named pipe the engine writes order lines to")
    ap.add_argument("--perp", nargs="+", required=True, help="USD-M symbols, e.g. BTCUSDT ETHUSDT")
    ap.add_argument("--dry-run", action="store_true", help="no keys, no network: acknowledge and cancel locally")
    ap.add_argument("--mainnet", action="store_true", help="real money; also needs AMM_ALLOW_MAINNET")
    ap.add_argument("--keepalive", type=float, default=1800.0, help="listen-key keepalive interval, seconds")
    ap.add_argument("--journal", help="dry run only: write the loopback's account messages here in the "
                                      "drop-copy format, so reconcile.py can be tried without keys")
    a = ap.parse_args()

    out = LineWriter(sys.stdout.fileno())
    tag = bt.session_tag()
    symbols = [s.upper() for s in a.perp]
    sel = selectors.DefaultSelector()
    user_ws = None
    rest = None
    stopping = threading.Event()
    gw_box = {}

    def emit(line):
        out.write(line + "\n")

    if a.dry_run:
        journal = None
        if a.journal:
            import binance_dropcopy
            journal = binance_dropcopy.Journal(a.journal)
            journal.write({"snapshot": {"positions": {s: 0.0 for s in symbols}, "open": []}})

        def deliver(m):
            if journal:
                journal.write({"msg": m})
            gw_box["gw"].on_user_event(m)
        venue = DryVenue(deliver)
        filters = {s: {"tick": "0.01", "step": "0.001", "min_qty": "0.001", "min_notional": "0"} for s in symbols}
        positions = {}
        print("gateway: DRY RUN, nothing is sent to any venue", file=sys.stderr, flush=True)
    else:
        rest_base, ws_base = bt.endpoints(a.mainnet)
        key, secret = bt.credentials()
        rest = bt.Rest(rest_base, key, secret)
        off = rest.sync_time()
        filters = bt.parse_filters(rest.exchange_info(), symbols)
        for s in symbols:
            rest.cancel_all(s)
        listen = rest.listen_key()
        user_ws = ws.WsConnection(ws_base + listen)
        positions = rest.positions()
        venue = rest
        print("gateway: %s, clock offset %d ms, session %s" % ("MAINNET" if a.mainnet else "testnet", off, tag),
              file=sys.stderr, flush=True)

    gw = Gateway(venue, symbols, filters, tag, emit)
    gw_box["gw"] = gw
    for s in symbols:
        gw.emit("P", PREFIX + s, positions.get(s, 0.0))
    if a.dry_run:
        gw.emit("G", "loopback")  # our orders are not in the venue's book: do not filter them out of it
    gw.emit("G", "ready")

    def on_signal(signum, frame):
        stopping.set()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    ofd = open_fifo(a.orders)
    sel.register(ofd, selectors.EVENT_READ, "orders")
    fl = fcntl.fcntl(sys.stdin.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(sys.stdin.fileno(), fcntl.F_SETFL, fl | os.O_NONBLOCK)
    sel.register(sys.stdin.fileno(), selectors.EVENT_READ, "md")
    if user_ws:
        sel.register(user_ws, selectors.EVENT_READ, "user")
    obuf, mbuf = b"", b""
    last_keepalive = time.time()
    down = False
    try:
        while not stopping.is_set():
            for key_, _ in sel.select(timeout=0.5):
                if key_.data == "orders":
                    try:
                        chunk = os.read(ofd, 65536)
                    except BlockingIOError:
                        continue
                    obuf += chunk
                    *lines, obuf = obuf.split(b"\n")
                    for l in lines:
                        gw.on_order_line(l.decode())
                elif key_.data == "md":
                    try:
                        chunk = os.read(sys.stdin.fileno(), 1 << 16)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise EOFError("market data ended")
                    mbuf += chunk
                    cut = mbuf.rfind(b"\n")
                    if cut >= 0:  # pass whole lines through, keep the partial tail
                        out.write(mbuf[:cut + 1].decode())
                        mbuf = mbuf[cut + 1:]
                elif key_.data == "user":
                    try:
                        msgs = user_ws.read()
                    except (OSError, ValueError) as e:
                        raise ConnectionError("user stream lost: %s" % e)
                    for m in msgs:
                        d = json.loads(m)
                        if d.get("e") == "listenKeyExpired":
                            raise ConnectionError("listen key expired")
                        gw.on_user_event(d)
            now = time.time()
            if rest and now - last_keepalive > a.keepalive:
                try:
                    rest.keepalive()
                except (bt.ApiError, OSError) as e:
                    print("gateway: keepalive failed: %s" % e, file=sys.stderr, flush=True)
                last_keepalive = now
    except (EOFError, ConnectionError) as e:
        print("gateway: %s" % e, file=sys.stderr, flush=True)
        down = True
    except BrokenPipeError:
        print("gateway: engine went away", file=sys.stderr, flush=True)
    finally:
        gw.shutdown()
        if down:
            try:
                gw.emit("G", "down")
            except OSError:
                pass
        if rest:
            try:
                rest.close_listen_key()
            except (bt.ApiError, OSError):
                pass
        if user_ws:
            user_ws.close()
        print("gateway: stopped (%s)" % json.dumps(gw.stats), file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
