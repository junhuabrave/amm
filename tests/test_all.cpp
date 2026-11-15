// Unit and integration tests. No external framework: CHECK records failures,
// main returns non-zero if any failed.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "amm/sim/scenario.hpp"

using namespace amm;
using namespace amm::sim;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) do { ++g_checks; const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { ++g_fail; std::printf("  FAIL %s:%d  %s = %.10g, expected %.10g (tol %.3g)\n", __FILE__, __LINE__, #a, _a, _b, static_cast<double>(tol)); } } while (0)
#define TEST(name) static void name(); static const Reg reg_##name(#name, name); static void name()
struct Case { const char* name; void (*fn)(); };
static std::vector<Case>& cases() { static std::vector<Case> c; return c; }
struct Reg { Reg(const char* n, void (*f)()) { cases().push_back({n, f}); } };

struct Rec {
  InstrumentId inst; Side side; double px, qty; bool ioc, hedge; OrderId id; bool live;
};
struct RecVenue final : Venue {
  std::vector<Rec> orders;
  OrderId next = 0;
  OrderId submit(InstrumentId i, Side s, double px, double qty, bool ioc, bool hedge) override {
    orders.push_back(Rec{i, s, px, qty, ioc, hedge, ++next, !ioc});
    return next;
  }
  void cancel(InstrumentId, OrderId id) override {
    for (auto& o : orders) if (o.id == id) o.live = false;
  }
  const Rec* live(InstrumentId i, Side s) const {
    for (auto it = orders.rbegin(); it != orders.rend(); ++it)
      if (it->inst == i && it->side == s && it->live) return &*it;
    return nullptr;
  }
};

static TopOfBook book(double mid, double half, double tick, TimeNs ts, double sz = 100) {
  return TopOfBook{floor_to(mid - half, tick), ceil_to(mid + half, tick), sz, sz, ts};
}

// ---- pricing ---------------------------------------------------------------

TEST(option_parity_and_greeks) {
  const double lv[2] = {100.0, 0.3};
  OptionPricer c(0, 1, 105, 0.5, true), p(0, 1, 105, 0.5, false);
  const PriceOut oc = c.price(lv, 0), op = p.price(lv, 0);
  NEAR(oc.px - op.px, 100.0 - 105.0, 1e-9);  // zero-carry put-call parity
  const double h = 1e-4;
  const double up[2] = {100.0 + h, 0.3}, dn[2] = {100.0 - h, 0.3};
  NEAR(oc.ex.to(0), (c.price(up, 0).px - c.price(dn, 0).px) / (2 * h), 1e-6);
  const double vu[2] = {100.0, 0.3 + h}, vd[2] = {100.0, 0.3 - h};
  NEAR(oc.ex.to(1), (c.price(vu, 0).px - c.price(vd, 0).px) / (2 * h), 1e-5);
  NEAR(oc.ex.to(0) - op.ex.to(0), 1.0, 1e-12);
  CHECK(c.price(lv, 0.25).px < oc.px);  // time decay
  OptionPricer w(0, 1, 105, 0.5, true, 0.1);
  NEAR(w.price(lv, 0).px, 0.1 * oc.px, 1e-12);  // warrant ratio
}

TEST(bond_par_and_duration) {
  // 5% coupon, yield 5% => par.
  const double lv[2] = {380.0, 120.0};
  BondPricer b(0, 1, 5.0, 8.0, 1.0);
  const PriceOut o = b.price(lv, 0);
  NEAR(o.px, 100.0, 1e-9);
  const double up[2] = {381.0, 120.0}, dn[2] = {379.0, 120.0};
  NEAR(o.ex.to(0), (b.price(up, 0).px - b.price(dn, 0).px) / 2.0, 1e-5);
  CHECK(o.ex.to(0) < 0);
  BondPricer hy(0, 1, 5.0, 8.0, 1.5);
  NEAR(hy.price(lv, 0).ex.to(1) / hy.price(lv, 0).ex.to(0), 1.5, 1e-12);
}

TEST(linear_pricer) {
  const double lv[1] = {410.0};
  LinearPricer f(0, 110.0, -0.08, 400.0);
  NEAR(f.price(lv, 0).px, 109.2, 1e-12);
  NEAR(f.price(lv, 0).ex.to(0), -0.08, 1e-12);
}

// ---- quoting maths -----------------------------------------------------------

TEST(glft_limits_and_monotonicity) {
  const Glft g0 = glft(1e-12, 2.0, 80.0, 100.0);
  NEAR(g0.base, 1.0 / 80.0, 1e-9);  // risk-neutral limit: half-spread 1/k
  const Glft a = glft(0.01, 2.0, 80.0, 100.0), b = glft(0.05, 2.0, 80.0, 100.0);
  CHECK(b.C > a.C);        // more risk aversion: stronger skew
  CHECK(b.base < a.base);  // and a tighter risk-neutral component
  const Glft liquid = glft(0.05, 8.0, 80.0, 100.0);
  CHECK(liquid.C < b.C);   // faster flow: inventory is cheaper to hold
}

TEST(rfq_margin_maximises_expected_profit) {
  const double a = -2.4, b = 60.0;
  for (double c : {-0.03, 0.0, 0.02, 0.08}) {
    const double d = rfq_margin(a, b, c);
    auto f = [&](double x) { return (x - c) / (1.0 + std::exp(a + b * x)); };
    double best = -1e9, arg = 0;
    for (double x = c; x < c + 0.5; x += 1e-5) if (f(x) > best) { best = f(x); arg = x; }
    NEAR(d, arg, 2e-5);
    CHECK(d > c);
  }
}

TEST(linalg_inverse_sqrt) {
  std::mt19937_64 rng(3);
  std::normal_distribution<double> N;
  const size_t n = 5;
  std::vector<double> m(n * n), a(n * n, 0.0);
  for (auto& x : m) x = N(rng);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j)
      for (size_t k = 0; k < n; ++k) a[i * n + j] += m[i * n + k] * m[j * n + k];
  const std::vector<double> r = inv_sqrt_sym(a, n);
  for (size_t i = 0; i < n; ++i)  // R A R = I
    for (size_t j = 0; j < n; ++j) {
      double x = 0;
      for (size_t k = 0; k < n; ++k)
        for (size_t l = 0; l < n; ++l) x += r[i * n + k] * a[k * n + l] * r[l * n + j];
      NEAR(x, i == j ? 1.0 : 0.0, 1e-7);
    }
  const std::vector<double> l = cholesky(a, n);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j) {
      double x = 0;
      for (size_t k = 0; k < n; ++k) x += l[i * n + k] * l[j * n + k];
      NEAR(x, a[i * n + j], 1e-9);
    }
}

// ---- signals -----------------------------------------------------------------

TEST(micro_price_symmetric_book_is_mid) {
  MicroPrice mp;
  NEAR(mp.update(TopOfBook{99.99, 100.01, 500, 500, 1000}), 100.0, 1e-12);
  CHECK(mp.ready());
}

TEST(micro_price_learns_predictive_imbalance) {
  // Synthetic market where the next mid move follows the book imbalance.
  MicroPrice mp;
  mp.horizon_s = 0.1;
  std::mt19937_64 rng(5);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  double mid = 100.0;
  TimeNs ts = 0;
  for (int i = 0; i < 40000; ++i) {
    const double bs = 100 + 900 * U(rng), as = 100 + 900 * U(rng);
    mp.update(TopOfBook{mid - 0.01, mid + 0.01, bs, as, ts += ns(0.1)});
    mid += 0.01 * (bs - as) / (bs + as);  // moves by imbalance * half-spread
  }
  CHECK(mp.bx() > 0.5);
}

TEST(intensity_estimator_recovers_curve) {
  IntensityEstimator e;
  e.init(1.0, 60.0);  // prior off on both level and slope
  e.tau_s = 5000; e.slope_ridge = 0.1;  // long memory, trust the data
  const double A = 2.0, k = 80.0;
  std::mt19937_64 rng(1);
  for (int round = 0; round < 400; ++round) {
    for (double d : {0.004, 0.012, 0.020, 0.028, 0.036}) {
      const double dt = 2.0;
      e.accrue(d, dt);
      for (int n = std::poisson_distribution<int>(A * std::exp(-k * d) * dt)(rng); n > 0; --n) e.fill(d);
    }
    e.fit(2.0);
  }
  NEAR(e.k(), k, 0.12 * k);
  NEAR(e.A(), A, 0.25 * A);
}

TEST(toxicity_measures_adverse_moves) {
  ToxicityTracker t;
  t.alpha = 0.5;
  t.record(0, Side::Buy, 100.0);
  t.settle(ns(1.0), 99.9);  // bought, then fair fell: adverse
  CHECK(t.adverse() > 0.04);
  ToxicityTracker u;
  u.alpha = 0.5;
  u.record(0, Side::Sell, 100.0);
  u.settle(ns(1.0), 99.9);  // sold, then fair fell: favourable, never negative
  NEAR(u.adverse(), 0.0, 1e-12);
}

TEST(win_curve_learns) {
  WinCurve w;
  w.init(-2.4, 60.0);
  const double p0 = w.p(0.05);
  for (int i = 0; i < 500; ++i) w.learn(0.05, false);  // we keep losing at this margin
  CHECK(w.p(0.05) < p0);
}

// ---- matching engine ------------------------------------------------------------

TEST(matching_price_time_priority) {
  MatchBook b(0.01);
  std::vector<Exec> ex;
  b.add(1, 0, Side::Sell, 10001, 5, false, ex);
  b.add(2, 1, Side::Sell, 10001, 5, false, ex);
  b.add(3, 1, Side::Sell, 10000, 5, false, ex);
  b.add(4, 2, Side::Buy, 10001, 12, true, ex);
  CHECK(ex.size() == 3);
  CHECK(ex[0].maker_id == 3 && ex[0].px_ticks == 10000);  // best price first
  CHECK(ex[1].maker_id == 1 && ex[1].qty == 5);           // then time priority
  CHECK(ex[2].maker_id == 2 && ex[2].qty == 2);
  CHECK(b.resting() == 1);                                // IOC remainder not rested
  CHECK(b.cancel(2) && !b.cancel(2));
  CHECK(b.resting() == 0);
}

TEST(matching_no_self_trade_and_top_excluding) {
  MatchBook b(0.5);
  std::vector<Exec> ex;
  b.add(1, 1, Side::Sell, 200, 1, false, ex);   // ours at 100.0
  b.add(2, 0, Side::Sell, 202, 3, false, ex);   // market at 101.0
  b.add(3, 0, Side::Buy, 196, 2, false, ex);
  b.add(4, 1, Side::Buy, 204, 2, true, ex);     // our IOC skips our own offer
  CHECK(ex.size() == 1 && ex[0].maker_id == 2 && ex[0].qty == 2);
  const TopOfBook t = b.top_excluding(1);
  NEAR(t.ask, 101.0, 1e-12);
  NEAR(t.ask_sz, 1.0, 1e-12);
  NEAR(t.bid, 98.0, 1e-12);
}

// ---- engine ------------------------------------------------------------------------

struct Mini {  // one factor, n identical linear instruments
  RecVenue v;
  Engine e;
  Mini(EngineConfig c, int n) : e(c, &v) {
    e.factors().add("F", 100.0, 0.02);
    e.factors().adapt_vol = false;
    for (int i = 0; i < n; ++i) {
      Instrument in;
      in.symbol = "I" + std::to_string(i);
      in.tick = 0.0001; in.quote_size = 100; in.max_pos = 1e6; in.A0 = 2; in.k0 = 80;
      if (i == 0) in.ref_for_factor = 0;
      in.pricer = e.add_pricer(std::make_unique<LinearPricer>(0));
      e.add_instrument(in);
    }
    e.start(0);
    for (int i = 0; i < n; ++i) e.on_book(i, book(100.0, 0.03, 0.01, 1000 + i));
  }
};

TEST(portfolio_skew_reduces_to_glft_for_one_asset) {
  EngineConfig c;
  c.hedge_band = 1e12;
  c.adapt_intensity = false;
  Mini p(c, 1);
  c.portfolio_skew = false;
  Mini s(c, 1);
  for (Mini* m : {&p, &s}) m->e.on_fill(Fill{0, Side::Buy, 100.0, 700, 2000, 0, false});
  const Quote qp = p.e.make_quote(0), qs = s.e.make_quote(0);
  const Glft g = glft(c.gamma, 2, 80, 100);
  NEAR(qs.skew, 700 * 0.02 * g.C, 1e-12);
  NEAR(qp.skew, qs.skew, 1e-9);
  NEAR(qp.half, qs.half, 1e-9);
  CHECK(qp.skew > 0 && qp.bid < 100.0 - qp.half + 1e-9);  // long: both quotes shift down
}

TEST(shared_risk_is_charged_less_when_more_instruments_can_shed_it) {
  EngineConfig c;
  c.hedge_band = 1e12;
  c.adapt_intensity = false;
  Mini one(c, 1), two(c, 2);
  one.e.on_fill(Fill{0, Side::Buy, 100.0, 700, 2000, 0, false});
  two.e.on_fill(Fill{0, Side::Buy, 100.0, 700, 2000, 0, false});
  const double s1 = one.e.make_quote(0).skew;
  // Two identical, perfectly correlated instruments: exact multi-asset result
  // is 1/sqrt(2) of the single-asset skew, applied to both.
  NEAR(two.e.make_quote(0).skew, s1 / std::sqrt(2.0), 1e-9);
  NEAR(two.e.make_quote(1).skew, s1 / std::sqrt(2.0), 1e-9);
  c.portfolio_skew = false;
  Mini silo(c, 2);
  silo.e.on_fill(Fill{0, Side::Buy, 100.0, 700, 2000, 0, false});
  NEAR(silo.e.make_quote(1).skew, 0.0, 1e-12);  // siloed: the other book is blind to it
}

struct Full {
  Universe u = default_universe();
  Ids id;
  RecVenue v;
  Engine e;
  TimeNs ts = 1000;
  explicit Full(EngineConfig c, bool decorrelate = false) : e(c, &v) {
    build_engine(u, e);
    if (decorrelate)
      for (FactorId a = 0; a < 5; ++a)
        for (FactorId b = a + 1; b < 5; ++b) e.factors().set_corr(a, b, 0.0);
    e.factors().adapt_vol = false;
    e.start(0);
    for (size_t i = 0; i < u.insts.size(); ++i)
      if (u.insts[i].in.proto == Protocol::Clob)
        e.on_book(i, book(e.fair(i), u.insts[i].bg_half, u.insts[i].in.tick, ts += 1000));
  }
};

TEST(delta_hedged_option_book_does_not_skew_the_stock) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12;
  Full f(c, true);  // factors decorrelated so that only delta links option and stock
  const double delta = OptionPricer(0, 1, 100, 0.25, true, 1.0, -0.1).price(f.e.factors().levels(), 0).ex.to(0);
  f.e.on_fill(Fill{f.id.c100, Side::Buy, f.e.fair(f.id.c100), 50, f.ts += 1000, 0, false});
  const double unhedged = f.e.make_quote(f.id.stock).skew;
  CHECK(unhedged > 0);  // long delta via options pushes stock quotes down
  CHECK(f.e.make_quote(f.id.p100).skew < 0);  // and makes the engine bid up puts
  f.e.on_fill(Fill{f.id.stock, Side::Sell, f.e.fair(f.id.stock), std::round(50 * 100 * delta), f.ts += 1000, 0, false});
  NEAR(f.e.factor_exposure(f.id.EQ), 0.0, 1.0);
  // Delta-neutral: the lean almost disappears. A small residual remains by
  // design - shedding the remaining vega means trading options, which will
  // bring delta back, and the multi-asset solution anticipates that.
  CHECK(std::fabs(f.e.make_quote(f.id.stock).skew) < 0.2 * unhedged);
  CHECK(f.e.make_quote(f.id.c110).skew > 0);  // vega is still long: other options lean to sell it
  // With the real spot-vol correlation (-0.7), long vega behaves like short
  // spot, so the hedged book leans to buy stock.
  Full g(c);
  g.e.on_fill(Fill{g.id.c100, Side::Buy, g.e.fair(g.id.c100), 50, g.ts += 1000, 0, false});
  g.e.on_fill(Fill{g.id.stock, Side::Sell, g.e.fair(g.id.stock), std::round(50 * 100 * delta), g.ts += 1000, 0, false});
  CHECK(g.e.make_quote(g.id.stock).skew < 0);
  // A siloed stock book just leans against its own short position.
  EngineConfig sc = mode_config(Mode::Siloed);
  sc.hedge_band = 1e12;
  Full s(sc);
  s.e.on_fill(Fill{s.id.stock, Side::Sell, s.e.fair(s.id.stock), std::round(50 * 100 * delta), s.ts += 1000, 0, false});
  CHECK(s.e.make_quote(s.id.stock).skew < 0);
}

TEST(cross_asset_fair_leads_the_derivative_market) {
  Full uni(mode_config(Mode::Unified)), silo(mode_config(Mode::Siloed));
  for (Full* f : {&uni, &silo}) {
    const double c0 = f->e.fair(f->id.c100), w0 = f->e.fair(f->id.warrant), p0 = f->e.fair(f->id.p100);
    f->e.on_book(f->id.stock, book(101.0, 0.03, 0.01, f->ts += 1000));  // only the stock ticks
    const double dc = f->e.fair(f->id.c100) - c0, dw = f->e.fair(f->id.warrant) - w0, dp = f->e.fair(f->id.p100) - p0;
    if (f == &uni) {
      CHECK(dc > 0.4 && dc < 0.7);   // ~ delta * 1.00
      CHECK(dp < -0.3 && dp > -0.6);
      CHECK(dw > 0.02 && dw < 0.07); // ratio 0.1
      const Rec* b = f->v.live(f->id.c100, Side::Buy);
      CHECK(b && b->px > c0);        // and the option was requoted off the stock tick
    } else {
      NEAR(dc, 0.0, 1e-12);
      NEAR(dw, 0.0, 1e-12);
    }
  }
}

TEST(bond_fair_tracks_rates_between_composites) {
  Full f(mode_config(Mode::Unified));
  f.e.on_composite(f.id.bond_a, f.e.fair(f.id.bond_a), f.ts += 1000);
  const double b0 = f.e.fair(f.id.bond_a);
  // Rate future sells off 0.40 = +5bp: an 8y bond should fall ~0.3.
  f.e.on_book(f.id.fut, book(f.e.fair(f.id.fut) - 0.40, 0.0075, 0.005, f.ts += 1000));
  const double d = f.e.fair(f.id.bond_a) - b0;
  CHECK(d < -0.2 && d > -0.45);
  const auto bid = f.e.on_rfq(f.id.bond_a, Side::Sell, 1, f.ts += 1000);  // client sells, we bid
  const auto ask = f.e.on_rfq(f.id.bond_a, Side::Buy, 1, f.ts += 1000);
  CHECK(bid && ask && *bid < f.e.fair(f.id.bond_a) && *ask > f.e.fair(f.id.bond_a));
}

TEST(rfq_price_leans_with_inventory) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12;
  Full f(c);
  const double ask0 = *f.e.on_rfq(f.id.bond_a, Side::Buy, 1, f.ts += 1000);
  const double bid0 = *f.e.on_rfq(f.id.bond_a, Side::Sell, 1, f.ts += 1000);
  f.e.on_fill(Fill{f.id.bond_a, Side::Buy, f.e.fair(f.id.bond_a), 8, f.ts += 1000, 0, false});  // now long 8mm
  CHECK(*f.e.on_rfq(f.id.bond_a, Side::Buy, 1, f.ts += 1000) < ask0);   // keener to sell
  CHECK(*f.e.on_rfq(f.id.bond_a, Side::Sell, 1, f.ts += 1000) < bid0);  // less keen to buy
  // The other bond shares rate and credit risk, so it leans too.
  CHECK(f.e.marginal_risk(f.id.bond_b) > 0);
}

TEST(quotes_are_valid_and_respect_obligations) {
  Full f(mode_config(Mode::Unified));
  for (size_t i = 0; i < f.u.insts.size(); ++i) {
    const Instrument& in = f.u.insts[i].in;
    if (in.proto != Protocol::Clob) continue;
    const Quote q = f.e.make_quote(i);
    CHECK(q.bid_on && q.ask_on);
    CHECK(q.ask - q.bid >= in.tick - 1e-9);
    NEAR(q.bid / in.tick, std::round(q.bid / in.tick), 1e-6);
    NEAR(q.ask / in.tick, std::round(q.ask / in.tick), 1e-6);
    if (in.max_spread > 0) CHECK(q.ask - q.bid <= in.max_spread + 1e-9);
    const Rec* b = f.v.live(i, Side::Buy);
    const Rec* a = f.v.live(i, Side::Sell);
    CHECK(b && a && b->px < a->px);
  }
}

TEST(position_limit_turns_off_one_side) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 1e12;
  Full f(c);
  f.e.on_fill(Fill{f.id.c100, Side::Buy, f.e.fair(f.id.c100), 200, f.ts += 1000, 0, false});
  const Quote q = f.e.make_quote(f.id.c100);
  CHECK(!q.bid_on && q.ask_on);
  CHECK(f.v.live(f.id.c100, Side::Buy) == nullptr);
}

TEST(risk_limit_only_quotes_the_reducing_side) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 5.0;
  Full f(c);
  f.e.on_fill(Fill{f.id.stock, Side::Buy, 100.0, 2000, f.ts += 1000, 0, false});
  CHECK(f.e.risk() > 5.0);
  const Quote q = f.e.make_quote(f.id.stock);
  CHECK(!q.bid_on && q.ask_on);
  const Quote qc = f.e.make_quote(f.id.c100), qp = f.e.make_quote(f.id.p100);
  CHECK(!qc.bid_on && qc.ask_on);  // calls add delta
  CHECK(qp.bid_on && !qp.ask_on);  // puts reduce it
}

TEST(hedger_trades_factor_exposure_back_inside_band) {
  EngineConfig c = mode_config(Mode::Unified);
  Full f(c);
  f.e.on_fill(Fill{f.id.c100, Side::Buy, f.e.fair(f.id.c100), 150, f.ts += 1000, 0, false});
  const Rec* h = nullptr;
  for (const auto& o : f.v.orders) if (o.hedge) h = &o;
  CHECK(h && h->inst == f.id.stock && h->side == Side::Sell && h->ioc);
  if (h) {
    const double e0 = f.e.factor_exposure(f.id.EQ);
    f.e.on_fill(Fill{h->inst, h->side, h->px, h->qty, f.ts += 1000, h->id, true});
    CHECK(std::fabs(f.e.factor_exposure(f.id.EQ)) < std::fabs(e0));
    CHECK(std::fabs(f.e.factor_exposure(f.id.EQ)) * f.e.factors().vol(f.id.EQ) <= c.hedge_band + 1.0);
  }
}

TEST(kill_switch_cancels_everything) {
  EngineConfig c = mode_config(Mode::Unified);
  c.max_loss = 1000.0; c.hedge_band = 1e12;
  Full f(c);
  f.e.on_fill(Fill{f.id.stock, Side::Buy, 120.0, 1000, f.ts += 1000, 0, false});  // -20k vs fair
  f.e.on_timer(f.ts += ns(0.1));
  CHECK(f.e.halted());
  for (const auto& o : f.v.orders) CHECK(!o.live);
  CHECK(!f.e.on_rfq(f.id.bond_a, Side::Buy, 1, f.ts += 1000));
}

// ---- end to end -------------------------------------------------------------------

static SimResult run(Mode m, uint64_t seed, double secs_, double* eng_pnl = nullptr) {
  const Universe u = default_universe();
  SimConfig sc;
  sc.seed = seed; sc.duration_s = secs_;
  Sim sim(u, sc);
  Engine eng(mode_config(m), &sim);
  build_engine(u, eng);
  sim.attach(&eng);
  const SimResult r = sim.run();
  if (eng_pnl) *eng_pnl = eng.pnl();
  return r;
}

TEST(simulation_is_deterministic) {
  const SimResult a = run(Mode::Unified, 42, 120), b = run(Mode::Unified, 42, 120);
  CHECK(a.pnl == b.pnl && a.fills == b.fills && a.volume == b.volume);
  CHECK(run(Mode::Unified, 43, 120).pnl != a.pnl);
}

TEST(engine_and_simulator_books_agree) {
  double ep = 0;
  const SimResult r = run(Mode::Unified, 7, 300, &ep);
  CHECK(r.fills > 500);
  // Independent accounting: the engine marks to its own fair values, the
  // simulator to the truth. They differ only by valuation error on inventory.
  NEAR(ep, r.pnl, 0.05 * std::fabs(r.pnl) + 2500.0);
}

TEST(unified_avoids_derivative_sniping) {
  double su = 0, ss = 0;
  for (uint64_t seed = 1; seed <= 3; ++seed) {
    const SimResult u = run(Mode::Unified, seed, 600), s = run(Mode::Siloed, seed, 600);
    const Ids id;
    for (InstrumentId i : {id.c100, id.p100, id.c110, id.p90, id.warrant, id.perp}) {
      su += u.inst_sniped[i];
      ss += s.inst_sniped[i];
    }
  }
  CHECK(su < 0.75 * ss);
}

int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  for (const auto& c : cases()) {
    if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
    const int before = g_fail;
    c.fn();
    ++ran;
    std::printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("\n%d tests, %d checks, %d failed\n", ran, g_checks, g_fail);
  return g_fail ? 1 : 0;
}
