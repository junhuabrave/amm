#!/usr/bin/env python3
"""Does one market move before another? Lead-lag of mid prices in a recorded journal.

    python3 feeds/leadlag.py data/session.csv BINANCE:BTCUSDT BINANCE-PERP:BTCUSDT

Times are receipt times as recorded, which is what the engine sees: a lead
here is one the engine can use, whatever its cause on the exchange side
(matching-engine order, network path, websocket batching).

Two measures, on mids sampled every --step milliseconds:

  cross-correlation   corr(return of A over a step, return of B over a step
                      shifted by k steps). Mass at positive k means A moves
                      first.
  response            after A's mid moves by at least --move bp within one
                      step, how far B moves in the same direction over the
                      next horizons, as a share of A's move. If B moves later
                      by most of A's move, A leads and the gap is the edge.

Both are reported in both directions (A->B and B->A). Standard library only.
"""
import argparse
import math
import sys


def load_mids(path, syms, step_ns):
    """{sym: [mid at each grid time]} on a common grid (last value carried forward)."""
    want = set(syms)
    last = {}
    grid = {s: [] for s in syms}
    t_next = None
    with open(path, "rb") as f:
        for raw in f:
            p = raw.split(b",")
            if len(p) != 7 or p[1] != b"B":
                continue
            s = p[2].decode()
            if s not in want:
                continue
            try:
                ts = int(p[0])
                bid, ask = float(p[3]), float(p[5])
            except ValueError:
                continue
            if t_next is None:
                t_next = ts - ts % step_ns + step_ns
            while ts >= t_next:
                if len(last) == len(syms):
                    for x in syms:
                        grid[x].append(last[x])
                t_next += step_ns
            if ask > bid > 0:
                last[s] = 0.5 * (bid + ask)
    return grid


def returns_bp(series):
    return [1e4 * (b - a) / a for a, b in zip(series, series[1:])]


def xcorr(ra, rb, max_lag):
    """{k: corr(ra[t], rb[t+k])}."""
    n = min(len(ra), len(rb))
    ma, mb = sum(ra[:n]) / n, sum(rb[:n]) / n
    va = math.sqrt(sum((x - ma) ** 2 for x in ra[:n]))
    vb = math.sqrt(sum((x - mb) ** 2 for x in rb[:n]))
    out = {}
    for k in range(-max_lag, max_lag + 1):
        s = 0.0
        for t in range(max(0, -k), min(n, n - k)):
            s += (ra[t] - ma) * (rb[t + k] - mb)
        out[k] = s / (va * vb) if va > 0 and vb > 0 else 0.0
    return out


def response(ma, mb, ra, move_bp, horizons):
    """Mean share of A's move that B makes in the same direction by each horizon (steps).

    Also B's move during the step in which A moved (horizon 0).
    """
    sums = {h: 0.0 for h in [0] + horizons}
    n = 0
    hmax = max(horizons)
    for t, r in enumerate(ra):
        if abs(r) < move_bp or t + 1 + hmax >= len(mb):
            continue
        sgn = 1.0 if r > 0 else -1.0
        base = mb[t]  # B's mid at the start of A's move
        sums[0] += sgn * 1e4 * (mb[t + 1] - base) / base / abs(r)
        for h in horizons:
            sums[h] += sgn * 1e4 * (mb[t + 1 + h] - base) / base / abs(r)
        n += 1
    return n, {h: (sums[h] / n if n else float("nan")) for h in sums}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--step", type=float, default=50.0, help="grid step, ms")
    ap.add_argument("--max-lag", type=float, default=1000.0, help="cross-correlation range, ms")
    ap.add_argument("--move", type=float, default=1.0, help="response: A moves at least this many bp in one step")
    a = ap.parse_args()
    step_ns = int(a.step * 1e6)
    g = load_mids(a.journal, [a.a, a.b], step_ns)
    ma, mb = g[a.a], g[a.b]
    if len(ma) < 100:
        sys.exit("not enough overlapping data (%d samples)" % len(ma))
    ra, rb = returns_bp(ma), returns_bp(mb)
    print("%s vs %s: %d samples of %.0f ms (%.1f min)" % (a.a, a.b, len(ma), a.step, len(ma) * a.step / 60000))
    lag = int(a.max_lag / a.step)
    xc = xcorr(ra, rb, lag)
    print("\ncross-correlation of %.0f ms returns, corr(A[t], B[t+k]); k > 0: A first" % a.step)
    for k in range(-lag, lag + 1):
        if abs(k) <= 4 or k % max(1, lag // 5) == 0:
            bar = "#" * int(round(max(0.0, xc[k]) * 60))
            print("  k=%+5.0f ms  %+.3f %s" % (k * a.step, xc[k], bar))
    pos = sum(xc[k] for k in range(1, lag + 1))
    neg = sum(xc[k] for k in range(-lag, 0))
    print("  sum over k>0 (A leads) %+.3f   over k<0 (B leads) %+.3f   at k=0 %+.3f" % (pos, neg, xc[0]))
    hs = [1, 2, 4, 10, 20]
    for name, x, y, rx in ((a.a + " -> " + a.b, ma, mb, ra), (a.b + " -> " + a.a, mb, ma, rb)):
        n, resp = response(x, y, rx, a.move, hs)
        print("\nresponse %s: %d moves of >= %.1f bp in one step" % (name, n, a.move))
        print("  follower's move in the same direction, as a share of the leader's move:")
        print("  same step %+.2f" % resp[0] + "".join("   +%.0f ms %+.2f" % (h * a.step, resp[h]) for h in hs))


if __name__ == "__main__":
    main()
