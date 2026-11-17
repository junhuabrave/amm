"""Minimal websocket client (RFC 6455), standard library only.

Enough for public market-data streams: TLS, the opening handshake, masked
client frames, fragmented and large server frames, ping/pong and close. No
extensions (permessage-deflate is not requested, so servers send plain frames).

The frame codec is pure (`encode_frame`, `FrameDecoder`) so it can be tested
without a socket; `WsConnection` is non-blocking and meant to sit in a
`selectors` loop next to other connections.
"""
import base64
import hashlib
import os
import socket
import ssl
import struct
import urllib.parse

import common

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA
MAX_MESSAGE = 16 << 20


class ProtocolError(ValueError):
    pass


class ConnectionClosed(OSError):
    pass


def accept_key(key):
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def encode_frame(opcode, payload=b"", mask_key=None, fin=True):
    """One client-to-server frame. Clients must mask; mask_key is injectable for tests."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    mask_key = os.urandom(4) if mask_key is None else mask_key
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 1 << 16:
        head += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack("!Q", n)
    return head + mask_key + _mask(payload, mask_key)


def _mask(data, key):
    if not data:
        return b""
    k = (key * (len(data) // 4 + 1))[: len(data)]
    return (int.from_bytes(data, "big") ^ int.from_bytes(k, "big")).to_bytes(len(data), "big")


class FrameDecoder:
    """Incremental decoder: feed bytes, get complete (opcode, payload) messages.

    Fragmented data messages are reassembled; control frames may arrive in
    between and are returned as they come.
    """

    def __init__(self):
        self.buf = bytearray()
        self.frag_op = None
        self.frag = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            fr = self._one()
            if fr is None:
                return out
            fin, op, payload = fr
            if op >= 0x8:  # control frame: never fragmented
                if not fin or len(payload) > 125:
                    raise ProtocolError("bad control frame")
                out.append((op, payload))
            elif op == OP_CONT:
                if self.frag_op is None:
                    raise ProtocolError("continuation without start")
                self.frag += payload
                if len(self.frag) > MAX_MESSAGE:
                    raise ProtocolError("message too large")
                if fin:
                    out.append((self.frag_op, bytes(self.frag)))
                    self.frag_op, self.frag = None, bytearray()
            else:
                if self.frag_op is not None:
                    raise ProtocolError("new message inside fragmented one")
                if fin:
                    out.append((op, payload))
                else:
                    self.frag_op, self.frag = op, bytearray(payload)

    def _one(self):
        b = self.buf
        if len(b) < 2:
            return None
        fin, op, masked, n = b[0] & 0x80, b[0] & 0x0F, b[1] & 0x80, b[1] & 0x7F
        if b[0] & 0x70:
            raise ProtocolError("reserved bits set")
        pos = 2
        if n == 126:
            if len(b) < 4:
                return None
            n, pos = struct.unpack_from("!H", b, 2)[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, pos = struct.unpack_from("!Q", b, 2)[0], 10
        if n > MAX_MESSAGE:
            raise ProtocolError("frame too large")
        key = b""
        if masked:  # servers should not mask, but tolerate it
            if len(b) < pos + 4:
                return None
            key, pos = bytes(b[pos:pos + 4]), pos + 4
        if len(b) < pos + n:
            return None
        payload = bytes(b[pos:pos + n])
        del b[:pos + n]
        return bool(fin), op, _mask(payload, key) if masked else payload


class WsConnection:
    """A connected, handshaken websocket. Blocking connect, then non-blocking reads."""

    def __init__(self, url, cafile=None, timeout=10.0, sock_factory=None):
        u = urllib.parse.urlsplit(url)
        if u.scheme not in ("ws", "wss"):
            raise ValueError("not a websocket url: %s" % url)
        self.url = url
        host, port = u.hostname, u.port or (443 if u.scheme == "wss" else 80)
        path = (u.path or "/") + ("?" + u.query if u.query else "")
        raw = (sock_factory or socket.create_connection)((host, port), timeout)
        self.sock = common.ssl_context(cafile).wrap_socket(raw, server_hostname=host) if u.scheme == "wss" else raw
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                           % (path, host, common.USER_AGENT, key)).encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionClosed("closed during handshake")
            resp += chunk
            if len(resp) > 65536:
                raise ProtocolError("handshake response too long")
        head, rest = resp.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if " 101 " not in lines[0] + " ":
            raise ProtocolError("handshake refused: %s" % lines[0])
        hdrs = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
        if hdrs.get("sec-websocket-accept") != accept_key(key):
            raise ProtocolError("bad Sec-WebSocket-Accept")
        self.decoder = FrameDecoder()
        self.sock.setblocking(False)
        self.closed = False
        self.pending = self.decoder.feed(rest)

    def fileno(self):
        return self.sock.fileno()

    def read(self):
        """Drain the socket; return complete text messages. Answers pings, raises ConnectionClosed."""
        frames, self.pending = self.pending, []
        while True:
            try:
                chunk = self.sock.recv(1 << 16)
            except (BlockingIOError, InterruptedError, ssl.SSLWantReadError):
                break  # drained: everything the kernel and TLS layer held is decoded
            if not chunk:
                self.closed = True
                raise ConnectionClosed("peer closed the connection")
            frames += self.decoder.feed(chunk)
        out = []
        for op, payload in frames:
            if op == OP_TEXT:
                out.append(payload.decode("utf-8"))
            elif op == OP_PING:
                self._send(encode_frame(OP_PONG, payload))
            elif op == OP_CLOSE:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 1005
                self.close()
                raise ConnectionClosed("server closed (%d %s)" % (code, payload[2:].decode("utf-8", "replace")))
        return out

    def send_text(self, text):
        """Send one text message (a subscription, an application-level ping)."""
        self._send(encode_frame(OP_TEXT, text))

    def _send(self, data):
        self.sock.setblocking(True)
        try:
            self.sock.sendall(data)
        finally:
            self.sock.setblocking(False)

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self._send(encode_frame(OP_CLOSE, struct.pack("!H", 1000)))
            except OSError:
                pass
        try:
            self.sock.close()
        except OSError:
            pass
