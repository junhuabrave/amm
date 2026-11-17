#!/usr/bin/env python3
"""Bybit public market data over websockets (USDT linear perpetuals) as normalised events.

    python3 feeds/binance_ws.py --perp BTCUSDT --bybit-perp BTCUSDT | ./build/amm_run ...

Used through feeds/binance_ws.py (--bybit-perp), so Binance and Bybit share one
process, one receipt clock and one ordered output stream: what a cross-venue
lead-lag measurement needs.

Stream: wss://stream.bybit.com/v5/public/linear, one connection, subscribed
after the handshake to
  orderbook.1.SYMBOL   top of book, pushed on change (a snapshot, then deltas;
                       a fresh snapshot replaces the state)
  publicTrade.SYMBOL   every trade, with the taker side
Bybit closes connections that send nothing, so the client pings ({"op":
"ping"}) every 20 s. Symbols are written as BYBIT-PERP:SYMBOL.

Nothing is sent to the venue but the handshake, the subscription and pings:
no account, no key, no orders.
"""
import json

BYBIT_LINEAR_WS = "wss://stream.bybit.com/v5/public/linear"
PREFIX = "BYBIT-PERP:"
MAX_ARGS_PER_SUBSCRIBE = 10  # Bybit's limit per subscribe request


class BybitPerp:
    """One Bybit linear-perpetual connection: subscription, keep-alive and parsing."""

    ping_every = 20.0

    def __init__(self, symbols, url=BYBIT_LINEAR_WS, prefix=PREFIX):
        self.symbols = [s.upper() for s in symbols]
        self.url = url
        self.prefix = prefix
        self.top = {}  # symbol -> [bid px, bid qty, ask px, ask qty] (None while unknown)

    def subscriptions(self):
        args = []
        for s in self.symbols:
            args += ["orderbook.1." + s, "publicTrade." + s]
        return [json.dumps({"op": "subscribe", "args": args[i:i + MAX_ARGS_PER_SUBSCRIBE]})
                for i in range(0, len(args), MAX_ARGS_PER_SUBSCRIBE)]

    def on_open(self, conn):
        self.top = {}  # a new connection starts from fresh snapshots
        for m in self.subscriptions():
            conn.send_text(m)

    def ping(self, conn):
        conn.send_text('{"op":"ping"}')

    def events(self, msg):
        """One message -> [(event tuple, kind, exchange time ms)]."""
        m = json.loads(msg)
        topic = m.get("topic", "")
        if topic.startswith("orderbook."):
            return self._book(m)
        if topic.startswith("publicTrade."):
            out = []
            for t in m.get("data", []):
                side = "B" if t["S"] == "Buy" else "S"
                out.append((("T", self.prefix + t["s"], float(t["p"]), float(t["v"]), side), "publicTrade", t.get("T")))
            return out
        return []  # subscription acks, pongs

    def _book(self, m):
        d = m["data"]
        sym = d["s"]
        if m.get("type") == "snapshot" or sym not in self.top:
            self.top[sym] = [None, None, None, None]
        t = self.top[sym]
        for side, k in (("b", 0), ("a", 2)):
            for px, qty in d.get(side, []):
                px, qty = float(px), float(qty)
                if qty > 0:
                    t[k], t[k + 1] = px, qty
                elif t[k] == px:  # the best level was removed; the new best comes in the same push
                    t[k] = t[k + 1] = None
        b, bs, a, a_s = t
        if b is None or a is None or not (a > b > 0):
            return []
        return [(("B", self.prefix + sym, b, bs, a, a_s), "orderbook.1", m.get("cts") or m.get("ts"))]
