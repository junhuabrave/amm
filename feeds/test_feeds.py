"""Offline tests of the adapters against payloads shaped like the venues' documented responses.

    python3 feeds/test_feeds.py
"""
import io
import unittest

import binance
import coinbase
import common

BINANCE_DEPTH = {"lastUpdateId": 1027024,
                 "bids": [["60000.10", "0.431"], ["60000.00", "2.0"]],
                 "asks": [["60000.20", "0.120"], ["60000.30", "1.5"]]}
BINANCE_TRADES = [
    {"id": 101, "price": "60000.10", "qty": "0.010", "quoteQty": "600.0", "time": 1499865549590, "isBuyerMaker": True, "isBestMatch": True},
    {"id": 103, "price": "60000.20", "qty": "0.030", "quoteQty": "1800.0", "time": 1499865549800, "isBuyerMaker": False, "isBestMatch": True},
    {"id": 102, "price": "60000.20", "qty": "0.020", "quoteQty": "1200.0", "time": 1499865549700, "isBuyerMaker": False, "isBestMatch": True},
]
COINBASE_BOOK = {"sequence": 13051505638, "bids": [["6247.58", "6.3578146", 2]], "asks": [["6251.52", "2", 1]],
                 "time": "2021-02-12T01:09:23.334723Z"}
COINBASE_TRADES = [
    {"time": "2020-03-20T00:36:59.860Z", "trade_id": 86327327, "price": "6225.32", "size": "0.06469797", "side": "sell"},
    {"time": "2020-03-20T00:36:58.000Z", "trade_id": 86327326, "price": "6225.00", "size": "0.5", "side": "buy"},
]


class Parsers(unittest.TestCase):
    def test_binance_depth(self):
        self.assertEqual(binance.parse_depth(BINANCE_DEPTH), (60000.10, 0.431, 60000.20, 0.120))

    def test_binance_trades_cursor_order_and_aggressor(self):
        new, last = binance.parse_trades(BINANCE_TRADES, None)
        self.assertEqual((new, last), ([], 103))  # first poll only sets the cursor
        new, last = binance.parse_trades(BINANCE_TRADES, 101)
        self.assertEqual(new, [(60000.20, 0.020, "B"), (60000.20, 0.030, "B")])  # oldest first, buyer aggressor
        self.assertEqual(last, 103)
        new, _ = binance.parse_trades(BINANCE_TRADES, 100)
        self.assertEqual(new[0], (60000.10, 0.010, "S"))  # buyer was maker: aggressor sold
        self.assertEqual(binance.parse_trades([], 55), ([], 55))

    def test_coinbase(self):
        self.assertEqual(coinbase.parse_book(COINBASE_BOOK), (6247.58, 6.3578146, 6251.52, 2.0))
        new, last = coinbase.parse_trades(COINBASE_TRADES, 86327325)
        self.assertEqual(new, [(6225.00, 0.5, "S"), (6225.32, 0.06469797, "B")])  # side is the maker's
        self.assertEqual(last, 86327327)


class Plumbing(unittest.TestCase):
    def test_source_emits_trades_then_book(self):
        calls = []

        def fetch(url):
            calls.append(url)
            return BINANCE_DEPTH if "/depth" in url else BINANCE_TRADES

        s = binance.BinanceSource("btcusdt", perp=True, fetch=fetch)
        self.assertEqual(s.name, "BINANCE-PERP:BTCUSDT")
        self.assertEqual(s.poll(), [("B", s.name, 60000.10, 0.431, 60000.20, 0.120)])
        s.last_id = 102
        self.assertEqual(s.poll(), [("T", s.name, 60000.20, 0.030, "B"), ("B", s.name, 60000.10, 0.431, 60000.20, 0.120)])
        self.assertTrue(all(u.startswith("https://fapi.binance.com/fapi/v1/") for u in calls))

    def test_run_writes_lines_and_survives_a_failing_source(self):
        class Good:
            name = "GOOD"
            def poll(self):
                return [("B", "X:Y", 1.0, 2.0, 3.0, 4.0), ("T", "X:Y", 1.5, 0.25, "S")]

        class Bad:
            name = "BAD"
            def poll(self):
                raise OSError("network down")

        t = [0]

        def clock():
            t[0] += 100_000_000  # 0.1 s per call
            return t[0]

        out, err = io.StringIO(), io.StringIO()
        common.run([Bad(), Good()], interval=0.1, duration=1.0, out=out, err=err, clock=clock, sleep=lambda s: None)
        lines = out.getvalue().splitlines()
        self.assertTrue(lines)
        ts, kind, sym, bid, bsz, ask, asz = lines[0].split(",")
        self.assertEqual((kind, sym, float(bid), float(bsz), float(ask), float(asz)), ("B", "X:Y", 1.0, 2.0, 3.0, 4.0))
        self.assertEqual(lines[1].split(",")[1:], ["T", "X:Y", "1.5", "0.25", "S"])
        self.assertEqual(err.getvalue().count("network down"), 1)  # backed off, not retried every poll
        stamps = [int(l.split(",")[0]) for l in lines]
        self.assertEqual(stamps, sorted(stamps))


if __name__ == "__main__":
    unittest.main()
