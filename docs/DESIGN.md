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
calibration, file-based configuration), a parameter sweep over recorded
sessions, an order path to a real venue (pre-trade risk gate, gateway process
for Binance USD-M futures with testnet as default, drop copy, reconciler,
kill file; section 12), tests, and a latency benchmark.

**Out of scope (not implemented).** Any session against a real venue with real
or test money so far (the gateway has run in dry-run mode on live data only;
testnet needs API keys only the owner can create), low-latency feeds (FIX,
ITCH, OUCH; market data arrives over websockets or REST), multi-level order
ladders and queue-position modelling, engine state persistence (a restart
cancels everything and takes over venue positions), regulatory reporting and best-execution
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
- **Reference markets** (`quote = false`) are observed like any other: they
  move their factor and keep a basis, but are never quoted. Several markets
  can observe one factor (a perpetual and its spot market); the latest to
  move pulls the factor, and each keeps its own basis.
- **Lead-lag** (`lead`, `lead_mode`, `lead_beta`, `lead_tau_ms`,
  `lead_gain`, off by default). Correlation between factors enters risk, not
  fair value: a move in one factor does not move another's level. A follower
  market names a leader, either a factor (its level) or another instrument
  (its mid, e.g. a perpetual following its spot market), and one of two shapes:
  - `gap`: the engine tracks `x = ln(mid) - beta * ln(leader)` against its
    average over `tau` and adds `mid * (exp(-gain * (x - avg)) - 1)` to fair
    value: when the leader has moved and the follower has not, its fair value
    moves first.
  - `momentum`: `x = beta * ln(leader)` against its average over `tau`, and
    `mid * (exp(gain * (x - avg)) - 1)`: the leader's own recent move predicts
    the follower's next one, whether or not the follower has started to move.
  The average treats the series as piecewise constant (it held its previous
  value since the last update), so a jump after a quiet spell counts in full.
  On Binance the gap shape did not help; the momentum shape from spot carries
  a small real signal (section 11).

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
  otherwise queue position is kept. With `min_quote_life_ms` set, a quote
  younger than that is kept unless it must go: the side is switched off, it
  is larger than the position limit now allows, or it rests more
  aggressively than the new target. Moves that would only make it more
  aggressive wait. This bounds the order rate on tick-by-tick data.
- **Orders in flight count against the limit.** A cancelled order can still
  fill until the venue confirms the cancel (`Engine::on_cancel_done`; the
  paper venue and the simulator both confirm). Until then its remaining size
  is held as "closing" and subtracted from the room for new quotes, so the
  worst case, every order still able to fill doing so, stays inside
  `max_pos`. Found on first contact with websocket data: bursts of book
  updates made the quote flicker, a dozen cancelled orders were still in
  flight when the market jumped, and all of them filled (BTC reached 0.285
  against a 0.1 limit). Tested by `runner_respects_position_limit_when_quotes_flicker`.
- **One order in flight per side** (`wait_cancel_ack`). A quote that has to go
  is cancelled at once, but its replacement is sent only when the cancel is
  confirmed. Without this, in fast markets the engine cancelled and replaced
  on every book update while earlier cancels were still in flight: averages
  of 3 orders a second hid bursts of hundreds, which a venue would throttle,
  and the stacked orders filled adversely. Found by the pre-trade gate's
  message-rate and open-order checks. On the 4-hour Binance journal mean paper
  P&L went from +11 to +63 and drawdown from 67 to 27.
- **Pre-trade gate** (`live/risk_gate.hpp`) between the engine and any venue,
  paper or real: kill switch, price collar against the last mid, order size and
  notional caps, a token-bucket message rate inside the venue's own limit,
  open-order cap, and self-trade prevention (a passive order that would cross
  our own resting order is refused; IOC hedges go through with the venue
  told to cancel our resting side instead, Binance `EXPIRE_MAKER`). A refused
  order returns id 0 and the engine treats it as not sent. Cancels are never
  blocked.

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
| naive | 43,908 | 19,124 | 3.3 | 14,482 | 13,027 | 53,603 | 65.1% |
| siloed | 90,999 | 10,894 | 11.8 | 3,968 | 13,726 | 16,466 | 23.2% |
| +fair | 116,185 | 6,705 | 15.5 | 2,588 | 11,494 | 14,613 | 24.3% |
| +skew | 113,576 | 16,577 | 12.2 | 6,147 | 14,228 | 21,253 | 35.5% |
| unified | 160,891 | 9,229 | 18.4 | 2,375 | 11,632 | 18,417 | 36.9% |

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
  about 60% of the drawdown.

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
| Parameter sweep | `apps/amm_sweep.cpp` | Replays one journal under a grid of config overrides, in parallel; P&L, drawdown, order rate, peak positions, P&L by time slice |
| Adapters | `feeds/*.py` | Binance spot and perpetuals (REST polling, and websockets in `binance_ws.py` over `ws.py`), Bybit perpetuals (websockets, `bybit_ws.py`, through `binance_ws.py --bybit-perp`), Coinbase; standard library only |
| Sample data | `apps/amm_record_sim.cpp`, `feeds/synthetic.py` | Journals and a live-style stream with no network |

**Why adapters are separate processes.** A venue adapter only has to print
lines. It can be written in any language, restarted without touching the
engine, and replaced by a file. Adding a venue is one small file (see the
docstring in `feeds/common.py`); adding an asset class to it is a config
section. The shipped adapters poll REST endpoints, which is simple and has no
dependencies but is limited to roughly one update per half second per symbol.
`binance_ws.py` writes the same lines from Binance's push streams: every
top-of-book change (`bookTicker`, real time) and every aggregated trade
(`aggTrade`; on futures these are pushed in batches about every 100 ms).
Perpetual book and trades come on two connections (Binance's `/public` and
`/market` routes), so a trade can be stamped a few milliseconds away from the
book change it caused; `--perp-book-url` and `--perp-trade-url` can point both
at the legacy combined route to keep them in one ordered connection. A native
C++ feed handler would write the same lines again.

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

**Calibration.** Volatility is estimated from one-second mid changes by
iterated truncation: start from the total, set aside moves beyond five
sigmas as jumps, re-estimate from the rest, and repeat until the set of jumps
stops changing. An earlier median-based scale failed on real tapes: Binance
perpetual mids are unchanged in about half of all one-second samples (BTC 60%,
ETH 48% on 2026-11-15), so the median move is zero or one tick, and ETH showed
a diffusive vol of 0.015 with almost every ordinary move labelled a jump.
Truncation gives 0.184 for ETH with two jumps in 15 minutes. Volatility
measured over 1 to 60 second horizons is flat on these tapes (4.4 to 4.6 for
BTC, 0.17 to 0.19 for ETH), so one-second sampling is not inflated by
bid-ask bounce in the mid. The fill curve is measured the way a resting quote experiences it: a
hypothetical quote is placed at each distance from the prevailing mid and
counted as filled when a trade prints at or through it within the horizon.
Public prints identify the curve at and beyond the touch only; inside the
spread it is an extrapolation that the engine's online estimator then corrects
from its own fills.

**Backtesting.** Replay is the backtest: a journal recorded live (or from the
simulator) runs through the same runner and paper venue, and gives the same
result every time. `amm_sweep` replays one journal under a grid of config
overrides (`--set engine.gamma=0.01,0.05,0.2`, `--set instrument:*.max_pos=...`)
on all cores and tabulates P&L, drawdown, order rate, how close each
instrument came to its limit, and P&L per time slice (`--split N`). The
journal is read and parsed once and every run steps through it in lockstep,
so a 12-setting grid over a 4-hour journal (800 MB, 11M events) takes about
10 s; separate readers per run thrashed the file cache on an 8 GB machine and
took 48 minutes.

Paper P&L is strongly path dependent: one different fill changes every later
quote, position and fill. On the 4-hour journal, changing ETH's k0 from 19.68
to 19.7 moved P&L from +12 to -41. `--jitter N` therefore replays each
setting N more times with every A0 and k0 scaled by a fixed factor within
+-1% and reports mean, spread and worst case; settings are compared on those
ensembles. The same downward bias applies, and a setting chosen on one session
is fitted to it: choose on one recording and confirm on another.

What the sweeps found on Binance perpetuals (16 paths per setting):

| gamma | quote life | 15 min, mean P&L | 4 h, mean P&L (st.dev) | 4 h orders/s |
|---|---:|---:|---:|---:|
| 0.05 | 1 s | -8.7 | -177 (14) | 8.3 |
| 0.005 | 1 s | -5.8 | -115 (16) | 7.8 |
| 0.005 | 2.5 s | 0.1 | -81 (13) | 5.7 |
| 0.005 | 5 s | 14.3 | -2 (22) | 4.4 |
| 0.005 | 10 s | 10.5 | 18.5 (21) | 3.3 |
| 0.005 | 20 s | 11.8 | 8.9 (8) | 2.3 |
| 0.05 | 10 s | 3.1 | -6 (10) | 3.5 |

Requoting on every small move of fair value is what loses. The likely
mechanism, not yet isolated: each cancel and replace gives up queue position,
and the replacement joins the back of the queue just as the market moves. Keeping a quote for 5 to 20 seconds unless it
must go (it is through fair value, too large, or switched off; section 8)
turned a steady loss into a small gain on both journals and cut the order
rate by more than half. Lower gamma helped at every quote life. With no
minimum life the loss limit stopped the engine in two of three 4-hour runs.
Both instruments reach their position limits in nearly every run, so the
limits, not the quotes, bound inventory risk here.

With one order in flight per side (section 8) the picture shifts again. Mean
paper P&L over 8 jittered paths, gamma 0.005, cancel acks awaited, on three
sessions (the third a weekday morning, 2026-11-16 08:19 to 08:37 UTC, 1.5M
events; a network that intercepted TLS to Binance cut that recording short):

| quote life | Sun 15 min | Sun 4 h | Mon 18 min | 4 h orders/s |
|---|---:|---:|---:|---:|
| 5 s | 5.1 | 52.7 | 7.5 | 1.9 |
| 10 s | 3.2 | 63.2 | 8.2 | 1.3 |
| 20 s | 7.7 | 52.0 | 13.3 | 0.9 |
| 30 s | 7.8 | 87.7 | 12.2 | 0.8 |
| 60 s | 14.5 | 51.9 | 6.3 | 0.6 |
| 10 min | 4.6 | 58.0 | 18.5 | 0.2 |

Every life from 5 s up is positive on all three; beyond 30 s the sessions
disagree on the ranking and drawdown grows. The spread between sessions is
now larger than the spread between paths within a session, so more
sessions, not finer grids, are what would sharpen the choice.

**Fees decide it.** Everything above ignored trading fees, and the paper
venue let a quote that was marketable on arrival take liquidity, which the
post-only orders the gateway sends cannot do. With both corrected (maker and
taker fees charged on every fill; quotes post-only in paper as live), mean
paper P&L at gamma 0.005 and a 30 s quote life, 4 jittered paths each:

| maker fee | quotes | Sun 15 min | Mon 18 min | Sun 4 h | 4 h volume |
|---|---|---:|---:|---:|---:|
| 0.02% (Binance base tier) | at the touch | -11.4 | +5.5 | -5.3 | 338k |
| 0.02% | widened by the fee | -2.5 | -3.4 | +69.4 | 30k |
| 0.01% | at the touch | -6.6 | +9.5 | +28.5 | 338k |
| 0 | at the touch | -1.8 | +13.4 | +62.3 | 338k |
| -0.005% (rebate) | at the touch | +0.6 | +15.4 | +79.2 | 338k |

At the base tier, quoting at the touch loses: the earlier positive results
were mostly fees not paid and fills a post-only order could not have had.
Widening quotes by the fee (`fees_in_quotes`, 13 USDT a side on BTC against a
0.1 spread) makes the engine rarely quote; the 4-hour gain rests on 172
fills during a few large moves and is not evidence of an edge. At-the-touch
quoting is positive on all three sessions only with a maker rebate, and
breaks even near a 0.01% maker fee. So on these markets the strategy's
viability is a question of fee tier, which no parameter here changes.
`config/binance_perp.ini` ships the base-tier fees with `fees_in_quotes` on,
so by default it does not quote where it cannot pay the fee; set your own
tier's rates (and `fees_in_quotes = false` with a rebate) to quote at the
touch.

**Cross-asset fair value on real data: a small signal, not enough on its
own.** The simulator's largest gain over a single-instrument quoter comes
from fair value that moves before an instrument's own market.
`feeds/leadlag.py` measures whether that exists between real markets, in
receipt time (what the engine sees), on a 50 ms grid.

*BTC and ETH perpetuals* (4 h): most of the co-movement is in the same step
(correlation 0.48 at lag 0, 0.12 one step either way). After a BTC move of at
least 1 bp, ETH has made 63% of it within the same step and 95% within a
second; after an ETH move, BTC makes 36% and 71%. As a predictor of the next
move, the gap between a market and what its partner implies explains under
0.5% of the variance at every averaging window (0.25 to 5 s) and horizon (0.1
to 2 s), and for ETH its sign is wrong. Swept as the gap term on three
sessions it cost up to 59 (4 h) and gained at most 5 (Monday).

*Spot against perpetual* (3.7 h, 2026-11-16 19:31 to 23:15 UTC, both coins):
the perpetual leads, not spot. Cross-correlation sums to 0.75 (BTC) and 0.83
(ETH) at perp-first lags against 0.49 and 0.48 at spot-first lags; after a
perp move of 1 bp spot makes 46% of it in the same step and 94-99% within a
second. The gap term is useless here too (R² 0.000): most gaps open because
the perpetual moved, and the perpetual does not come back. Spot's *own recent
move* is different. Spot's log mid minus its 100 ms average predicts the
perpetual's next 250 ms move with a coefficient of 0.51-0.69 and R² 2-3%,
alike for BTC and ETH and in both halves of the session (3.0% then 2.4% for BTC, 3.4% then 1.9% for
ETH); a regression of the perp's next move on both
markets' last 50 ms returns puts 0.34 on spot's and 0.10 on its own, nearly
doubling the R² of the perp's own return alone. That is the `momentum` lead
shape, `lead = BTC-USDT-SPOT` in `config/binance_perp_spot.ini`.

Swept on the same session (7-run jittered ensembles, mean P&L net of fees,
gain 0.9, tau 250 ms; 0.02% maker fee / zero fee):

| quote life | no lead | spot momentum |
| --- | --- | --- |
| 1 s | -18.6 / -26.9 | -3.1 / -16.4 |
| 5 s | -12.9 / -31.9 | -6.9 / -29.5 |
| 30 s | -1.3 / +40.7 | -2.0 / +39.0 (half the fills) |

The signal works as a toxicity filter: whenever quotes move fast enough to use
a half-second signal, it removes the fills that would have been picked off
(fills drop by about a half at zero fee) and recovers most of the loss. It
does not change the ranking: on this data, slow quoting beats fast, and the
fee tier decides the sign. At 30 s quote life it earns the same on half the
volume. The config ships with `lead_gain = 0`; 0.9 at 250 ms is the setting to
try with fast quoting or on a rebate tier, after confirming it on another
session. Spot as a factor observer (`ref_gain`) is off: with both markets at
gain 1 the factor snaps to whichever ticked last, and the perpetual ticks
several hundred times a second, so spot's information is erased within
milliseconds.

**Verified and not verified.** Tests cover the event format, the config
files, the paper venue's fill rules, replay determinism (a session recorded
live replays to the identical result), warm-up, calibration against tapes
with known parameters (including a mid that sits still for most samples),
config overrides for sweeps, the websocket framing and handshake, and the position
limit under quote flicker with orders in flight. The adapters' parsers are
tested against responses and stream messages captured from the live Binance
spot, Binance perpetual and Coinbase endpoints on 2026-11-15.

Live sessions on Binance USD-M perpetuals (BTCUSDT, ETHUSDT), 2026-11-15:

| | REST, 1 s polling | Websockets |
|---|---:|---:|
| Length | 300 s | 898 s and 4 h |
| Events | 5,656 | 765,302; 11.3M over 4 h (411k trades) |
| Exchange-to-receipt time, p50 / p99 | - | about 125 / 220 ms (includes clock offset) |
| Calibrated BTC vol / k | 3.5 / 3.5 | 4.65 / 1.9 (15 min), 4.88 / 2.33 (4 h) |
| Paper orders per second | - | 15.2 at 250 ms quote life, 36.2 at 0; 0.8 with the current config (4 h) |
| Paper P&L on volume | -1.2 on 48k | 250 ms: -17.1 on 952k (15 min); current config: +8.7 on 74k (15 min), +70.8 on 555k (4 h), +12.3 on 59k (Mon 18 min) |
| Peak position vs limit | - | at the limit, never beyond |

The 4-hour session (2026-11-15 20:30 to 11-16 00:30 UTC) and the 15-minute one
ran with no drops or reconnects; exchange-to-receipt latency stayed near
70 to 100 ms median. Paper P&L stays
biased downward for the reasons above; over this window BTC rose about 100
USDT while both books sat long at their limits. No session has yet run
through Binance's 24-hour disconnect, on a weekday, or in a busy market.

## 12. Live path: orders to a real venue

```
 binance_ws.py ── market data ──> binance_gateway.py ──> amm_run --orders FIFO ──┐
                                   ^   | REST orders,        (engine, risk gate,  │
                                   |   | user-data stream    own-order filter,    │
                                   |   v                     state snapshots)     │
                                  Binance <── order lines (N/X/K) over FIFO ──────┘
                                   │
          binance_dropcopy.py <────┘ user-data stream, own connection ──> journal
          reconcile.py: engine state vs drop copy vs REST ──> kill file + cancel all
```

**Protocol.** The engine writes one line per order action (`N` new, `X`
cancel, `K` kill) to a named pipe; the gateway writes execution reports
(`A` accepted, `F` fill, `D` done with a reason, `P` venue position, `G`
gateway status) into the engine's event stream. The gateway is the only
writer of that stream: market data reaches it on standard input and passes
through as whole lines. Two processes writing into one pipe could interleave
inside a line (macOS guarantees atomic pipe writes only up to 512 bytes).

**Gateway** (`feeds/binance_gateway.py`, plumbing in `binance_trade.py`).
Quotes go as post-only (`GTX`): rejected rather than allowed to take
liquidity. Hedges go as IOC. Prices are rounded to the tick never more
aggressively than asked, sizes down to the step; orders under the venue's
minimum size or notional are refused locally. A cancel for an order still on
its way is held until the venue has it, never sent before. A request that
times out is resolved by asking the venue for the order rather than guessing.
A 429 or 418 pauses new orders for the venue's retry-after. Startup cancels
every open order on the configured symbols and reports positions; shutdown
and kill cancel everything. Losing the user-data stream reports the gateway
down, which stops the engine: without execution reports it cannot know its
position. Testnet is the default; mainnet needs `--mainnet` and
`AMM_ALLOW_MAINNET=MAINNET_REAL_MONEY`. Keys come from the environment and
are never printed. `--dry-run` acknowledges and cancels locally with no keys
and no network, to exercise the whole pipeline on live data.

**Runner in gateway mode.** Waits for the gateway's ready; takes over venue
positions before quoting; subtracts our accepted resting size from the
displayed book, and holds a book update in which we are the whole touch
(the next level is unknown from top of book, and passing it on would let
fair value follow our own quote); stops on a venue position that still
differs from its own after a 3 s grace (Binance sends the account update and
the fill that explains it separately, in either order), a gateway reporting down, the loss limit, or a kill file; and
writes a JSON state line every second.

**Drop copy and reconciler.** `binance_dropcopy.py` holds its own connection
to the account stream and journals every message, append-only, with a REST
snapshot at start and after every reconnect. `reconcile.py` rebuilds
positions and open orders from that journal and compares them with the
engine's state and with REST every second. A difference lasting longer than
the grace period (fills and cancels in flight make short ones normal), or an
engine whose state has stopped updating, writes the kill file and cancels
every open order itself, through a path independent of the engine and the
gateway. `scripts/live_gateway.sh dry|testnet` runs the whole stack.

**Verified.** C++ tests drive the runner through a full gateway session
(ready, position takeover, ack, partial fill, venue expiry and requote,
unknown reports, state snapshots, kill on a position mismatch, kill file).
Python tests cover signing (against the worked example in Binance's API
documentation), rounding, order ids, translation of user-data messages,
the gateway's order handling with a fake venue (cancel ordering, rejects,
rate limits, kill, timeouts) and the reconciler. A dry run on live Binance
data sent 264 orders through the gateway in a minute, with every ack and
cancel reconciled. **Not verified:** any session on the testnet or mainnet;
the user-data message fixtures follow Binance's documented format rather
than captured messages, because capturing them needs an account.

## 13. Roadmap

Ordered by what blocks real use first.

1. Done for Binance: REST and websocket adapters run against the live venue,
   and first contact found and fixed the in-flight cancel breach (section 8).
   Next: a native feed handler and a full-depth book for queue modelling.
2. Calibrate on recorded sessions and replace the placeholder numbers in
   `config/crypto.ini`.
3. Built for Binance USD-M futures, not yet run against a venue (section 12):
   order gateway with acks, rejects, throttles, own-order filtering, startup
   recovery (cancel all, take over positions), drop copy and reconciler.
   Next: a testnet session (needs testnet API keys), captured user-data
   messages as test fixtures, engine state persistence across restarts.
4. Multi-level ladders, queue-position model, and requote hysteresis tuned to
   venue fee schedules.
5. Volatility surface (SVI per expiry, vega buckets as separate factors),
   dividends and rates, American exercise for single-stock options.
6. Exact treatment of idiosyncratic risk in the multi-asset term; finite-horizon
   (end-of-day) boundary terms.
7. Partly built: pre-trade gate (price collar, size, message rate, open
   orders, self-trade), kill file, and the reconciler as an independent
   process fed by the drop copy that can stop the engine and cancel at the
   venue. Next: per-venue regulatory limits, audit trail of every order
   decision, and alerting beyond log lines.
8. Sharding by underlying across cores; kernel-bypass I/O.
