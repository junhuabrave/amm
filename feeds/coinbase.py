#!/usr/bin/env python3
"""Coinbase Exchange public market data as normalised events.

    python3 feeds/coinbase.py BTC-USD ETH-USD | ./build/amm_run --config my.ini

Symbols are emitted as COINBASE:<PRODUCT>. REST polling of the public book
and trades endpoints; no account or API key is involved.
"""
import argparse

import common

BASE = "https://api.exchange.coinbase.com"


def parse_book(payload):
    """{"bids": [["price", "size", num_orders]], "asks": [...]} -> (bid, bid_sz, ask, ask_sz)"""
    bid, ask = payload["bids"][0], payload["asks"][0]
    return float(bid[0]), float(bid[1]), float(ask[0]), float(ask[1])


def parse_trades(payload, last_id):
    """[{"trade_id", "price", "size", "side", "time"}, ...] -> ([(price, qty, side)], new_last_id)

    Coinbase reports the MAKER side: "buy" is a down-tick (the aggressor
    sold), "sell" an up-tick. Same cursor behaviour as the Binance adapter.
    """
    rows = sorted(payload, key=lambda t: t["trade_id"])
    newest = rows[-1]["trade_id"] if rows else last_id
    if last_id is None:
        return [], newest
    out = [(float(t["price"]), float(t["size"]), "S" if t["side"] == "buy" else "B") for t in rows if t["trade_id"] > last_id]
    return out, newest


class CoinbaseSource:
    def __init__(self, product, base=BASE, fetch=common.http_json):
        self.product = product.upper()
        self.base = base
        self.name = "COINBASE:" + self.product
        self.fetch = fetch
        self.last_id = None

    def poll(self):
        book = self.fetch("%s/products/%s/book?level=1" % (self.base, self.product))
        trades = self.fetch("%s/products/%s/trades?limit=100" % (self.base, self.product))
        new, self.last_id = parse_trades(trades, self.last_id)
        events = [("T", self.name, px, qty, side) for px, qty, side in new]
        events.append(("B", self.name) + parse_book(book))
        return events


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("products", nargs="+", help="e.g. BTC-USD")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between polls (default 1.0)")
    ap.add_argument("--duration", type=float, help="stop after this many seconds")
    ap.add_argument("--url", default=BASE)
    a = ap.parse_args()
    try:
        common.run([CoinbaseSource(p, a.url) for p in a.products], a.interval, a.duration)
    except (KeyboardInterrupt, BrokenPipeError):
        pass


if __name__ == "__main__":
    main()
