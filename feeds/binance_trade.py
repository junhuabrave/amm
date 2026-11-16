"""Binance USD-M futures trading plumbing shared by the gateway, drop copy and reconciler.

Pure functions where possible (signing, rounding, id mapping, parsing of
user-data messages) so they are tested offline; `Rest` takes an injectable
transport for the same reason.

Endpoints (USD-M futures):
  testnet  https://testnet.binancefuture.com        wss://fstream.binancefuture.com/ws/<listenKey>
  mainnet  https://fapi.binance.com                 wss://fstream.binance.com/ws/<listenKey>

Credentials come from the environment (BINANCE_API_KEY, BINANCE_API_SECRET)
and are never printed. Mainnet needs --mainnet and AMM_ALLOW_MAINNET set to
MAINNET_REAL_MONEY: two separate deliberate acts.
"""
import decimal
import hashlib
import hmac
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

import common

TESTNET_REST = "https://testnet.binancefuture.com"
TESTNET_WS = "wss://fstream.binancefuture.com/ws/"
MAINNET_REST = "https://fapi.binance.com"
MAINNET_WS = "wss://fstream.binance.com/ws/"
MAINNET_ACK = "MAINNET_REAL_MONEY"


def endpoints(mainnet=False, env=os.environ):
    """(rest, ws) bases; refuses mainnet unless both deliberate switches are set."""
    if not mainnet:
        return TESTNET_REST, TESTNET_WS
    if env.get("AMM_ALLOW_MAINNET") != MAINNET_ACK:
        raise SystemExit("refusing mainnet: set AMM_ALLOW_MAINNET=%s as well as --mainnet" % MAINNET_ACK)
    return MAINNET_REST, MAINNET_WS


def credentials(env=os.environ):
    key, secret = env.get("BINANCE_API_KEY"), env.get("BINANCE_API_SECRET")
    if not key or not secret:
        raise SystemExit("set BINANCE_API_KEY and BINANCE_API_SECRET (testnet keys from testnet.binancefuture.com)")
    return key, secret


def sign(secret, query):
    """HMAC-SHA256 hex signature of the query string, as Binance requires."""
    return hmac.new(secret.encode(), query.encode(), hashlib.sha256).hexdigest()


# ---- order ids --------------------------------------------------------------

def session_tag(now=None):
    """Short, per-process prefix so ids from a previous run are never reused."""
    n = int((now if now is not None else time.time()) * 10) % (36 ** 6)
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    s = ""
    for _ in range(6):
        s = digits[n % 36] + s
        n //= 36
    return s


def client_id(tag, engine_id):
    """Engine order id -> Binance newClientOrderId (<= 36 chars of [A-Za-z0-9_-])."""
    return "amm%s-%x" % (tag, int(engine_id))


def engine_id(tag, cid):
    """Inverse of client_id; None for ids this session did not create."""
    p = "amm%s-" % tag
    if not cid or not cid.startswith(p):
        return None
    try:
        return int(cid[len(p):], 16)
    except ValueError:
        return None


# ---- exchange filters -------------------------------------------------------

def parse_filters(exchange_info, symbols):
    """{symbol: {tick, step, min_qty, min_notional}} from /fapi/v1/exchangeInfo."""
    out = {}
    for s in exchange_info.get("symbols", []):
        if s.get("symbol") not in symbols:
            continue
        f = {"tick": None, "step": None, "min_qty": "0", "min_notional": "0"}
        for flt in s.get("filters", []):
            t = flt.get("filterType")
            if t == "PRICE_FILTER":
                f["tick"] = flt["tickSize"]
            elif t == "LOT_SIZE":
                f["step"], f["min_qty"] = flt["stepSize"], flt["minQty"]
            elif t == "MIN_NOTIONAL":
                f["min_notional"] = flt.get("notional", flt.get("minNotional", "0"))
        out[s["symbol"]] = f
    missing = [s for s in symbols if s not in out]
    if missing:
        raise ValueError("symbols not listed by the exchange: %s" % ", ".join(missing))
    return out


def _fmt(d):
    s = format(d.normalize(), "f")
    return s if s not in ("-0", "") else "0"


def round_price(px, tick, side):
    """Round to the tick, never more aggressively than asked (buys down, sells up)."""
    t = decimal.Decimal(tick)
    q = decimal.Decimal(repr(float(px))) / t
    q = q.to_integral_value(decimal.ROUND_FLOOR if side == "B" else decimal.ROUND_CEILING)
    return _fmt(q * t)


def round_qty(qty, step):
    """Round down to the step (never send more than the engine asked for)."""
    t = decimal.Decimal(step)
    q = (decimal.Decimal(repr(float(qty))) / t).to_integral_value(decimal.ROUND_FLOOR)
    return _fmt(q * t)


def order_params(sym, side, px, qty, tif, cid, filters):
    """Parameters for POST /fapi/v1/order, or raise ValueError if the order cannot be sent."""
    f = filters[sym]
    p = round_price(px, f["tick"], side)
    q = round_qty(qty, f["step"])
    if decimal.Decimal(q) < decimal.Decimal(f["min_qty"]) or decimal.Decimal(q) <= 0:
        raise ValueError("quantity below minimum")
    if decimal.Decimal(q) * decimal.Decimal(p) < decimal.Decimal(f["min_notional"]):
        raise ValueError("notional below minimum")
    return {
        "symbol": sym,
        "side": "BUY" if side == "B" else "SELL",
        "type": "LIMIT",
        # GTX = post-only: the venue rejects (expires) it rather than let it take liquidity
        "timeInForce": "GTX" if tif == "POST" else "IOC",
        "quantity": q,
        "price": p,
        "newClientOrderId": cid,
        "newOrderRespType": "ACK",
        # never trade with ourselves: our resting order is cancelled instead
        "selfTradePreventionMode": "EXPIRE_MAKER",
    }


# ---- user-data stream -------------------------------------------------------

def reports_from_user_event(msg, tag, prefix):
    """One user-data message -> list of report tuples for the engine.

    ('A', id) accepted, ('F', id, px, qty) fill, ('D', id, reason) done,
    ('P', symbol, qty) position. Orders from other sessions are skipped.
    """
    e = msg.get("e")
    out = []
    if e == "ORDER_TRADE_UPDATE":
        o = msg["o"]
        eid = engine_id(tag, o.get("c"))
        if eid is None:
            return out
        x, status = o.get("x"), o.get("X")
        if x == "NEW":
            out.append(("A", eid))
        elif x == "TRADE":
            out.append(("F", eid, float(o["L"]), float(o["l"])))
            if status == "FILLED":
                out.append(("D", eid, "filled"))
        elif x in ("CANCELED", "EXPIRED", "CALCULATED") or status in ("CANCELED", "EXPIRED", "EXPIRED_IN_MATCH"):
            reason = {"CANCELED": "cancelled", "EXPIRED": "expired", "CALCULATED": "liquidated"}.get(x, "expired")
            if status == "EXPIRED_IN_MATCH":
                reason = "expired self-trade"
            out.append(("D", eid, reason))
    elif e == "ACCOUNT_UPDATE":
        for p in msg.get("a", {}).get("P", []):
            if p.get("ps", "BOTH") == "BOTH":
                out.append(("P", prefix + p["s"], float(p["pa"])))
    return out


def report_line(ts_ns, r):
    if r[0] == "A":
        return "%d,A,%d" % (ts_ns, r[1])
    if r[0] == "F":
        return "%d,F,%d,%r,%r" % (ts_ns, r[1], r[2], r[3])
    if r[0] == "D":
        return "%d,D,%d,%s" % (ts_ns, r[1], r[2].replace(",", " "))
    if r[0] == "P":
        return "%d,P,%s,%r" % (ts_ns, r[1], r[2])
    if r[0] == "G":
        return "%d,G,%s" % (ts_ns, r[1])
    raise ValueError(r)


# ---- REST -------------------------------------------------------------------

class ApiError(Exception):
    def __init__(self, status, code, msg, headers=None):
        super().__init__("HTTP %s code %s: %s" % (status, code, msg))
        self.status, self.code, self.msg, self.headers = status, code, msg, headers or {}


def urllib_transport(method, url, headers, timeout):
    """(status, headers, body bytes); HTTP errors are returned, not raised."""
    req = urllib.request.Request(url, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=common.ssl_context()) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read()


class Rest:
    """Signed REST client. `transport(method, url, headers, timeout)` is injectable."""

    def __init__(self, base, key=None, secret=None, transport=urllib_transport, clock=time.time, recv_window=5000):
        self.base, self.key, self.secret = base, key, secret
        self.transport, self.clock, self.recv_window = transport, clock, recv_window
        self.offset_ms = 0
        self.order_count_10s = None

    def call(self, method, path, params=None, signed=False, key_only=False, timeout=10.0):
        params = dict(params or {})
        headers = {"User-Agent": common.USER_AGENT}
        if signed or key_only:
            headers["X-MBX-APIKEY"] = self.key
        if signed:
            params["recvWindow"] = self.recv_window
            params["timestamp"] = int(self.clock() * 1000) + self.offset_ms
        q = urllib.parse.urlencode(params)
        if signed:
            q += "&signature=" + sign(self.secret, q)
        url = self.base + path + ("?" + q if q else "")
        status, hdrs, body = self.transport(method, url, headers, timeout)
        h = {k.lower(): v for k, v in (hdrs or {}).items()}
        if "x-mbx-order-count-10s" in h:
            self.order_count_10s = int(h["x-mbx-order-count-10s"])
        try:
            data = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            data = {"msg": body[:200].decode("utf-8", "replace")}
        if status >= 400:
            raise ApiError(status, data.get("code") if isinstance(data, dict) else None,
                           data.get("msg") if isinstance(data, dict) else str(data), h)
        return data

    def sync_time(self):
        t0 = self.clock()
        server = self.call("GET", "/fapi/v1/time")["serverTime"]
        t1 = self.clock()
        self.offset_ms = int(server - (t0 + t1) / 2 * 1000)
        return self.offset_ms

    # thin wrappers
    def exchange_info(self):
        return self.call("GET", "/fapi/v1/exchangeInfo")

    def new_order(self, params):
        return self.call("POST", "/fapi/v1/order", params, signed=True)

    def cancel_order(self, symbol, cid):
        return self.call("DELETE", "/fapi/v1/order", {"symbol": symbol, "origClientOrderId": cid}, signed=True)

    def query_order(self, symbol, cid):
        return self.call("GET", "/fapi/v1/order", {"symbol": symbol, "origClientOrderId": cid}, signed=True)

    def cancel_all(self, symbol):
        return self.call("DELETE", "/fapi/v1/allOpenOrders", {"symbol": symbol}, signed=True)

    def open_orders(self, symbol=None):
        return self.call("GET", "/fapi/v1/openOrders", {"symbol": symbol} if symbol else {}, signed=True)

    def positions(self):
        """{symbol: net position} for one-way (BOTH) positions."""
        out = {}
        for p in self.call("GET", "/fapi/v2/positionRisk", signed=True):
            if p.get("positionSide", "BOTH") == "BOTH":
                out[p["symbol"]] = float(p["positionAmt"])
        return out

    def listen_key(self):
        return self.call("POST", "/fapi/v1/listenKey", key_only=True)["listenKey"]

    def keepalive(self):
        return self.call("PUT", "/fapi/v1/listenKey", key_only=True)

    def close_listen_key(self):
        return self.call("DELETE", "/fapi/v1/listenKey", key_only=True)
