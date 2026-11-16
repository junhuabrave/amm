#!/usr/bin/env python3
"""Reconciler: an independent check that the engine's book matches the venue's, with a kill switch.

    python3 feeds/reconcile.py --config config/binance_perp.ini --state data/state.jsonl \\
        --journal data/dropcopy.jsonl --kill-file data/KILL

Three views of the same account are compared every --interval seconds:

  engine     the last line of the engine's state file (amm_run --state):
             positions and the orders it believes are resting
  drop copy  positions and open orders rebuilt from the drop-copy journal
             (binance_dropcopy.py): last snapshot, then every fill and order update
  venue      (unless --no-rest) positions and open orders polled from REST

A difference that persists longer than --grace seconds is a breach: fills and
cancels in flight make short differences normal, lasting ones are not. On a
breach the reconciler writes the kill file (amm_run --kill-file stops at
once), cancels every open order on the venue itself (unless --no-rest), and
keeps reporting. It also trips if the engine's state stops updating
(--stale seconds): a hung engine cannot cancel its own quotes.
"""
import argparse
import json
import os
import sys
import time

PREFIX = "BINANCE-PERP:"


def instrument_map(config_path):
    """{engine instrument symbol: venue symbol} from an amm config file."""
    out, cur = {}, None
    with open(config_path) as f:
        text = f.read()
    for raw in text.splitlines():
        line = raw.split(";")[0].split("#")[0].strip()
        if line.startswith("[") and line.endswith("]"):
            h = line[1:-1].split()
            cur = h[1] if len(h) == 2 and h[0] == "instrument" else None
        elif cur and "=" in line:
            k, v = [x.strip() for x in line.split("=", 1)]
            if k == "feed":
                out[cur] = v[len(PREFIX):] if v.startswith(PREFIX) else v
    return out


def engine_id_of(cid):
    """'amm<tag>-<hex>' -> engine id (tag ignored: the gateway cancels everything at startup)."""
    if not cid or not cid.startswith("amm") or "-" not in cid:
        return None
    try:
        return int(cid.rsplit("-", 1)[1], 16)
    except ValueError:
        return None


class DropCopyBook:
    """Positions and open orders rebuilt from drop-copy journal records."""

    def __init__(self):
        self.positions = {}
        self.open = set()  # client order ids
        self.records = 0
        self.synced = False  # a snapshot has been seen

    def apply(self, rec):
        self.records += 1
        if "snapshot" in rec:
            s = rec["snapshot"]
            self.positions = {k: float(v) for k, v in s.get("positions", {}).items()}
            self.open = set(s.get("open", []))
            self.synced = True
            return
        m = rec.get("msg")
        if not m:
            return
        if m.get("e") == "ORDER_TRADE_UPDATE":
            o = m["o"]
            cid, x, status = o.get("c"), o.get("x"), o.get("X")
            if x == "NEW":
                self.open.add(cid)
            if x == "TRADE":
                q = float(o["l"]) * (1 if o.get("S") == "BUY" else -1)
                self.positions[o["s"]] = self.positions.get(o["s"], 0.0) + q
            if status in ("FILLED", "CANCELED", "EXPIRED", "EXPIRED_IN_MATCH"):
                self.open.discard(cid)
        elif m.get("e") == "ACCOUNT_UPDATE":  # authoritative position after each change
            for p in m.get("a", {}).get("P", []):
                if p.get("ps", "BOTH") == "BOTH":
                    self.positions[p["s"]] = float(p["pa"])


class Reconciler:
    def __init__(self, inst_map, grace=5.0, tolerance=1e-9, stale=10.0):
        self.map, self.grace, self.tol, self.stale = inst_map, grace, tolerance, stale
        self.venue_syms = sorted(set(inst_map.values()))
        self.since = {}  # breach key -> first time seen
        self.engine = None
        self.engine_seen = None
        self.venue = None  # (positions, open cids)

    def set_engine(self, state, now):
        if self.engine is None or state.get("ts") != self.engine.get("ts"):
            self.engine_seen = now
        self.engine = state

    def set_venue(self, positions, open_cids):
        self.venue = (positions, set(open_cids))

    def differences(self, book, now):
        """{key: description} of everything that differs right now."""
        d = {}
        if self.engine is not None and self.engine_seen is not None and now - self.engine_seen > self.stale:
            d["stale"] = "engine state not updated for %.0fs" % (now - self.engine_seen)
        if self.engine is not None and book.synced:
            ep = self.engine.get("positions", {})
            for inst, vs in self.map.items():
                e, b = float(ep.get(inst, 0.0)), book.positions.get(vs, 0.0)
                if abs(e - b) > self.tol:
                    d["pos:" + vs] = "%s position: engine %g, drop copy %g" % (vs, e, b)
            mine = {o["id"] for o in self.engine.get("open", []) if o.get("acked") and o.get("left", 0) > 0}
            venue_ids = {engine_id_of(c) for c in book.open} - {None}
            for i in sorted(venue_ids - mine):
                d["orphan:%d" % i] = "order %d open at the venue, unknown to the engine" % i
            for i in sorted(mine - venue_ids):
                d["ghost:%d" % i] = "order %d resting in the engine, not open at the venue" % i
        if self.venue is not None and book.synced:
            vp, vo = self.venue
            for vs in self.venue_syms:
                if abs(vp.get(vs, 0.0) - book.positions.get(vs, 0.0)) > self.tol:
                    d["venue:" + vs] = "%s position: venue %g, drop copy %g" % (vs, vp.get(vs, 0.0), book.positions.get(vs, 0.0))
            for c in sorted(vo - book.open):
                d["vorphan:" + c] = "order %s open at the venue, missing from the drop copy" % c
        return d

    def breaches(self, book, now):
        """Differences that have lasted longer than the grace period."""
        d = self.differences(book, now)
        for k in list(self.since):
            if k not in d:
                del self.since[k]
        out = []
        for k, why in d.items():
            first = self.since.setdefault(k, now)
            if now - first >= self.grace or k == "stale":
                out.append(why)
        return out


def last_json_line(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 65536))
            lines = f.read().splitlines()
    except OSError:
        return None
    for l in reversed(lines):
        try:
            return json.loads(l)
        except ValueError:
            continue  # partial last line
    return None


class Tail:
    """New complete lines appended to a file since the last call."""

    def __init__(self, path):
        self.path, self.pos, self.buf = path, 0, b""

    def read(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return []
        self.buf += data
        *lines, self.buf = self.buf.split(b"\n")
        return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--journal", required=True)
    ap.add_argument("--kill-file", required=True)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--grace", type=float, default=5.0)
    ap.add_argument("--stale", type=float, default=10.0)
    ap.add_argument("--no-rest", action="store_true", help="compare engine and drop copy only")
    ap.add_argument("--mainnet", action="store_true")
    a = ap.parse_args()

    rec = Reconciler(instrument_map(a.config), a.grace, stale=a.stale)
    book = DropCopyBook()
    tail = Tail(a.journal)
    rest = None
    if not a.no_rest:
        import binance_trade as bt
        base, _ = bt.endpoints(a.mainnet)
        key, secret = bt.credentials()
        rest = bt.Rest(base, key, secret)
        rest.sync_time()
    tripped = False
    print("reconcile: watching %s" % ", ".join("%s=%s" % kv for kv in rec.map.items()), file=sys.stderr, flush=True)
    try:
        while True:
            now = time.time()
            for l in tail.read():
                try:
                    book.apply(json.loads(l))
                except ValueError:
                    pass
            st = last_json_line(a.state)
            if st is not None:
                rec.set_engine(st, now)
            if rest:
                try:
                    opened = []
                    for s in rec.venue_syms:
                        opened += [o["clientOrderId"] for o in rest.open_orders(s)]
                    rec.set_venue(rest.positions(), opened)
                except Exception as e:  # noqa: BLE001 - a failed poll is reported, never fatal
                    print("reconcile: venue poll failed: %s" % e, file=sys.stderr, flush=True)
            bad = rec.breaches(book, now)
            if bad and not tripped:
                tripped = True
                why = "; ".join(bad)
                with open(a.kill_file, "w") as f:
                    f.write(why + "\n")
                print("reconcile: BREACH, kill file written: %s" % why, file=sys.stderr, flush=True)
                if rest:
                    for s in rec.venue_syms:
                        try:
                            rest.cancel_all(s)
                        except Exception as e:  # noqa: BLE001
                            print("reconcile: cancel-all %s failed: %s" % (s, e), file=sys.stderr, flush=True)
            elif bad:
                print("reconcile: still in breach: %s" % "; ".join(bad), file=sys.stderr, flush=True)
            time.sleep(a.interval)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
