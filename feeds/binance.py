#!/usr/bin/env python3
"""Binance public market data (spot and USD-M perpetuals) as normalised events.

    python3 feeds/binance.py BTCUSDT ETHUSDT --perp BTCUSDT | ./build/amm_run --config config/crypto.ini

Spot symbols are emitted as BINANCE:<SYMBOL>, perpetuals as BINANCE-PERP:<SYMBOL>.
Uses REST polling of the public depth and recent-trades endpoints; no account
or API key is involved and nothing is ever sent to the venue but GET requests.
"""
import argparse

import common

SPOT = "https://api.binance.com/api/v3"
PERP = "https://fapi.binance.com/fapi/v1"


def parse_depth(payload):
    """{"bids": [["price", "qty"], ...], "asks": [...]} -> (bid, bid_sz, ask, ask_sz)"""
    bid, ask = payload["bids"][0], payload["asks"][0]
    return float(bid[0]), float(bid[1]), float(ask[0]), float(ask[1])


def parse_trades(payload, last_id):
    """[{"id", "price", "qty", "time", "isBuyerMaker"}, ...] -> ([(price, qty, side)], new_last_id)

    isBuyerMaker true means the buyer was resting, so the aggressor sold.
    Only trades newer than last_id are returned, oldest first. With
    last_id None nothing is returned: the first poll just sets the cursor so
    history is not replayed as if it were live.
    """
    rows = sorted(payload, key=lambda t: t["id"])
    newest = rows[-1]["id"] if rows else last_id
    if last_id is None:
        return [], newest
    out = [(float(t["price"]), float(t["qty"]), "S" if t["isBuyerMaker"] else "B") for t in rows if t["id"] > last_id]
    return out, newest


class BinanceSource:
    def __init__(self, symbol, perp=False, base=None, fetch=common.http_json):
        self.symbol = symbol.upper()
        self.base = base or (PERP if perp else SPOT)
        self.name = ("BINANCE-PERP:" if perp else "BINANCE:") + self.symbol
        self.fetch = fetch
        self.last_id = None

    def poll(self):
        depth = self.fetch("%s/depth?symbol=%s&limit=5" % (self.base, self.symbol))
        trades = self.fetch("%s/trades?symbol=%s&limit=200" % (self.base, self.symbol))
        new, self.last_id = parse_trades(trades, self.last_id)
        events = [("T", self.name, px, qty, side) for px, qty, side in new]  # trades led to this book
        events.append(("B", self.name) + parse_depth(depth))
        return events


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spot", nargs="*", help="spot symbols, e.g. BTCUSDT")
    ap.add_argument("--perp", nargs="*", default=[], help="USD-M perpetual symbols")
    ap.add_argument("--interval", type=float, default=0.5, help="seconds between polls (default 0.5)")
    ap.add_argument("--duration", type=float, help="stop after this many seconds")
    ap.add_argument("--spot-url", default=SPOT, help="override, e.g. https://api.binance.us/api/v3")
    ap.add_argument("--perp-url", default=PERP)
    a = ap.parse_args()
    sources = [BinanceSource(s, base=a.spot_url) for s in a.spot] + [BinanceSource(s, True, a.perp_url) for s in a.perp]
    if not sources:
        ap.error("give at least one symbol")
    try:
        common.run(sources, a.interval, a.duration)
    except (KeyboardInterrupt, BrokenPipeError):
        pass


if __name__ == "__main__":
    main()
