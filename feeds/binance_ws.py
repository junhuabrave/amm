#!/usr/bin/env python3
"""Binance public market data over websockets (spot and USD-M perpetuals) as normalised events.

    python3 feeds/binance_ws.py --perp BTCUSDT ETHUSDT | ./build/amm_run --config config/binance_perp.ini

Same symbols and event lines as feeds/binance.py, but pushed by the venue
instead of polled: every top-of-book change (bookTicker) and every aggregated
trade (aggTrade), typically within a few milliseconds of the matching engine.

Streams:
  spot  wss://stream.binance.com:9443/stream          bookTicker + aggTrade, one connection
  perp  wss://fstream.binance.com/public/stream       bookTicker
        wss://fstream.binance.com/market/stream       aggTrade
(Binance routes high-frequency futures data under /public and the rest under
/market. Override with --perp-book-url / --perp-trade-url, e.g. to the legacy
combined wss://fstream.binance.com/stream, which keeps book and trades in one
ordered connection.)

Timestamps are local receipt times, as with the REST adapter, so the stream is
monotonic across connections. Book and trades of a perpetual arrive on two
connections, so a trade can be written a millisecond before or after the book
change it caused; the engine treats both as observations of the same market.
Exchange-to-receipt latency per stream is reported on stderr every --stats seconds.

--bybit-perp adds Bybit USDT perpetuals (feeds/bybit_ws.py) to the same
stream, as BYBIT-PERP:SYMBOL, for cross-venue measurements.

Nothing is sent to the venue but the websocket handshake (and, for Bybit, a
subscription and pings): no account, no key, no orders.
"""
import argparse
import json
import selectors
import sys
import time

import common
import ws

SPOT_WS = "wss://stream.binance.com:9443/stream"
PERP_BOOK_WS = "wss://fstream.binance.com/public/stream"
PERP_TRADE_WS = "wss://fstream.binance.com/market/stream"
MAX_STREAMS_PER_CONN = 200  # Binance allows 200 (futures) / 1024 (spot) streams per connection


def parse_book_ticker(d):
    """bookTicker payload -> (symbol, bid, bid_size, ask, ask_size)."""
    return d["s"], float(d["b"]), float(d["B"]), float(d["a"]), float(d["A"])


def parse_agg_trade(d):
    """aggTrade payload -> (symbol, price, qty, aggressor side).

    m (buyer is maker) true means the buyer was resting, so the aggressor sold.
    """
    return d["s"], float(d["p"]), float(d["q"]), "S" if d["m"] else "B"


def stream_urls(spot, perp, spot_url=SPOT_WS, perp_book_url=PERP_BOOK_WS, perp_trade_url=PERP_TRADE_WS):
    """[(url, prefix)] covering every requested symbol; combined streams, chunked per connection."""
    def chunks(names):
        return [names[i:i + MAX_STREAMS_PER_CONN] for i in range(0, len(names), MAX_STREAMS_PER_CONN)]

    def url(base, names):
        return base + "?streams=" + "/".join(names)

    out = []
    s = [x.lower() for x in spot]
    p = [x.lower() for x in perp]
    for names in chunks([n + "@bookTicker" for n in s] + [n + "@aggTrade" for n in s]):
        out.append((url(spot_url, names), "BINANCE:"))
    if perp_book_url == perp_trade_url:  # one combined connection keeps book and trades ordered
        for names in chunks([n + "@bookTicker" for n in p] + [n + "@aggTrade" for n in p]):
            out.append((url(perp_book_url, names), "BINANCE-PERP:"))
    else:
        for names in chunks([n + "@bookTicker" for n in p]):
            out.append((url(perp_book_url, names), "BINANCE-PERP:"))
        for names in chunks([n + "@aggTrade" for n in p]):
            out.append((url(perp_trade_url, names), "BINANCE-PERP:"))
    return out


def to_event(msg, prefix):
    """One combined-stream message -> (event tuple or None, kind, exchange time ms or None)."""
    m = json.loads(msg)
    d = m.get("data", m)
    stream = m.get("stream", "")
    kind = stream.rsplit("@", 1)[-1] if "@" in stream else d.get("e", "")
    if kind == "bookTicker":
        sym, b, bs, a, a_s = parse_book_ticker(d)
        if not (b > 0 and a > 0 and a > b):
            return None, kind, d.get("E")
        return ("B", prefix + sym, b, bs, a, a_s), kind, d.get("E")
    if kind == "aggTrade":
        sym, px, q, side = parse_agg_trade(d)
        return ("T", prefix + sym, px, q, side), kind, d.get("E")
    return None, kind, None  # subscription acks and anything unknown


class Stats:
    def __init__(self):
        self.reset()

    def reset(self):
        self.count = {}
        self.lat = {}

    def add(self, kind, recv_ms, exch_ms):
        self.count[kind] = self.count.get(kind, 0) + 1
        if exch_ms:
            self.lat.setdefault(kind, []).append(recv_ms - exch_ms)

    def report(self, seconds):
        parts = []
        for kind in sorted(self.count):
            s = "%s %.1f/s" % (kind, self.count[kind] / max(seconds, 1e-9))
            l = sorted(self.lat.get(kind, []))
            if l:
                s += " latency p50 %d ms p99 %d ms" % (l[len(l) // 2], l[min(len(l) - 1, int(len(l) * 0.99))])
            parts.append(s)
        return "; ".join(parts) or "no messages"


class BinanceStream:
    """One Binance combined-stream connection (no subscription, no client pings)."""

    ping_every = 0.0

    def __init__(self, url, prefix):
        self.url, self.prefix = url, prefix

    def on_open(self, conn):
        pass

    def events(self, msg):
        ev, kind, exch = to_event(msg, self.prefix)
        return [] if ev is None else [(ev, kind, exch)]


def as_stream(spec):
    return BinanceStream(*spec) if isinstance(spec, tuple) else spec


def run(specs, duration=None, stats_every=60.0, stall=30.0, cafile=None, out=sys.stdout, err=sys.stderr,
        connect=None, clock=time.time_ns):
    """Hold one websocket per spec, write event lines until `duration` seconds pass.

    A spec is a Binance (url, prefix) pair or a stream object with url,
    on_open(conn), events(msg) and ping_every / ping(conn) (bybit_ws.BybitPerp).

    A connection that fails, is closed by the server (Binance closes after
    24 h) or goes `stall` seconds without a message is reconnected with
    exponential back-off; the others keep running.
    """
    connect = connect or (lambda url: ws.WsConnection(url, cafile=cafile))
    specs = [as_stream(x) for x in specs]
    last_ping = {}
    sel = selectors.DefaultSelector()
    conns = {}  # spec index -> connection
    retry_at = {i: 0 for i in range(len(specs))}
    backoff = {i: 0.0 for i in range(len(specs))}
    last_msg = {}
    stats = Stats()
    start = last_stats = clock()

    def drop(i, why):
        c = conns.pop(i, None)
        if c is not None:
            try:
                sel.unregister(c)
            except (KeyError, ValueError):
                pass
            c.close()
        backoff[i] = min(60.0, max(1.0, 2.0 * backoff[i]))
        retry_at[i] = clock() + int(backoff[i] * 1e9)
        print("feed %s: %s (reconnect in %.0fs)" % (specs[i].url.split("?")[0], why, backoff[i]), file=err, flush=True)

    try:
        while duration is None or (clock() - start) / 1e9 < duration:
            now = clock()
            for i, sp in enumerate(specs):
                if i not in conns and now >= retry_at[i]:
                    c = None
                    try:
                        c = connect(sp.url)
                        sp.on_open(c)
                    except (OSError, ValueError) as e:
                        if c is not None:
                            c.close()
                        drop(i, e)
                        continue
                    conns[i] = c
                    last_msg[i] = last_ping[i] = now
                    sel.register(c, selectors.EVENT_READ, i)
                elif i in conns and sp.ping_every and (now - last_ping[i]) / 1e9 >= sp.ping_every:
                    try:
                        sp.ping(conns[i])
                    except OSError as e:
                        drop(i, e)
                        continue
                    last_ping[i] = now
            for key, _ in sel.select(timeout=0.5 if conns else 0.2):
                i = key.data
                try:
                    msgs = conns[i].read()
                except (OSError, ValueError) as e:
                    drop(i, e)
                    continue
                if not msgs:
                    continue
                recv = clock()
                last_msg[i] = recv
                backoff[i] = 0.0
                lines = []
                for m in msgs:
                    try:
                        evs = specs[i].events(m)
                    except (ValueError, KeyError, TypeError) as e:
                        print("feed: unparsed message (%s): %.200s" % (e, m), file=err, flush=True)
                        continue
                    for ev, kind, exch in evs:
                        stats.add(kind, recv // 1_000_000, exch)
                        lines.append(common.line(recv, ev[0], ev[1], *ev[2:]))
                if lines:
                    out.write("\n".join(lines) + "\n")
                    out.flush()
            now = clock()
            for i in list(conns):
                if (now - last_msg[i]) / 1e9 > stall:
                    drop(i, "no message for %.0fs" % stall)
            if stats_every and (now - last_stats) / 1e9 >= stats_every:
                print("feed stats: " + stats.report((now - last_stats) / 1e9), file=err, flush=True)
                stats.reset()
                last_stats = now
    finally:
        for c in list(conns.values()):
            c.close()
        sel.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spot", nargs="*", help="spot symbols, e.g. BTCUSDT")
    ap.add_argument("--perp", nargs="*", default=[], help="USD-M perpetual symbols")
    ap.add_argument("--bybit-perp", nargs="*", default=[], help="Bybit USDT perpetual symbols (BYBIT-PERP:...)")
    ap.add_argument("--duration", type=float, help="stop after this many seconds")
    ap.add_argument("--stats", type=float, default=60.0, help="seconds between rate/latency reports on stderr (0: off)")
    ap.add_argument("--stall", type=float, default=30.0, help="reconnect a connection silent this many seconds")
    ap.add_argument("--cafile", help="CA bundle for TLS verification (default: system; also $AMM_CAFILE)")
    ap.add_argument("--spot-url", default=SPOT_WS, help="e.g. wss://stream.binance.us:9443/stream")
    ap.add_argument("--perp-book-url", default=PERP_BOOK_WS)
    ap.add_argument("--perp-trade-url", default=PERP_TRADE_WS)
    a = ap.parse_args()
    if not a.spot and not a.perp and not a.bybit_perp:
        ap.error("give at least one symbol")
    specs = stream_urls(a.spot, a.perp, a.spot_url, a.perp_book_url, a.perp_trade_url) if a.spot or a.perp else []
    if a.bybit_perp:
        import bybit_ws
        specs.append(bybit_ws.BybitPerp(a.bybit_perp))
    try:
        run(specs, a.duration, a.stats, a.stall, a.cafile)
    except (KeyboardInterrupt, BrokenPipeError):
        pass


if __name__ == "__main__":
    main()
