# Research notes

What the design draws on, and what each source contributed. Formulas are
restated in [DESIGN.md](DESIGN.md) in the notation of the code.

## Quoting models

- **Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book".**
  The reservation-price framing: quote around fair value shifted against
  inventory, with a spread set by fill intensity and risk aversion.
- **Guéant, Lehalle & Fernandez-Tapia, "Dealing with the inventory risk: a
  solution to the market making problem"** — https://arxiv.org/abs/1105.3115.
  Closed-form asymptotic quotes for exponential intensities. `glft()` in
  `quoter.hpp` is this result with a general trade size.
- **Bergault, Evangelista, Guéant & Vieira, "Closed-form approximations in
  multi-asset market making"** — https://arxiv.org/abs/1810.04383.
  Quadratic approximation of the multi-asset value function and the resulting
  quotes. `Engine::refresh_liquidity()` evaluates it in factor space, which is
  the reduction that makes it usable for large universes.
- **Bergault & Guéant, "Size matters for OTC market makers: general results and
  dimensionality reduction techniques"** — https://arxiv.org/abs/1907.01225.
  Trade-size dependence and the factor approach to dimensionality.
- **Baldacci, Bergault & Guéant, "Algorithmic market making for options"** —
  https://arxiv.org/abs/1907.12433. Treating an option book through its vega
  makes the problem low-dimensional. Here vega is one factor among several, so
  the same machinery covers options and warrants together with their underlying.
- **Barzykin, Bergault, Guéant & Lemmel, "Optimal quoting under adverse
  selection and price reading"** — https://arxiv.org/abs/2508.20225. Motivation
  for charging measured adverse selection in the half-spread and RFQ cost.
  The engine uses a simple markout estimate, not that paper's model.

## Fair value and microstructure signals

- **Stoikov (2018), "The micro-price: a high-frequency estimator of future
  prices"** — https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2970694.
  Fair value as the expected future mid given the book. `MicroPrice` uses a
  linear, online-learned version (imbalance and order-flow imbalance) instead
  of the paper's Markov-chain construction.
- **Cont, Kukanov & Stoikov (2014), "The price impact of order book events".**
  Definition of order-flow imbalance used in `MicroPrice`.
- **Bergault & Guéant, "Liquidity dynamics in RFQ markets and impact on
  pricing"** — https://arxiv.org/abs/2309.04216, and **Guéant & Pu, "Mid-price
  estimation for European corporate bonds: a particle filtering approach"** —
  https://arxiv.org/abs/1810.05884. Fair value where there is no order book.
  The engine takes a simpler route: a factor model anchored by composite
  prices. A filter over RFQ outcomes is the natural upgrade.

## Industry context

- Risk.net, *Credit derivatives house of the year: Barclays* (Risk Awards 2026)
  — https://www.risk.net/awards/7962592/credit-derivatives-house-of-the-year-barclays.
  Describes a move to a central risk book for bonds, notes the approach is
  common in equity market-making and has been hard to implement in credit.
- Bank of England, Rebecca Jackson, *Frenemies at the gates* (February 2026) —
  https://www.bankofengland.co.uk/speech/2026/february/rebecca-jackson-speech-on-intraday-risk-management-and-principal-trading.
  On non-bank electronic market makers becoming established liquidity
  providers in vanilla products and expanding from there.
- IFR, *Goldman Sachs takes fight to marketmakers in anonymised trading push*
  (June 2026) — https://www.ifre.com/people-and-markets/2437820/goldman-sachs-takes-fight-to-marketmakers-in-anonymised-trading-push.
  Banks investing in systematic trading.

None of these describe the internals of any bank's quoting engine. The
comparison in this repository is against a well-specified single-instrument
quoter in a simulator, not against a production system.
