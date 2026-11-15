# Design

## 1. Scope

**Goal.** One market-making engine that quotes cash equities, listed options,
warrants, crypto spot and perpetuals, futures, credit indices and corporate
bonds from a single codebase and a single risk book.

**What "better" means here.** I cannot see inside any bank's production stack,
so the claim this project makes is narrower and testable. Public reporting
describes bank market-making as organised by desk and asset class, with a
central risk book treated as a recent step for some businesses
(see [RESEARCH.md](RESEARCH.md)). The design below removes that boundary at
three points, and the simulator measures what each one is worth against a
well-built single-instrument quoter:

1. **One fair-value model across assets.** Every instrument is priced off shared
   risk factors, so a tick in the underlying reprices its options, warrants and
   the bonds exposed to the same curve before their own markets move.
2. **One inventory.** Quotes lean on the covariance of each instrument with the
   whole book, not on that instrument's own position. Risk that many
   instruments can shed is charged less than risk only one can.
3. **One risk-aversion parameter in currency.** No per-desk spread tables: the
   same `gamma` produces the spread and skew for a share, a contract, a bitcoin
   fraction and a million of bond face.

**In scope (implemented).** Quoting logic for order-book and RFQ markets, the
factor model and pricers, online calibration, portfolio risk, hedging, limits
and kill switch, a deterministic event-driven simulator with a matching engine,
a paper-trading path on real market data (venue adapters, recorder, replay,
calibration, file-based configuration), tests, and a latency benchmark.

**Out of scope (not implemented).** Sending real orders to any venue, low-latency
feeds (FIX, ITCH, OUCH, websockets; the shipped adapters poll REST), multi-level order ladders and queue-position
modelling, persistence and recovery, regulatory reporting and best-execution
records, a real volatility surface (SVI/SABR, dividends, rates, American
exercise), FPGA or kernel-bypass I/O, and calibration to historical data. These
are listed again under *Roadmap*. Without them this is a research-grade core,
not a system that can be switched on against a live market.

## 2. Architecture

```
                 market data            fills            RFQs / composites
                      |                   |                     |
                      v                   v                     v
   +-------------------------------------------------------------------------+
   |  Engine  (single thread, deterministic, event in -> orders out)        |
   |                                                                         |
   |  signals            factor model           pricers        portfolio     |
   |  micro-price  --->  levels, vols,   --->   model px  ---> positions,    |
   |  OFI                correlations           + greeks       factor expo.  |
   |  fill intensity          ^                    |           Sigma*E, W*E  |
   |  toxicity                |  Newton step       v                |        |
   |  RFQ win curve      reference markets    fair = model+basis    |        |
   |                                               |                v        |
   |                                               +----> quoter <--+        |
   |                                                half-spread, skew        |
   |                                                 |              |        |
   |                                 order manager (CLOB)     RFQ pricer     |
   |                                                 |              |        |
   |              risk limits / kill switch ---------+------ hedger (IOC)    |
   +-------------------------------------------------|-----------------------+
                                                     v
                                          Venue interface (submit / cancel)
                                          simulator today, gateway later
```

Files, all under `include/amm/`:

| File | Responsibility |
|---|---|
| `core.hpp` | Ids, `Side`, `Instrument` reference data, `Exposures`, `TopOfBook`, `Fill`, the `Venue` interface |
| `factor.hpp` | `FactorModel`: factor levels, online volatilities, correlations, covariance products |
| `pricing.hpp` | `Pricer` interface and `LinearPricer`, `OptionPricer`, `BondPricer` |
| `signals.hpp` | `MicroPrice`, `IntensityEstimator`, `ToxicityTracker`, `WinCurve` |
| `quoter.hpp` | Closed-form quoting maths: `glft()`, `rfq_margin()` |
| `linalg.hpp` | Cholesky, Jacobi eigen-decomposition, inverse matrix square root |
| `engine.hpp` | `Engine`: event handlers, risk, quoting, order management, hedging |
| `sim/matching.hpp` | Price-time priority order book |
| `sim/sim.hpp` | Market simulator |
| `sim/scenario.hpp` | Reference 12-instrument universe and the comparison modes |
| `live/*.hpp` | Event format, config loader, paper venue, runner, calibration (section 11) |

The library is header-only C++20 with no dependencies.

## 3. How one engine handles every asset class

Nothing in the quoting path knows what an option or a bond is. Each instrument
is described by three things:

- **Reference data** (`Instrument`): tick, lot (fractional for crypto),
  multiplier, quote size, limits, protocol (order book or RFQ), any quoting
  obligation (maximum spread, as warrant issuers and designated market makers
  have).
- **A pricer** mapping factor levels to a model price and up to four
  sensitivities `dPrice/dFactor`.
- **Optional roles**: "this market is the observation of factor F" and "this
  instrument is the hedge for factor F".

| Asset class | Pricer | Factors | Protocol |
|---|---|---|---|
| Cash equity, crypto spot | `LinearPricer` | own price | order book |
| Crypto perpetual | `LinearPricer` + learned basis | spot price | order book |
| Listed option | `OptionPricer` (Black-Scholes, linear smile) | spot, ATM vol | order book |
| Warrant | `OptionPricer` with conversion ratio, max-spread obligation | spot, ATM vol | order book |
| Rate future, credit index | `LinearPricer` | rate (bp), credit spread (bp) | order book |
| Corporate bond | `BondPricer` (cash flows discounted at rate + beta x spread) | rate, credit spread | RFQ |

Adding an asset class is adding a `Pricer`. FX, commodities, ETFs (basket
pricer) and convertibles fit the same slot.

## 4. Fair value

For each instrument

```
fair = model(factor levels) + basis
```

- **Factor levels** are driven by reference markets. When the book of a
  reference instrument changes, the engine computes its micro-price and moves
  the factor by one damped Newton step so the model reprices that instrument
  to its market: `level += gain * (micro - basis - model) / sensitivity`. For
  linear instruments with gain 1 this is exact. The ATM option acts as the
  reference for the vol factor with gain 0.3, which filters tick noise.
- **Everything that depends on the factor is repriced and requoted in the same
  event.** This is the cross-asset lead-lag: the stock ticks, and options,
  the warrant and (through rates and credit) the bonds move before their own
  markets or composites do.
- **Basis** is whatever the model does not explain: perp-versus-spot funding
  basis, an option trading rich to the smile, a bond's idiosyncratic spread.
  It is absorbed slowly (time constant `basis_tau_s`, 2 s) from the
  instrument's own micro-price, or from composite prices for RFQ instruments,
  so the model supplies speed and the instrument's own market supplies level.
- **Micro-price**: `mid + bx * imbalance * half_spread + bz * OFI * half_spread`.
  The weights are learned online by ridge regression of the realised mid move
  on the two features, so the signal switches itself off where book pressure
  is not predictive.
- **Factor volatilities** are estimated online (time-weighted EWMA of realised
  variance, bounded to 0.5x-4x of the prior), so spreads and skews widen after
  jumps and relax afterwards without a separate regime switch.

## 5. Quoting on order books

For an instrument with quote size `D` (in effective units = units x
multiplier), fill intensity `A*exp(-k*d)` at distance `d` from fair, and risk
aversion `gamma`:

```
bid = fair - half - skew          ask = fair + half - skew

half = ln(1 + gamma*D/k) / (gamma*D)        liquidity term (-> 1/k as gamma -> 0)
     + (D/2) * self                         risk of the next fill
     + adverse                              measured adverse selection
     + fee

C    = sqrt( gamma/(2*A*D*k) * (1 + gamma*D/k)^(1 + k/(gamma*D)) )
```

This is the Guéant-Lehalle-Fernandez-Tapia closed form. For a single asset
`self = sigma*C` and `skew = q*sigma*C`.

### Multi-asset inventory term

The multi-asset extension (Bergault, Evangelista, Guéant, Vieira) approximates
the value function by `-q'Aq`, where `A` solves `A D A = (gamma/2) Sigma` and
`D = diag(d_i)` is each instrument's liquidity, `d_i = gamma / (2 C_i^2)`. The
solution `A = D^(-1/2) (D^(1/2) (gamma/2) Sigma D^(1/2))^(1/2) D^(-1/2)` needs
a matrix square root in instrument space, which does not scale to thousands of
options.

With a factor covariance `Sigma = B Sigma_F B'` (rows of `B` are the
sensitivities) it collapses. Writing `L` for the Cholesky factor of `Sigma_F`:

```
H = sum_i d_i b_i b_i'                 liquidity available along each factor direction (F x F)
W = L (L' H L)^(-1/2) L'               (F x F)
E = B' q                               the book's factor exposure

skew_i = sqrt(gamma/2) * b_i' W E
self_i = sqrt(gamma/2) * b_i' W b_i
```

The `D^(1/2)` terms cancel, so only an `F x F` eigen-decomposition is needed
regardless of the number of instruments. `W` is refreshed on the timer;
`W E` is refreshed on every fill; each quote then costs one dot product of at
most four terms. Idiosyncratic volatility is added per instrument as a
separate diagonal term (`idio * C * q`), which is an approximation.

What this buys, and the tests pin down:

- One instrument, one factor: reduces exactly to single-asset GLFT
  (`portfolio_skew_reduces_to_glft_for_one_asset`).
- Two identical, perfectly correlated instruments: skew on each is `1/sqrt(2)`
  of the single-asset value. Risk that two markets can shed is cheaper to
  hold (`shared_risk_is_charged_less_...`). A first version of this engine
  used the simpler `cov(price_i, portfolio)/sigma_i * C_i`; it over-leaned by
  up to `sqrt(n)` and lost money against a siloed quoter in the simulator,
  which is how the exact form earned its place.
- Long calls hedged short stock: the stock quote stops leaning; the remaining
  vega makes every other option and the warrant lean to sell vega
  (`delta_hedged_option_book_...`). With the real negative spot-vol
  correlation, long vega also makes the engine a better buyer of stock.

After the price is computed: round outward to tick, never lock or cross the
visible market, enforce the maximum-spread obligation, cap sizes at the
position limit, and under a portfolio risk breach show only the side that
reduces variance.

## 6. Quoting RFQs

For a client request of size `z` the engine shows `fair -/+ margin`, with the
margin maximising expected profit against a logistic win curve
`P(win | d) = 1 / (1 + exp(a + b*d))`:

```
maximise (d - c) * P(win | d)
c = (z/2) * self  +/-  skew  + adverse + fee
first-order condition:  d - c = (1 + exp(-(a + b*d))) / b       (unique root, Newton)
```

`c` is the cost of the trade to the book: negative skew contribution when the
trade reduces risk, so the engine prices keenly exactly when it wants the
trade. `a` and `b` are learned by online logistic regression on won/lost
outcomes. Between sparse composite prices, bond fair values keep moving with
the rate future and credit index through the factor model.

## 7. Online calibration

| Quantity | Estimator | Used for |
|---|---|---|
| `A`, `k` per instrument | Time-at-risk and fill counts per distance bucket, exponentially forgotten; ridge-regularised log-linear fit | spread and skew scale |
| Adverse selection | EWMA of fair-value move against us 250 ms after each fill | added to half-spread and RFQ cost |
| RFQ win curve | Online logistic regression | RFQ margin |
| Micro-price weights | Online ridge regression | fair value |
| Factor vols | Time-weighted EWMA of realised variance | spread, skew, hedging bands |

The slope `k` is only weakly identified when quotes sit in a narrow band of
distances, so it is shrunk to its prior and the level `A` is pinned at the
operating point. Fills through fair value are excluded from intensity (they are
adverse selection and are counted there instead).

## 8. Risk, hedging, controls

- **Portfolio risk**: `sqrt(E' Sigma_F E + idio)`, in currency per sqrt(second),
  recomputed on every fill.
- **Hedger**: for each factor with a designated hedge instrument, when
  `|E_f| * vol_f` exceeds `hedge_band` the engine sends an IOC order to bring
  it back to `hedge_target_frac` of the band. Netting across asset classes
  happens before any hedge is sent: a long-delta option fill and a stock sale
  from the stock quote offset without paying the spread. Hedges obey the
  position limit and a cooldown.
- **Limits**: per-instrument position limit (sizes shrink to zero at the
  limit), portfolio risk limit (reduce-only quoting), mark-to-fair loss limit
  (kill switch: cancel everything, decline RFQs).
- **Order manager**: one bid and one ask per instrument; cancel/replace when
  the target moves by `requote_ticks` or the resting size is badly depleted,
  otherwise queue position is kept.

## 9. Execution model

- **Single-threaded, event-driven, deterministic.** State changes only inside
  `on_book`, `on_fill`, `on_timer`, `on_rfq`, `on_composite`. No clocks are
  read; time comes with the event. The same event stream always yields the
  same orders, which is what makes exact replay possible and is tested
  (`simulation_is_deterministic`).
- **No allocation on the quote path** after `start()`. The slow-timescale
  refresh on the timer (`refresh_liquidity`) allocates small temporaries.
- **Measured tick-to-quote** on the development container (2 shared vCPUs, no
  pinning, no isolation; indicative only): about 0.2 us median for an update
  that reprices one instrument and about 0.9 us median, 1.3 us p99, for an
  underlying tick that reprices and requotes six. Run `amm_bench` on the
  target hardware for real numbers. This is the engine's compute time only;
  wire-to-wire latency in production is dominated by feed handlers, the
  network stack and the gateway, none of which exist here.
- **Scaling.** Per-event cost is linear in the instruments that depend on the
  factor that moved. The current code scans all instruments in `flush()` and
  recomputes exposures from scratch on a fill; both are fine at this size and
  are marked for dirty-lists and incremental updates before thousands of
  option series are loaded. Sharding by underlying across cores, with the
  shared factor state published by sequence lock, is the intended path.

## 10. Simulator

The simulator exists to compare designs under identical conditions, not to
forecast P&L.

- **Truth**: five correlated factors (equity, its implied vol, BTC, rates,
  credit) follow diffusion plus Poisson jumps. Each instrument's true value is
  its pricer on the true factors plus an idiosyncratic walk.
- **Background liquidity** requotes around the truth every 50 ms. After a jump
  it catches up after 1 ms on primary markets (stock, BTC spot, rate future,
  credit index) and after 5 ms on derivative markets (options, warrant, perp).
- **Noise takers** arrive Poisson with exponentially distributed willingness to
  pay, which generates the `A*exp(-k*d)` fill curve the theory assumes.
- **Snipers** hit any quote left through the new true value after a jump:
  50 us after the jump on primary markets, 3 ms on derivative markets, with
  probability 0.7.
- **RFQ clients** arrive Poisson; three competing dealers quote truth plus
  noise plus margin; a quarter of clients are informed and trade only when a
  price is through the truth. Composite prices arrive every 20 s with noise.
- **Engine connectivity**: 100 us market-data and 100 us order latency.
- **Accounting**: the simulator keeps its own position and cash for the engine
  and marks to the truth; the engine's own mark-to-fair P&L is checked against
  it (`engine_and_simulator_books_agree`).

Comparison modes (`sim/scenario.hpp`):

| Mode | Fair value | Inventory skew |
|---|---|---|
| `naive` | own market | none; 60% of market spread |
| `siloed` | own market (bonds: last composite) | single-asset GLFT on own inventory, online calibration, net factor hedging |
| `+fair` | factor model | as siloed |
| `+skew` | own market | multi-asset, whole book |
| `unified` | factor model | multi-asset, whole book |

### Results

24 seeds x 1800 simulated seconds, 12 instruments, `gamma = 0.05`
(`amm_sim --seeds 24 --duration 1800 --detail`):

| Mode | Mean P&L | St.dev across seeds | Sharpe (per run) | Max drawdown | Sniped loss | Hedge cost | RFQ hit ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| naive | 47,258 | 23,173 | 3.2 | 16,462 | 12,208 | 53,957 | 65.1% |
| siloed | 90,621 | 12,157 | 11.6 | 5,267 | 12,916 | 16,712 | 23.0% |
| +fair | 116,936 | 9,812 | 15.1 | 2,968 | 10,695 | 15,062 | 24.3% |
| +skew | 114,078 | 16,156 | 12.2 | 6,956 | 13,401 | 21,495 | 35.5% |
| unified | 163,319 | 10,670 | 18.4 | 2,631 | 10,864 | 19,026 | 37.1% |

Sharpe is mean over st.dev of one-second P&L increments times sqrt(seconds),
per run, not annualised.

Reading the table:

- Cross-asset fair values cut sniping losses on options, the warrant and the
  perp by roughly half (to zero on the perp) and remove most of the adverse
  selection on bonds between composites.
- Portfolio skew raises edge captured on every option and, by recognising
  that bond risk can be shed through futures and the index, lets the engine
  price RFQs tighter and win 37% instead of 23% at better total edge.
- The two effects compound: unified earns 1.8x the siloed quoter's P&L with
  about half the drawdown.

### How far these results go

- The simulated world is generated by the same model family the engine uses
  (same pricers, exponential fill curve, Gaussian factors). Real markets are
  misspecified relative to any model; the factor-based modes benefit most from
  this alignment.
- The `siloed` baseline has no cross-asset information at all. A real bond
  desk already adjusts for moves in rates between composites, so part of the
  bond improvement under `+fair` is credit for something desks do today.
- Sniping losses on primary markets are identical across modes by
  construction: nothing here makes the engine faster than a 50 us arbitrageur.
- Parameters (jump sizes, informed share, latencies) are plausible choices,
  not calibrations. Relative ordering was stable at `gamma` of 0.01 and 0.2
  (see README), but magnitudes move with every one of these inputs.
- No fees, financing, funding payments on the perp, capital charges or
  dividends.

## 11. Live path: paper trading on real market data

The engine is connected to real markets through one seam, a normalised event
stream, so that venues, recording, replay and calibration are all independent
of each other and of the engine.

```
 venue adapter (one per venue,       normalised events (CSV lines)
 any language, own process)   --->   B top of book / T trade / C composite
 feeds/binance.py, coinbase.py                |
                                              v
                    +-------------------- amm_run ---------------------+
  --record FILE <-- | journal    Runner: warm-up, timers, accounting   |
                    |               |                     ^            |
  --replay FILE --> | same code     v                     | fills      |
                    |            Engine --- orders ---> PaperVenue     |
                    +--------------------------------------------------+
                                              |
        amm_calibrate FILE  <--- journal -----+---> config/*.ini (vol, A0, k0, corr)
```

| Piece | File | What it does |
|---|---|---|
| Event format | `live/events.hpp` | Three line types; parse and format round-trip exactly |
| Configuration | `live/config.hpp`, `config/*.ini` | Factors, correlations, instruments, pricers and engine parameters from a file; errors carry file and line |
| Paper venue | `live/paper.hpp` | Matches our orders locally against the real book and prints |
| Runner | `live/runner.hpp`, `apps/amm_run.cpp` | Reads events from a pipe or a file, drives engine and venue, records, reports |
| Calibration | `live/calibrate.hpp`, `apps/amm_calibrate.cpp` | Volatility, jumps, correlation, spreads and fill curve from a journal |
| Adapters | `feeds/*.py` | Binance spot and perpetuals, Coinbase; standard library only |
| Sample data | `apps/amm_record_sim.cpp`, `feeds/synthetic.py` | Journals and a live-style stream with no network |

**Why adapters are separate processes.** A venue adapter only has to print
lines. It can be written in any language, restarted without touching the
engine, and replaced by a file. Adding a venue is one small file (see the
docstring in `feeds/common.py`); adding an asset class to it is a config
section. The shipped adapters poll REST endpoints, which is simple and has no
dependencies but is limited to roughly one update per half second per symbol.
That is adequate for paper trading and calibration and not for anything
latency-sensitive; a websocket or native C++ feed handler writes the same
lines.

**Paper venue rules.** Orders and cancels take effect one configured latency
after they are sent. A trade printing through our price fills us; a trade at
our price fills us only after the size displayed ahead of us when we joined
has traded; joining inside the spread puts nobody ahead. If the market quote
crosses our resting price we fill once against the displayed size. IOC and
marketable orders take the displayed size at the touch and no more. No order
leaves the process: there is no order API in the codebase.

**What paper trading cannot measure.** Flow that would have traded with our
better price but never printed, because nobody was quoting there, is
invisible. Adverse fills (the market running over a stale quote) are all
visible. Paper P&L on recorded data is therefore biased downward for a quoter
that sits inside the spread. Replaying the simulator's own journal shows the
size of this: the same engine that earns in the full simulator loses on the
replay, because the journal contains every sniper print but only the small
share of benign takers who were willing to pay the background spread. Treat
paper results as a check on behaviour, risk and adverse selection, not as an
estimate of spread capture.

**Warm-up.** The runner starts the engine only when every order-book
instrument has a market, and feeds factor reference markets first, so factor
levels come from the market and never from the rough values in the config.

**Calibration.** Volatility is a robust (median-based) estimate from one-second
mid changes, with moves beyond five robust sigmas reported separately as
jumps. The fill curve is measured the way a resting quote experiences it: a
hypothetical quote is placed at each distance from the prevailing mid and
counted as filled when a trade prints at or through it within the horizon.
Public prints identify the curve at and beyond the touch only; inside the
spread it is an extrapolation that the engine's online estimator then corrects
from its own fills.

**Verified and not verified.** Tests cover the event format, both config
files, the paper venue's fill rules, replay determinism (a session recorded
live replays to the identical result), warm-up, and calibration against a tape
with known parameters. The adapters' parsers are tested against responses captured from the live
Binance spot, Binance perpetual and Coinbase endpoints on 2026-11-15, and the
resulting lines run through `amm_run`. What has not been done is a continuous
live session: the build environment's shells have no route to any exchange,
so the polling loop has only run against stand-ins. Rate limits and regional
access are the things to watch on a first long run.

## 12. Roadmap

Ordered by what blocks real use first.

1. Run the adapters against the real venues and fix what first contact
   shows; then a websocket (or native) feed for sub-second data.
2. Calibrate on recorded sessions and replace the placeholder numbers in
   `config/crypto.ini`.
3. A real order gateway behind `Venue` (testnet first), with acknowledgements,
   rejects, throttles and own-order filtering of the book; state snapshot and
   recovery.
4. Multi-level ladders, queue-position model, and requote hysteresis tuned to
   venue fee schedules.
5. Volatility surface (SVI per expiry, vega buckets as separate factors),
   dividends and rates, American exercise for single-stock options.
6. Exact treatment of idiosyncratic risk in the multi-asset term; finite-horizon
   (end-of-day) boundary terms.
7. Pre-trade controls required by regulation (price collars, message limits,
   self-trade prevention at venue level) and an independent risk process that
   can kill the engine.
8. Sharding by underlying across cores; kernel-bypass I/O.
