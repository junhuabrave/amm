# amm — asset-agnostic automated market-making engine

A market-making engine that quotes cash equities, listed options, warrants,
crypto spot and perpetuals, futures, credit indices and corporate bonds (RFQ)
from one codebase, one fair-value model and one risk book. Header-only C++20,
no dependencies, with a deterministic multi-asset simulator, tests and a
latency benchmark.

**Status: research-grade core with paper trading and an untried order path.**
The quoting, risk and hedging logic is implemented and tested in simulation,
and runs against real public market data with orders matched locally. An
order gateway for Binance USD-M futures (testnet by default), a pre-trade risk
gate, a drop copy and a reconciler exist and are tested offline and in dry-run
mode on live data, but have not yet traded on any venue. There is no engine
state persistence or regulatory tooling.
See [docs/DESIGN.md](docs/DESIGN.md) for scope, design and roadmap, and
[docs/RESEARCH.md](docs/RESEARCH.md) for sources.

## The three ideas

1. **Every instrument is a pricer over shared risk factors.** A tick in the
   stock reprices its options, the warrant and (through rates and credit) the
   bonds in the same event, before their own markets move.
2. **Quotes lean on the whole book.** The multi-asset closed form of Bergault,
   Evangelista, Guéant and Vieira is evaluated in factor space
   (`skew_i = sqrt(gamma/2) * b_i' W E`), so cost does not grow with the number
   of instruments, and risk that many markets can shed is charged less.
3. **One risk aversion, in currency.** Spreads and skews for every asset class
   follow from `gamma`, online-estimated fill intensity, measured adverse
   selection and factor volatility. No per-desk spread tables.

## Build and run

```sh
cmake -S . -B build && cmake --build build     # or: ./build.sh  (g++ >= 11 only)
./build/amm_tests                              # 50 tests
python3 feeds/test_feeds.py                    # adapter tests, offline
python3 feeds/test_trading.py                  # gateway, drop copy, reconciler, offline
./build/amm_sim --seeds 24 --duration 1800 --detail
./build/amm_bench
```

`amm_sim` options: `--seeds N`, `--duration SECONDS`, `--gamma G`,
`--csv FILE` (per-second P&L paths), `--detail` (per-instrument breakdown).

## Paper trading on real market data

```sh
# live: a venue adapter pipes normalised events into the runner
python3 feeds/binance_ws.py --perp BTCUSDT ETHUSDT \
  | ./build/amm_run --config config/binance_perp.ini --record data/session.csv

# the REST polling adapter (about one update per second) still works the same way
python3 feeds/binance.py BTCUSDT ETHUSDT --perp BTCUSDT | ./build/amm_run --config config/crypto.ini

# replay the recorded session (identical result), then calibrate from it
./build/amm_run --config config/crypto.ini --replay data/session.csv
./build/amm_calibrate data/session.csv
python3 feeds/leadlag.py data/session.csv BINANCE:BTCUSDT BINANCE-PERP:BTCUSDT   # does one market move first?

# backtest a grid of settings on the same recording: one read of the file, runs in parallel,
# --split N shows P&L by time slice, --jitter N averages over N perturbed paths per setting
./build/amm_sweep --config config/binance_perp.ini --replay data/session.csv \
  --set engine.gamma=0.005,0.05 --set engine.min_quote_life_ms=1000,5000,10000 --jitter 15

# no network: try the same pipe on a synthetic stream, or on a simulator journal
python3 feeds/synthetic.py --seconds 600 | ./build/amm_run --config config/crypto.ini
./build/amm_record_sim --out data/sim.csv && ./build/amm_run --config config/sim.ini --replay data/sim.csv
```

- Orders are matched in-process against the real book and trade prints, with
  latency and queue position. Nothing is sent to the venue except requests
  for public data (REST GETs, websocket subscriptions).
- `feeds/binance_ws.py` streams every top-of-book change and every aggregated
  trade (Binance perps: about 2,000 book updates a second across BTC and ETH).
  It uses a small standard-library websocket client (`feeds/ws.py`), reconnects
  on drops and silences, and reports rate and exchange-to-receipt latency on
  stderr. TLS is always verified; Python builds without a CA bundle fall back
  to the system one (or `--cafile` / `$AMM_CAFILE`).
- Instruments, factors, pricers and engine parameters live in `config/*.ini`.
  The numbers in `config/crypto.ini` are placeholders: record an hour, run
  `amm_calibrate`, and replace them.
- A new venue is one small adapter that prints three kinds of line
  (`feeds/common.py` explains how).
- The adapters' parsers are tested against responses and stream messages
  captured from the live Binance and Coinbase endpoints, and live Binance
  perpetual sessions have been recorded over both REST and websockets.
- Paper P&L understates spread capture (flow that never printed is invisible)
  and shows adverse selection in full. Maker and taker fees are charged on
  every fill and quotes are post-only, as live. On the recorded Binance
  sessions, at-the-touch quoting loses at the base fee tier and is positive on
  all of them only with a maker rebate. See [DESIGN.md §11](docs/DESIGN.md).

## Orders to a venue (Binance USD-M futures)

```sh
scripts/live_gateway.sh dry        # live market data, orders acknowledged locally: no keys, nothing sent
export BINANCE_API_KEY=... BINANCE_API_SECRET=...   # testnet keys from testnet.binancefuture.com
scripts/live_gateway.sh testnet    # orders to the testnet, priced off the testnet's own market
touch data/live/KILL               # stop trading: everything is cancelled
```

- The engine writes order lines to a named pipe; `feeds/binance_gateway.py`
  sends them (post-only quotes, IOC hedges) and writes execution reports back
  into the engine's event stream. Mainnet needs `--mainnet` and
  `AMM_ALLOW_MAINNET=MAINNET_REAL_MONEY`; the script does not offer it.
- Every order passes a pre-trade gate (price collar, size, message rate,
  open orders, self-trade, kill switch), in paper mode too.
- `feeds/binance_dropcopy.py` journals the account's execution stream on its
  own connection; `feeds/reconcile.py` compares engine, drop copy and venue
  every second, and on a lasting difference writes the kill file and cancels
  at the venue itself.
- Not yet run on the testnet: that needs API keys only the account owner can
  create. See [DESIGN.md §12](docs/DESIGN.md#12-live-path-orders-to-a-real-venue).

## Simulation results

Twelve instruments on five correlated risk factors with jumps, noise takers,
latency arbitrageurs, informed RFQ clients and competing dealers. Every mode
sees the identical market per seed. 24 seeds x 1800 simulated seconds:

| Mode | Mean P&L | St.dev across seeds | Sharpe (per run) | Max drawdown | RFQ hit ratio |
|---|---:|---:|---:|---:|---:|
| naive (fixed share of market spread) | 43,908 | 19,124 | 3.3 | 14,482 | 65.1% |
| siloed (optimal single-instrument quoting) | 90,999 | 10,894 | 11.8 | 3,968 | 23.2% |
| + cross-asset fair value | 116,185 | 6,705 | 15.5 | 2,588 | 24.3% |
| + portfolio skew | 113,576 | 16,577 | 12.2 | 6,147 | 35.5% |
| unified (both) | 160,891 | 9,229 | 18.4 | 2,375 | 36.9% |

The ordering holds at other risk aversions (12 seeds):

| gamma | siloed | + fair | + skew | unified |
|---|---:|---:|---:|---:|
| 0.01 | 102,386 | 151,176 | 103,780 | 170,629 |
| 0.05 | 89,284 | 115,465 | 107,969 | 158,619 |
| 0.20 | 29,141 | 32,936 | 74,483 | 97,653 |

These numbers compare designs inside a simulator whose world is built from the
same model family as the engine. They are not a forecast of live P&L and not a
comparison with any firm's production system. The limits are spelled out in
[DESIGN.md §10](docs/DESIGN.md#how-far-these-results-go).

Engine compute time, tick to quote, on a shared 2-vCPU container (indicative):
about 0.2 us median for a single-instrument update, 0.9 us median / 1.3 us p99
for an underlying tick that requotes six instruments.

## Layout

```
include/amm/          engine library (core, factor, pricing, signals, quoter, linalg, engine)
include/amm/sim/      matching engine, market simulator, reference scenario
include/amm/live/     event format, config loader, paper venue, risk gate, gateway client, runner, calibration
apps/                 amm_sim (comparison), amm_bench (latency), amm_run (paper trading),
                      amm_calibrate, amm_sweep (replay grid), amm_record_sim
feeds/                venue adapters (Binance, Coinbase, synthetic), Binance order gateway,
                      drop copy, reconciler, and their tests
scripts/              live_gateway.sh: the whole live stack (dry | testnet)
config/               sim.ini (reference universe), crypto.ini (starter live universe),
                      binance_perp.ini (BTC and ETH perpetuals, calibrated and swept),
                      binance_perp_spot.ini (the same, with spot BTC and ETH as reference markets and a spot-momentum lead term, off)
tests/                unit and end-to-end tests
docs/                 DESIGN.md, RESEARCH.md
```

## Using the engine

```cpp
amm::Engine eng(cfg, &venue);                     // venue implements submit/cancel
auto EQ  = eng.factors().add("EQ", 100.0, 0.0124);
auto VOL = eng.factors().add("VOL", 0.30, 1.2e-4);
eng.factors().set_corr(EQ, VOL, -0.7);

amm::Instrument stock;  /* tick, lot, mult, quote_size, max_pos, A0, k0 ... */
stock.ref_for_factor = EQ; stock.hedges_factor = EQ;
stock.pricer = eng.add_pricer(std::make_unique<amm::LinearPricer>(EQ));
eng.add_instrument(stock);
// ... options, bonds, etc. the same way, then:
eng.start(now_ns);
eng.on_book(id, top_of_book);                     // -> orders on the venue
eng.on_fill(fill);
eng.on_timer(now_ns);                             // every ~100 ms
auto px = eng.on_rfq(bond_id, amm::Side::Buy, 1.0, now_ns);
```
