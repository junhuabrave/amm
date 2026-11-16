#!/usr/bin/env python3
"""Offline tests for the trading path: signing, rounding, ids, user-data parsing,
the gateway's order handling, the drop-copy book and the reconciler.

    python3 feeds/test_trading.py

User-data message fixtures follow the field layout of Binance's USD-M
futures documentation (ORDER_TRADE_UPDATE, ACCOUNT_UPDATE); they are not
captures from a live account, which needs API keys.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import binance_gateway as gwm  # noqa: E402
import binance_trade as bt  # noqa: E402
import reconcile as rc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILTERS = {
    "BTCUSDT": {"tick": "0.10", "step": "0.001", "min_qty": "0.001", "min_notional": "100"},
    "ETHUSDT": {"tick": "0.01", "step": "0.001", "min_qty": "0.001", "min_notional": "20"},
}

EXCHANGE_INFO = {"symbols": [
    {"symbol": "BTCUSDT", "filters": [
        {"filterType": "PRICE_FILTER", "minPrice": "556.80", "maxPrice": "4529764", "tickSize": "0.10"},
        {"filterType": "LOT_SIZE", "stepSize": "0.001", "maxQty": "1000", "minQty": "0.001"},
        {"filterType": "MIN_NOTIONAL", "notional": "100"}]},
    {"symbol": "ETHUSDT", "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.01"},
        {"filterType": "LOT_SIZE", "stepSize": "0.001", "minQty": "0.001"},
        {"filterType": "MIN_NOTIONAL", "notional": "20"}]},
    {"symbol": "XRPUSDT", "filters": []},
]}


def otu(cid, x, X, s="BTCUSDT", side="BUY", l="0", L="0", z="0", m=True):
    return {"e": "ORDER_TRADE_UPDATE", "E": 1, "T": 1, "o": {
        "s": s, "c": cid, "S": side, "o": "LIMIT", "f": "GTX", "q": "0.005", "p": "65000",
        "x": x, "X": X, "i": 123, "l": l, "z": z, "L": L, "m": m, "ps": "BOTH"}}


class Signing(unittest.TestCase):
    def test_binance_documentation_example(self):
        secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"
        q = ("symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
             "&recvWindow=5000&timestamp=1499827319559")
        self.assertEqual(bt.sign(secret, q), "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71")

    def test_mainnet_needs_two_switches(self):
        self.assertEqual(bt.endpoints(False, env={})[0], bt.TESTNET_REST)
        with self.assertRaises(SystemExit):
            bt.endpoints(True, env={})
        with self.assertRaises(SystemExit):
            bt.endpoints(True, env={"AMM_ALLOW_MAINNET": "yes"})
        self.assertEqual(bt.endpoints(True, env={"AMM_ALLOW_MAINNET": bt.MAINNET_ACK})[0], bt.MAINNET_REST)

    def test_missing_keys_are_reported_not_guessed(self):
        with self.assertRaises(SystemExit):
            bt.credentials(env={})

    def test_rest_signs_and_maps_errors(self):
        calls = []

        def transport(method, url, headers, timeout):
            calls.append((method, url, headers))
            if "fail" in url:
                return 400, {}, b'{"code":-2010,"msg":"Order would immediately trigger."}'
            return 200, {"X-MBX-ORDER-COUNT-10S": "7"}, b'{"ok":true}'

        r = bt.Rest("https://x", "KEY", "SECRET", transport=transport, clock=lambda: 1700000000.0)
        r.offset_ms = 250
        self.assertEqual(r.call("POST", "/p", {"a": 1}, signed=True), {"ok": True})
        method, url, headers = calls[-1]
        self.assertEqual(headers["X-MBX-APIKEY"], "KEY")
        q = url.split("?", 1)[1]
        body, sig = q.rsplit("&signature=", 1)
        self.assertEqual(body, "a=1&recvWindow=5000&timestamp=1700000000250")
        self.assertEqual(sig, bt.sign("SECRET", body))
        self.assertEqual(r.order_count_10s, 7)
        with self.assertRaises(bt.ApiError) as e:
            r.call("POST", "/fail", signed=True)
        self.assertEqual((e.exception.status, e.exception.code), (400, -2010))
        r.call("GET", "/public")
        self.assertNotIn("X-MBX-APIKEY", calls[-1][2])  # public calls carry no key


class Orders(unittest.TestCase):
    def test_ids_round_trip_and_fit(self):
        tag = bt.session_tag(1763251200.0)
        self.assertEqual(len(tag), 6)
        big = (1 << 62) | 123456789  # hedge-tagged id
        cid = bt.client_id(tag, big)
        self.assertLessEqual(len(cid), 36)
        self.assertRegex(cid, r"^[A-Za-z0-9_-]{1,36}$")
        self.assertEqual(bt.engine_id(tag, cid), big)
        self.assertIsNone(bt.engine_id("zzzzzz", cid))  # another session's order
        self.assertIsNone(bt.engine_id(tag, "web_123"))

    def test_filters_from_exchange_info(self):
        f = bt.parse_filters(EXCHANGE_INFO, ["BTCUSDT", "ETHUSDT"])
        self.assertEqual(f["BTCUSDT"], {"tick": "0.10", "step": "0.001", "min_qty": "0.001", "min_notional": "100"})
        with self.assertRaises(ValueError):
            bt.parse_filters(EXCHANGE_INFO, ["BTCUSDT", "NOPE"])

    def test_rounding_is_never_more_aggressive(self):
        self.assertEqual(bt.round_price(65000.17, "0.10", "B"), "65000.1")
        self.assertEqual(bt.round_price(65000.11, "0.10", "S"), "65000.2")
        self.assertEqual(bt.round_price(64609.900000000001, "0.10", "B"), "64609.9")
        self.assertEqual(bt.round_price(1816.61, "0.01", "S"), "1816.61")
        self.assertEqual(bt.round_qty(0.0059999, "0.001"), "0.005")
        self.assertEqual(bt.round_qty(2, "0.001"), "2")

    def test_order_params(self):
        p = bt.order_params("BTCUSDT", "B", 65000.17, 0.005, "POST", "cid1", FILTERS)
        self.assertEqual(p, {"symbol": "BTCUSDT", "side": "BUY", "type": "LIMIT", "timeInForce": "GTX",
                             "quantity": "0.005", "price": "65000.1", "newClientOrderId": "cid1",
                             "newOrderRespType": "ACK", "selfTradePreventionMode": "EXPIRE_MAKER"})
        self.assertEqual(bt.order_params("ETHUSDT", "S", 1800, 0.1, "IOC", "c", FILTERS)["timeInForce"], "IOC")
        with self.assertRaises(ValueError):
            bt.order_params("BTCUSDT", "B", 65000, 0.001, "POST", "c", FILTERS)  # 65 USDT < 100 minimum
        with self.assertRaises(ValueError):
            bt.order_params("BTCUSDT", "B", 65000, 0.0004, "POST", "c", FILTERS)  # rounds to zero


class UserStream(unittest.TestCase):
    tag = "abc123"

    def reports(self, msg):
        return bt.reports_from_user_event(msg, self.tag, "BINANCE-PERP:")

    def test_order_lifecycle(self):
        cid = bt.client_id(self.tag, 42)
        self.assertEqual(self.reports(otu(cid, "NEW", "NEW")), [("A", 42)])
        self.assertEqual(self.reports(otu(cid, "TRADE", "PARTIALLY_FILLED", l="0.002", L="65000.1")),
                         [("F", 42, 65000.1, 0.002, False)])
        self.assertEqual(self.reports(otu(cid, "TRADE", "FILLED", l="0.003", L="65000.1", m=False)),
                         [("F", 42, 65000.1, 0.003, True), ("D", 42, "filled")])
        self.assertEqual(self.reports(otu(cid, "CANCELED", "CANCELED")), [("D", 42, "cancelled")])
        self.assertEqual(self.reports(otu(cid, "EXPIRED", "EXPIRED")), [("D", 42, "expired")])  # GTX that would take
        self.assertEqual(self.reports(otu(cid, "EXPIRED", "EXPIRED_IN_MATCH")), [("D", 42, "expired self-trade")])

    def test_other_sessions_and_positions(self):
        self.assertEqual(self.reports(otu("web_1", "NEW", "NEW")), [])
        acct = {"e": "ACCOUNT_UPDATE", "a": {"m": "ORDER", "B": [], "P": [
            {"s": "BTCUSDT", "pa": "0.005", "ep": "65000", "ps": "BOTH"},
            {"s": "ETHUSDT", "pa": "-1.5", "ep": "1800", "ps": "BOTH"}]}}
        self.assertEqual(self.reports(acct), [("P", "BINANCE-PERP:BTCUSDT", 0.005), ("P", "BINANCE-PERP:ETHUSDT", -1.5)])

    def test_report_lines_parse_in_the_engine_format(self):
        self.assertEqual(bt.report_line(5, ("F", 42, 65000.1, 0.002, False)), "5,F,42,65000.1,0.002,M")
        self.assertEqual(bt.report_line(5, ("F", 42, 65000.1, 0.002, True)), "5,F,42,65000.1,0.002,T")
        self.assertEqual(bt.report_line(5, ("D", 42, "rejected -2010, x")), "5,D,42,rejected -2010  x")
        self.assertEqual(bt.report_line(5, ("G", "ready")), "5,G,ready")


class FakeVenue:
    def __init__(self):
        self.calls = []
        self.fail_new = None
        self.fail_cancel = None
        self.query = None
        self.during_new = None

    def new_order(self, p):
        self.calls.append(("new", p["newClientOrderId"]))
        if self.during_new:
            f, self.during_new = self.during_new, None
            f()
        if self.fail_new:
            raise self.fail_new

    def cancel_order(self, sym, cid):
        self.calls.append(("cancel", cid))
        if self.fail_cancel:
            raise self.fail_cancel

    def query_order(self, sym, cid):
        self.calls.append(("query", cid))
        if isinstance(self.query, Exception):
            raise self.query
        return self.query

    def cancel_all(self, sym):
        self.calls.append(("cancel_all", sym))


class GatewayLogic(unittest.TestCase):
    def setUp(self):
        self.lines = []
        self.v = FakeVenue()
        self.g = gwm.Gateway(self.v, ["BTCUSDT", "ETHUSDT"], FILTERS, "t00001", self.lines.append,
                             clock=lambda: 7, synchronous=True)

    def test_new_cancel_and_reports(self):
        self.g.on_order_line("N,5,BINANCE-PERP:BTCUSDT,B,65000.17,0.005,POST")
        cid = bt.client_id("t00001", 5)
        self.assertEqual(self.v.calls, [("new", cid)])
        self.g.on_user_event(otu(cid, "NEW", "NEW"))
        self.g.on_order_line("X,5,BINANCE-PERP:BTCUSDT")
        self.assertEqual(self.v.calls[-1], ("cancel", cid))
        self.g.on_user_event(otu(cid, "CANCELED", "CANCELED"))
        self.assertEqual(self.lines, ["7,A,5", "7,D,5,cancelled"])
        self.g.on_order_line("X,5,BINANCE-PERP:BTCUSDT")  # already done: nothing sent
        self.assertEqual(len(self.v.calls), 2)

    def test_cancel_waits_for_the_order_to_reach_the_venue(self):
        # the engine's cancel arrives while the new-order call is still in flight
        self.v.during_new = lambda: self.g.on_order_line("X,6,BINANCE-PERP:BTCUSDT")
        self.g.on_order_line("N,6,BINANCE-PERP:BTCUSDT,S,65010,0.005,POST")
        cid = bt.client_id("t00001", 6)
        self.assertEqual(self.v.calls, [("new", cid), ("cancel", cid)])  # sent after, never before

    def test_refusals(self):
        self.v.fail_new = bt.ApiError(400, -2010, "would trigger")
        self.g.on_order_line("N,7,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        self.g.on_order_line("N,8,BINANCE-PERP:BTCUSDT,B,65000,0.001,POST")  # below min notional: not sent
        self.g.on_order_line("N,9,BINANCE-PERP:XRPUSDT,B,1,10,POST")
        self.assertEqual(self.lines, ["7,D,7,rejected -2010", "7,D,8,rejected notional below minimum",
                                      "7,D,9,rejected unknown symbol"])
        self.assertEqual(len(self.v.calls), 1)

    def test_rate_limit_pauses_new_orders(self):
        self.v.fail_new = bt.ApiError(429, -1015, "Too many new orders", {"retry-after": "30"})
        self.g.on_order_line("N,1,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        self.v.fail_new = None
        self.g.on_order_line("N,2,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        self.assertEqual(self.lines[-1], "7,D,2,rejected rate limited")
        self.assertEqual(len(self.v.calls), 1)

    def test_kill_cancels_everything_and_refuses_more(self):
        self.g.on_order_line("K,position mismatch")
        self.assertEqual(sorted(self.v.calls), [("cancel_all", "BTCUSDT"), ("cancel_all", "ETHUSDT")])
        self.g.on_order_line("N,3,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        self.assertEqual(self.lines, ["7,D,3,rejected killed"])

    def test_unknown_outcome_is_resolved_by_asking(self):
        gwm.time.sleep, real = (lambda s: None), gwm.time.sleep
        try:
            self.v.fail_new = TimeoutError("timed out")
            self.v.query = bt.ApiError(400, -2013, "Order does not exist.")
            self.g.on_order_line("N,4,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
            self.assertEqual(self.lines, ["7,D,4,rejected not received"])
            self.v.query = {"status": "NEW"}
            self.g.on_order_line("N,5,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
            self.assertIn(5, self.g.orders)  # it got there: live, the stream will report it
            self.assertEqual(self.g.orders[5]["state"], "live")
        finally:
            gwm.time.sleep = real

    def test_cancel_of_an_order_that_just_filled_is_quiet(self):
        self.g.on_order_line("N,5,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        self.v.fail_cancel = bt.ApiError(400, -2011, "Unknown order sent.")
        self.g.on_order_line("X,5,BINANCE-PERP:BTCUSDT")
        self.assertEqual(self.lines, [])

    def test_dry_venue_round_trip(self):
        lines = []
        box = {}
        dv = gwm.DryVenue(lambda m: box["g"].on_user_event(m))
        g = gwm.Gateway(dv, ["BTCUSDT"], FILTERS, "t00002", lines.append, clock=lambda: 1, synchronous=True)
        box["g"] = g
        g.on_order_line("N,1,BINANCE-PERP:BTCUSDT,B,65000,0.005,POST")
        g.on_order_line("N,2,BINANCE-PERP:BTCUSDT,S,65000,0.005,IOC")
        g.on_order_line("X,1,BINANCE-PERP:BTCUSDT")
        self.assertEqual(lines, ["1,A,1", "1,D,2,expired", "1,D,1,cancelled"])


class Reconciliation(unittest.TestCase):
    MAP = {"BTC-USDT-PERP": "BTCUSDT", "ETH-USDT-PERP": "ETHUSDT"}

    def test_instrument_map_from_config(self):
        self.assertEqual(rc.instrument_map(os.path.join(ROOT, "config", "binance_perp.ini")), self.MAP)

    def test_drop_copy_book(self):
        b = rc.DropCopyBook()
        b.apply({"snapshot": {"positions": {"BTCUSDT": 0.01}, "open": []}})
        b.apply({"msg": otu("ammx-5", "NEW", "NEW")})
        b.apply({"msg": otu("ammx-5", "TRADE", "PARTIALLY_FILLED", l="0.002", L="65000")})
        self.assertAlmostEqual(b.positions["BTCUSDT"], 0.012)
        self.assertEqual(b.open, {"ammx-5"})
        b.apply({"msg": otu("ammx-6", "TRADE", "FILLED", side="SELL", l="0.005", L="65000")})
        self.assertAlmostEqual(b.positions["BTCUSDT"], 0.007)
        b.apply({"msg": {"e": "ACCOUNT_UPDATE", "a": {"P": [{"s": "BTCUSDT", "pa": "0.0071", "ps": "BOTH"}]}}})
        self.assertAlmostEqual(b.positions["BTCUSDT"], 0.0071)  # the venue's figure wins
        b.apply({"msg": otu("ammx-5", "CANCELED", "CANCELED")})
        self.assertEqual(b.open, set())

    def engine(self, ts, btc=0.0, open_ids=()):
        return {"ts": ts, "positions": {"BTC-USDT-PERP": btc, "ETH-USDT-PERP": 0.0},
                "open": [{"id": i, "acked": True, "left": 0.005} for i in open_ids]}

    def test_short_differences_are_tolerated_lasting_ones_are_not(self):
        r = rc.Reconciler(self.MAP, grace=5.0, stale=10.0)
        b = rc.DropCopyBook()
        b.apply({"snapshot": {"positions": {"BTCUSDT": 0.0}, "open": []}})
        r.set_engine(self.engine(1), 100.0)
        self.assertEqual(r.breaches(b, 100.0), [])
        b.apply({"msg": otu("ammx-a", "TRADE", "FILLED", l="0.005", L="65000")})  # fill not yet seen by engine
        self.assertEqual(r.breaches(b, 101.0), [])
        r.set_engine(self.engine(2, btc=0.005), 102.0)  # engine catches up within grace
        self.assertEqual(r.breaches(b, 102.0), [])
        b.apply({"msg": otu("ammx-b", "TRADE", "FILLED", l="0.005", L="65000")})  # a fill the engine never gets
        r.set_engine(self.engine(3, btc=0.005), 103.0)
        self.assertEqual(r.breaches(b, 104.0), [])
        r.set_engine(self.engine(4, btc=0.005), 108.0)
        self.assertEqual(r.breaches(b, 108.5), [])  # first seen at 104: 4.5 s, inside the grace
        out = r.breaches(b, 109.5)
        self.assertEqual(len(out), 1)
        self.assertIn("BTCUSDT position: engine 0.005, drop copy 0.01", out[0])

    def test_orphan_and_ghost_orders(self):
        r = rc.Reconciler(self.MAP, grace=0.0)
        b = rc.DropCopyBook()
        b.apply({"snapshot": {"positions": {}, "open": ["ammt-a", "web_1"]}})  # web_1: not ours, ignored
        r.set_engine(self.engine(1, open_ids=[11]), 1.0)
        out = r.breaches(b, 1.0)
        self.assertIn("order 10 open at the venue, unknown to the engine", out)
        self.assertIn("order 11 resting in the engine, not open at the venue", out)

    def test_venue_poll_checks_the_drop_copy(self):
        r = rc.Reconciler(self.MAP, grace=0.0)
        b = rc.DropCopyBook()
        b.apply({"snapshot": {"positions": {"BTCUSDT": 0.0}, "open": []}})
        r.set_venue({"BTCUSDT": 0.003}, ["ammt-1"])
        out = r.breaches(b, 1.0)
        self.assertTrue(any("venue 0.003, drop copy 0" in x for x in out))
        self.assertTrue(any("missing from the drop copy" in x for x in out))

    def test_a_hung_engine_trips_at_once(self):
        r = rc.Reconciler(self.MAP, grace=5.0, stale=10.0)
        b = rc.DropCopyBook()
        r.set_engine(self.engine(1), 0.0)
        r.set_engine(self.engine(1), 11.0)  # same snapshot again: no progress
        self.assertEqual(r.breaches(b, 11.0), ["engine state not updated for 11s"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
