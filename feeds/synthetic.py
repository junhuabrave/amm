#!/usr/bin/env python3
"""Synthetic stand-in for a venue adapter: no network needed.

Emits the symbols config/crypto.ini expects, as fast as it can, so the
live pipe can be tried end to end before pointing it at a real venue:

    python3 feeds/synthetic.py --seconds 600 | ./build/amm_run --config config/crypto.ini

Prices are a correlated random walk; trades print at the touch and
occasionally through it. This is a plumbing check, not a market model.
"""
import argparse
import math
import random

import common


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=600.0, help="simulated seconds to emit")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    dt = 0.25
    # name, mid, vol per sqrt(s), half-spread, tick, size
    syms = [["BINANCE:BTCUSDT", 97000.0, 6.0, 1.0, 0.01, 0.02],
            ["BINANCE:ETHUSDT", 3500.0, 0.45, 0.08, 0.01, 0.3],
            ["BINANCE-PERP:BTCUSDT", 97030.0, 6.0, 1.0, 0.1, 0.02]]
    t = 1_700_000_000 * 10**9
    try:
        for _ in range(int(a.seconds / dt)):
            t += int(dt * 1e9)
            common_shock = rnd.gauss(0, 1)
            for s in syms:
                z = common_shock if "BTC" in s[0] else 0.8 * common_shock + 0.6 * rnd.gauss(0, 1)
                s[1] += s[2] * math.sqrt(dt) * z + (0.02 * s[2] * rnd.gauss(0, 1) if "PERP" in s[0] else 0.0)
                bid = math.floor((s[1] - s[3]) / s[4]) * s[4]
                ask = math.ceil((s[1] + s[3]) / s[4]) * s[4]
                for side, px in (("B", ask), ("S", bid)):
                    if rnd.random() < 0.6 * dt:
                        through = s[3] * rnd.expovariate(2.0) if rnd.random() < 0.3 else 0.0
                        print(common.line(t, "T", s[0], px + (through if side == "B" else -through), s[5] * rnd.uniform(0.2, 1.5), side))
                print(common.line(t, "B", s[0], bid, s[5] * rnd.uniform(1, 5), ask, s[5] * rnd.uniform(1, 5)))
    except (KeyboardInterrupt, BrokenPipeError):
        pass


if __name__ == "__main__":
    main()
