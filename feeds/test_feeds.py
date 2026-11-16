"""Offline tests of the adapters against payloads shaped like the venues' documented responses.

    python3 feeds/test_feeds.py
"""
import io
import unittest

import json
import socket
import struct

import binance
import binance_ws
import coinbase
import common
import ws

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


# Responses captured from the live endpoints on 2026-11-15 (trimmed to a few rows).
REAL_BINANCE_SPOT_DEPTH = {"lastUpdateId": 102929564974,
    "bids": [["65439.05000000", "4.91823000"], ["65439.01000000", "0.00018000"]],
    "asks": [["65439.06000000", "0.22579000"], ["65439.07000000", "0.00072000"]]}
REAL_BINANCE_SPOT_TRADES = [
    {"id": 6864031334, "price": "65460.01000000", "qty": "0.03819000", "quoteQty": "2499.91778190", "time": 1794749649016, "isBuyerMaker": False, "isBestMatch": True},
    {"id": 6864031335, "price": "65460.01000000", "qty": "0.04962000", "quoteQty": "3248.12569620", "time": 1794749649016, "isBuyerMaker": False, "isBestMatch": True}]
REAL_BINANCE_PERP_DEPTH = {"lastUpdateId": 12123019959927, "E": 1794749666232, "T": 1794749666228,
    "bids": [["65460.50", "5.772"], ["65460.40", "0.032"]], "asks": [["65460.60", "0.162"], ["65460.70", "0.003"]]}
REAL_BINANCE_PERP_TRADES = [
    {"id": 8279844435, "price": "65429.50", "qty": "0.001", "quoteQty": "65.42", "time": 1794749682978, "isBuyerMaker": True, "isRPITrade": False},
    {"id": 8279844436, "price": "65429.60", "qty": "0.137", "quoteQty": "8963.85", "time": 1794749683272, "isBuyerMaker": False, "isRPITrade": False}]
REAL_COINBASE_BOOK = {"bids": [["65416.68", "0.02527264", 3]], "asks": [["65416.69", "0.07311991", 3]],
    "sequence": 139485979677, "auction_mode": False, "auction": None, "time": "2026-11-15T13:35:17.914307550Z"}
REAL_COINBASE_TRADES = [
    {"trade_id": 1128610863, "side": "buy", "size": "0.00070487", "price": "65435.31000000", "time": "2026-11-15T13:35:31.288307Z"},
    {"trade_id": 1128610861, "side": "sell", "size": "0.00068021", "price": "65435.32000000", "time": "2026-11-15T13:35:31.190746Z"}]


class RealPayloads(unittest.TestCase):
    def test_binance_spot(self):
        self.assertEqual(binance.parse_depth(REAL_BINANCE_SPOT_DEPTH), (65439.05, 4.91823, 65439.06, 0.22579))
        new, last = binance.parse_trades(REAL_BINANCE_SPOT_TRADES, 6864031333)
        self.assertEqual(new, [(65460.01, 0.03819, "B"), (65460.01, 0.04962, "B")])
        self.assertEqual(last, 6864031335)

    def test_binance_perp(self):
        self.assertEqual(binance.parse_depth(REAL_BINANCE_PERP_DEPTH), (65460.5, 5.772, 65460.6, 0.162))
        new, _ = binance.parse_trades(REAL_BINANCE_PERP_TRADES, 0)
        self.assertEqual(new, [(65429.5, 0.001, "S"), (65429.6, 0.137, "B")])

    def test_coinbase(self):
        self.assertEqual(coinbase.parse_book(REAL_COINBASE_BOOK), (65416.68, 0.02527264, 65416.69, 0.07311991))
        new, last = coinbase.parse_trades(REAL_COINBASE_TRADES, 0)
        # The up-tick (65435.32, maker "sell") was a buyer lifting the offer; the later print at 65435.31 hit the bid.
        self.assertEqual(new, [(65435.32, 0.00068021, "B"), (65435.31, 0.00070487, "S")])
        self.assertEqual(last, 1128610863)

    def test_full_poll_produces_lines_the_runner_accepts(self):
        def fetch(url):
            return REAL_BINANCE_PERP_DEPTH if "/depth" in url else REAL_BINANCE_PERP_TRADES
        s = binance.BinanceSource("BTCUSDT", perp=True, fetch=fetch)
        s.poll()
        s.last_id = 8279844435
        lines = [common.line(1794749683500000000, ev[0], ev[1], *ev[2:]) for ev in s.poll()]
        self.assertEqual(lines, ["1794749683500000000,T,BINANCE-PERP:BTCUSDT,65429.6,0.137,B",
                                 "1794749683500000000,B,BINANCE-PERP:BTCUSDT,65460.5,5.772,65460.6,0.162"])


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


# Websocket messages captured from the live Binance streams on 2026-11-15.
REAL_WS_PERP_BOOK = '{"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":12123442506725,"s":"BTCUSDT","ps":"BTCUSDT","b":"64725.80","B":"0.776","a":"64725.90","A":"1.647","T":1794753590558,"E":1794753590558,"st":1}}'
REAL_WS_PERP_TRADE = '{"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1794753591559,"a":3526044535,"s":"BTCUSDT","p":"64730.80","q":"0.028","nq":"0.028","f":8280153052,"l":8280153053,"T":1794753591449,"m":true,"st":1}}'
REAL_WS_SPOT_BOOK = '{"stream":"btcusdt@bookTicker","data":{"u":102932694632,"s":"BTCUSDT","b":"64756.54000000","B":"3.61760000","a":"64756.55000000","A":"0.00875000"}}'
REAL_WS_SPOT_TRADE = '{"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1794753594557,"s":"BTCUSDT","a":4118908727,"p":"64756.55000000","q":"0.00020000","f":6864373052,"l":6864373052,"T":1794753594557,"m":false,"M":true}}'


def server_frame(opcode, payload, fin=True):
    """Unmasked server-to-client frame."""
    n = len(payload)
    head = bytes([(0x80 if fin else 0) | opcode])
    if n < 126:
        head += bytes([n])
    elif n < 1 << 16:
        head += bytes([126]) + struct.pack("!H", n)
    else:
        head += bytes([127]) + struct.pack("!Q", n)
    return head + payload


class WebsocketCodec(unittest.TestCase):
    def test_accept_key_matches_rfc_example(self):
        self.assertEqual(ws.accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_client_frames_are_masked_and_decode_back(self):
        for n in (0, 5, 125, 126, 65535, 65536, 200000):
            payload = bytes(range(256)) * (n // 256) + bytes(range(n % 256))
            f = ws.encode_frame(ws.OP_TEXT, payload, mask_key=b"\x01\x02\x03\x04")
            self.assertTrue(f[1] & 0x80)  # mask bit
            self.assertEqual(ws.FrameDecoder().feed(f), [(ws.OP_TEXT, payload)])

    def test_rfc_masked_hello(self):
        # RFC 6455 section 5.7: a masked "Hello"
        self.assertEqual(ws.encode_frame(ws.OP_TEXT, "Hello", mask_key=bytes.fromhex("37fa213d")),
                         bytes.fromhex("818537fa213d7f9f4d5158"))

    def test_decoder_reassembles_fragments_split_byte_by_byte(self):
        stream = (server_frame(ws.OP_TEXT, b"Hel", fin=False) + server_frame(ws.OP_PING, b"x")
                  + server_frame(ws.OP_CONT, b"lo", fin=True) + server_frame(ws.OP_BINARY, b"\x00" * 300))
        d, out = ws.FrameDecoder(), []
        for i in range(len(stream)):
            out += d.feed(stream[i:i + 1])
        self.assertEqual(out, [(ws.OP_PING, b"x"), (ws.OP_TEXT, b"Hello"), (ws.OP_BINARY, b"\x00" * 300)])

    def test_decoder_rejects_protocol_errors(self):
        for bad in (server_frame(ws.OP_CONT, b"x"),                  # continuation with nothing to continue
                    server_frame(ws.OP_PING, b"x", fin=False),       # fragmented control frame
                    bytes([0xC1, 0x00])):                            # reserved bit (compression not negotiated)
            with self.assertRaises(ws.ProtocolError):
                ws.FrameDecoder().feed(bad)


class FakeServerSocket:
    """Stands in for the TLS socket: serves a canned handshake reply, then frames."""

    def __init__(self, frames):
        self.a, self.b = socket.socketpair()
        self.frames = frames
        self.sent = b""

    def __call__(self, addr, timeout):
        return self

    def sendall(self, data):
        self.sent += data
        if data.startswith(b"GET "):
            key = [l.split(b": ")[1] for l in data.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0].decode()
            self.b.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n\r\n" % ws.accept_key(key)).encode() + self.frames)

    def recv(self, n):
        return self.a.recv(n)

    def setblocking(self, flag):
        self.a.setblocking(flag)

    def fileno(self):
        return self.a.fileno()

    def close(self):
        self.a.close()
        self.b.close()


class WebsocketConnection(unittest.TestCase):
    def test_handshake_messages_ping_and_close(self):
        frames = (server_frame(ws.OP_TEXT, REAL_WS_PERP_BOOK.encode()) + server_frame(ws.OP_PING, b"hb")
                  + server_frame(ws.OP_TEXT, REAL_WS_PERP_TRADE.encode()))
        fake = FakeServerSocket(frames)
        c = ws.WsConnection("ws://example.test/stream?streams=btcusdt@bookTicker", sock_factory=fake)
        self.assertIn(b"GET /stream?streams=btcusdt@bookTicker HTTP/1.1", fake.sent)
        self.assertEqual(c.read(), [REAL_WS_PERP_BOOK, REAL_WS_PERP_TRADE])
        pong = ws.FrameDecoder().feed(fake.sent[fake.sent.index(b"\r\n\r\n") + 4:])
        self.assertEqual(pong, [(ws.OP_PONG, b"hb")])  # answered the ping with its payload
        fake.b.sendall(server_frame(ws.OP_CLOSE, struct.pack("!H", 1001) + b"going away"))
        with self.assertRaises(ws.ConnectionClosed) as cm:
            c.read()
        self.assertIn("1001", str(cm.exception))

    def test_handshake_with_wrong_accept_key_is_refused(self):
        fake = FakeServerSocket(b"")
        fake.sendall = lambda data: fake.b.sendall(b"HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: nope\r\n\r\n")
        with self.assertRaises(ws.ProtocolError):
            ws.WsConnection("ws://example.test/x", sock_factory=fake)


class BinanceWebsocket(unittest.TestCase):
    def test_real_messages(self):
        ev = [binance_ws.to_event(m, p)[0] for m, p in ((REAL_WS_PERP_BOOK, "BINANCE-PERP:"), (REAL_WS_PERP_TRADE, "BINANCE-PERP:"),
                                                        (REAL_WS_SPOT_BOOK, "BINANCE:"), (REAL_WS_SPOT_TRADE, "BINANCE:"))]
        self.assertEqual(ev, [("B", "BINANCE-PERP:BTCUSDT", 64725.8, 0.776, 64725.9, 1.647),
                              ("T", "BINANCE-PERP:BTCUSDT", 64730.8, 0.028, "S"),   # buyer was maker: aggressor sold
                              ("B", "BINANCE:BTCUSDT", 64756.54, 3.6176, 64756.55, 0.00875),
                              ("T", "BINANCE:BTCUSDT", 64756.55, 0.0002, "B")])
        self.assertEqual(binance_ws.to_event(REAL_WS_PERP_TRADE, "X:")[2], 1794753591559)  # exchange time for latency stats
        self.assertIsNone(binance_ws.to_event(REAL_WS_SPOT_BOOK, "X:")[2])                 # spot bookTicker carries none

    def test_lines_match_the_rest_adapter_and_the_runner_format(self):
        ev, _, _ = binance_ws.to_event(REAL_WS_PERP_BOOK, "BINANCE-PERP:")
        self.assertEqual(common.line(1794753590600000000, ev[0], ev[1], *ev[2:]),
                         "1794753590600000000,B,BINANCE-PERP:BTCUSDT,64725.8,0.776,64725.9,1.647")

    def test_acks_and_bad_books_are_skipped(self):
        self.assertIsNone(binance_ws.to_event('{"result":null,"id":1}', "X:")[0])
        crossed = json.loads(REAL_WS_PERP_BOOK)
        crossed["data"]["a"] = "64725.70"
        self.assertIsNone(binance_ws.to_event(json.dumps(crossed), "X:")[0])

    def test_stream_urls(self):
        u = binance_ws.stream_urls(["BTCUSDT"], ["btcusdt", "ETHUSDT"])
        self.assertEqual(u, [
            ("wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@aggTrade", "BINANCE:"),
            ("wss://fstream.binance.com/public/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker", "BINANCE-PERP:"),
            ("wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/ethusdt@aggTrade", "BINANCE-PERP:")])
        legacy = "wss://fstream.binance.com/stream"
        self.assertEqual(binance_ws.stream_urls([], ["BTCUSDT"], perp_book_url=legacy, perp_trade_url=legacy),
                         [(legacy + "?streams=btcusdt@bookTicker/btcusdt@aggTrade", "BINANCE-PERP:")])
        many = binance_ws.stream_urls([], ["S%d" % i for i in range(250)])
        self.assertEqual(len(many), 4)  # 250 book + 250 trade streams, at most 200 per connection

    def test_run_reconnects_a_dropped_connection_and_keeps_the_other(self):
        t = [0]

        def clock():
            t[0] += 50_000_000  # 50 ms per call
            return t[0]

        class Conn:
            def __init__(self, url, script):
                self.a, self.b = socket.socketpair()
                self.b.send(b"x")  # always readable
                self.script = script
                self.url = url

            def fileno(self):
                return self.a.fileno()

            def read(self):
                if not self.script:
                    return []
                step = self.script.pop(0)
                if isinstance(step, Exception):
                    raise step
                return step

            def close(self):
                self.a.close()
                self.b.close()

        made = []

        def connect(url):
            n = sum(1 for u in made if u == url)
            made.append(url)
            if "book" in url:
                return Conn(url, [[REAL_WS_PERP_BOOK]] + ([ws.ConnectionClosed("server closed (1001 )")] if n == 0 else []))
            return Conn(url, [[REAL_WS_PERP_TRADE]] * 3)

        out, err = io.StringIO(), io.StringIO()
        specs = [("wss://x/book", "BINANCE-PERP:"), ("wss://x/trade", "BINANCE-PERP:")]
        binance_ws.run(specs, duration=3.0, stats_every=0, out=out, err=err, connect=connect, clock=clock)
        self.assertEqual(made.count("wss://x/book"), 2)   # dropped once, reconnected
        self.assertEqual(made.count("wss://x/trade"), 1)  # untouched
        self.assertIn("reconnect in 1s", err.getvalue())
        lines = out.getvalue().splitlines()
        self.assertEqual(sum(l.split(",")[1] == "B" for l in lines), 2)
        self.assertEqual(sum(l.split(",")[1] == "T" for l in lines), 3)
        stamps = [int(l.split(",")[0]) for l in lines]
        self.assertEqual(stamps, sorted(stamps))

    def test_tls_context_always_verifies(self):
        ctx = common.ssl_context()
        import ssl
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(ctx.check_hostname)
        self.assertGreater(ctx.cert_store_stats().get("x509_ca", 0), 0)


if __name__ == "__main__":
    unittest.main()
