#!/usr/bin/env python3
"""Drop copy: an independent, append-only journal of everything the venue says about the account.

    python3 feeds/binance_dropcopy.py --journal data/dropcopy.jsonl --perp BTCUSDT ETHUSDT

Runs as its own process on its own websocket connection to the account's
user-data stream, separate from the order path (the gateway), so a bug or a
stall in the gateway or the engine cannot hide an execution from it. Every
message is written as one JSON line {"recv_ns": ..., "msg": {...}} and the
file is flushed on every write and fsynced every second. A snapshot of
positions and open orders from REST is written first, and again after every
reconnect (messages may have been missed while disconnected):
{"recv_ns": ..., "snapshot": {"positions": {...}, "open": [client ids]}}.

feeds/reconcile.py reads this journal. Testnet by default; mainnet needs the
same two switches as the gateway.
"""
import argparse
import json
import os
import selectors
import sys
import time

import binance_trade as bt
import ws


class Journal:
    def __init__(self, path, clock=time.time_ns):
        self.f = open(path, "a", buffering=1)
        self.clock = clock
        self.last_sync = 0.0

    def write(self, rec):
        rec = dict(rec)
        rec.setdefault("recv_ns", self.clock())
        self.f.write(json.dumps(rec, separators=(",", ":")) + "\n")
        self.f.flush()
        if time.time() - self.last_sync >= 1.0:
            os.fsync(self.f.fileno())
            self.last_sync = time.time()

    def close(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()


def snapshot(rest, symbols):
    opened = []
    for s in symbols:
        opened += [o["clientOrderId"] for o in rest.open_orders(s)]
    pos = rest.positions()
    return {"positions": {s: pos.get(s, 0.0) for s in symbols}, "open": opened}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--journal", required=True)
    ap.add_argument("--perp", nargs="+", required=True)
    ap.add_argument("--mainnet", action="store_true")
    ap.add_argument("--keepalive", type=float, default=1800.0)
    a = ap.parse_args()
    symbols = [s.upper() for s in a.perp]
    rest_base, ws_base = bt.endpoints(a.mainnet)
    key, secret = bt.credentials()
    rest = bt.Rest(rest_base, key, secret)
    rest.sync_time()
    j = Journal(a.journal)
    backoff = 0.0
    try:
        while True:
            try:
                listen = rest.listen_key()
                conn = ws.WsConnection(ws_base + listen)
                j.write({"snapshot": snapshot(rest, symbols)})
                print("dropcopy: connected", file=sys.stderr, flush=True)
                backoff = 0.0
                last_keepalive = time.time()
                sel = selectors.DefaultSelector()
                sel.register(conn, selectors.EVENT_READ)
                while True:
                    for _ in sel.select(timeout=1.0):
                        for m in conn.read():
                            d = json.loads(m)
                            j.write({"msg": d})
                            if d.get("e") == "listenKeyExpired":
                                raise ConnectionError("listen key expired")
                    if time.time() - last_keepalive > a.keepalive:
                        rest.keepalive()
                        last_keepalive = time.time()
            except (OSError, ValueError, ConnectionError, bt.ApiError) as e:
                backoff = min(60.0, max(1.0, 2 * backoff))
                j.write({"event": "disconnected", "why": str(e)})
                print("dropcopy: %s (reconnect in %.0fs)" % (e, backoff), file=sys.stderr, flush=True)
                time.sleep(backoff)
    except KeyboardInterrupt:
        pass
    finally:
        j.close()


if __name__ == "__main__":
    main()
