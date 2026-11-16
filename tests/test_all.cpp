// Unit and integration tests. No external framework: CHECK records failures,
// main returns non-zero if any failed.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include <fstream>
#include <sstream>

#include "amm/live/calibrate.hpp"
#include "amm/live/risk_gate.hpp"
#include "amm/live/runner.hpp"
#include "amm/sim/scenario.hpp"

using namespace amm;
using namespace amm::sim;
using namespace amm::live;

#ifndef AMM_SOURCE_DIR
#define AMM_SOURCE_DIR "."
#endif

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
  bool refuse = false;  // act as a risk gate that refuses everything
  OrderId submit(InstrumentId i, Side s, double px, double qty, bool ioc, bool hedge) override {
    if (refuse) return 0;
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

// ---- live path: events, config, paper venue, runner, calibration ----------------

TEST(event_lines_round_trip) {
  MdEvent e;
  CHECK(parse_event("1700000000123456789,B,BINANCE:BTCUSDT,60000.1,0.431,60000.2,0.12", e));
  CHECK(e.ts == 1700000000123456789LL && e.type == 'B' && e.sym == "BINANCE:BTCUSDT");
  NEAR(e.a, 60000.1, 1e-9); NEAR(e.b, 0.431, 1e-12); NEAR(e.c, 60000.2, 1e-9); NEAR(e.d, 0.12, 1e-12);
  MdEvent r;
  CHECK(parse_event(format_event(e), r) && r.ts == e.ts && r.a == e.a && r.d == e.d && r.sym == e.sym);
  CHECK(parse_event("5,T,ACME C100 3M,5.93,10,S\r", e) && e.type == 'T' && e.c == -1.0 && e.sym == "ACME C100 3M");
  CHECK(parse_event(format_event(e), r) && r.c == -1.0 && r.b == 10.0);
  CHECK(parse_event("7,C,BOND,99.5", e) && e.type == 'C' && e.a == 99.5);
  for (const char* bad : {"", "# comment", "x,B,S,1,1,2,1", "5,B,S,1,1,2", "5,T,S,1,1", "5,Q,S,1", "5,B,,1,1,2,1", "5,B,S,a,1,2,1"})
    CHECK(!parse_event(bad, e));
}

TEST(config_file_reproduces_the_reference_universe) {
  const LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/sim.ini");
  RecVenue v1, v2;
  Engine a(cfg.engine, &v1), b(mode_config(Mode::Unified), &v2);
  amm::live::build_engine(cfg, a);
  const Universe u = default_universe();
  amm::sim::build_engine(u, b);
  a.start(0); b.start(0);
  CHECK(a.instruments() == b.instruments());
  for (size_t i = 0; i < b.instruments(); ++i) {
    const Instrument& x = a.instrument(i);
    const Instrument& y = b.instrument(i);
    CHECK(x.symbol == y.symbol && x.cls == y.cls && x.proto == y.proto);
    CHECK(x.tick == y.tick && x.lot == y.lot && x.mult == y.mult && x.quote_size == y.quote_size);
    CHECK(x.max_pos == y.max_pos && x.max_spread == y.max_spread && x.idio_vol == y.idio_vol);
    NEAR(x.A0, y.A0, 1e-9); NEAR(x.k0, y.k0, 1e-9);
    CHECK(x.rfq_a0 == y.rfq_a0 && x.rfq_b0 == y.rfq_b0 && x.ref_gain == y.ref_gain);
    CHECK(x.ref_for_factor == y.ref_for_factor && x.hedges_factor == y.hedges_factor);
    NEAR(a.fair(i), b.fair(i), 1e-9);
    NEAR(a.sigma(i), b.sigma(i), 1e-12);
  }
  CHECK(load_config(std::string(AMM_SOURCE_DIR) + "/config/crypto.ini").instruments.size() == 3);
}

TEST(config_errors_are_reported_with_location) {
  auto fails = [](const std::string& text, const std::string& needle) {
    try {
      std::istringstream is(text);
      const LiveConfig c = parse_config(is, "t.ini");
      RecVenue v;
      Engine e(c.engine, &v);
      amm::live::build_engine(c, e);
    } catch (const std::exception& ex) {
      return std::string(ex.what()).find(needle) != std::string::npos;
    }
    return false;
  };
  const std::string f = "[factor F]\nlevel = 1\nvol = 0.1\n";
  const std::string i = "[instrument X]\ntick=0.01\nlot=1\nmult=1\nquote_size=1\nmax_pos=10\n";
  CHECK(fails(f + "[engine]\ngama = 1\n", "t.ini:5"));
  CHECK(fails(f + "[engine]\ngamma = abc\n", "not a number"));
  CHECK(fails(f + i, "pricer"));
  CHECK(fails(f + i + "pricer = linear G\n", "unknown factor 'G'"));
  CHECK(fails(f + i + "pricer = linear F\nhedges = Z\n", "unknown factor 'Z'"));
  CHECK(fails(f + "[instrument X]\npricer = linear F\ntick = 0\n", "must all be > 0"));
  CHECK(fails(i, "no [factor]"));
  CHECK(!fails(f + i + "pricer = linear F ; trailing comment\n", ""));
}

struct PaperFix {
  PaperVenue v{0.001};
  std::vector<Fill> fills;
  PaperFix() {
    v.resize(1);
    v.set_fill_handler([this](const Fill& f) { fills.push_back(f); });
    v.on_time(0);
    v.on_book(0, TopOfBook{99.0, 101.0, 50, 60, 0});
  }
  double filled() const { double q = 0; for (const auto& f : fills) q += f.qty; return q; }
};

TEST(paper_orders_wait_for_latency_and_cancels_race) {
  PaperFix p;
  const OrderId id = p.v.submit(0, Side::Buy, 100.0, 10, false, false);
  p.v.on_trade(0, 99.5, 10, -1);  // prints through our price, but we are not live yet
  CHECK(p.fills.empty());
  p.v.on_time(ns(0.001));
  CHECK(p.v.resting(0) == 1);
  p.v.cancel(0, id);
  p.v.on_trade(0, 99.5, 4, -1);   // cancel still in flight: we get filled
  CHECK(p.fills.size() == 1 && p.fills[0].qty == 4 && p.fills[0].px == 100.0 && p.fills[0].side == Side::Buy);
  p.v.on_time(ns(0.002));
  CHECK(p.v.resting(0) == 0);
  p.v.on_trade(0, 99.5, 10, -1);
  CHECK(p.fills.size() == 1);
}

TEST(paper_queue_position_at_the_touch) {
  PaperFix p;
  p.v.submit(0, Side::Buy, 99.0, 10, false, false);   // joins the 50 already bid at 99
  p.v.submit(0, Side::Sell, 100.5, 10, false, false); // improves the offer: nobody ahead
  p.v.on_time(ns(0.001));
  p.v.on_trade(0, 99.0, 30, -1);
  CHECK(p.fills.empty());            // 30 of the 50 ahead of us traded
  p.v.on_trade(0, 99.0, 25, -1);     // 20 more ahead, then 5 for us
  CHECK(p.fills.size() == 1 && p.fills[0].qty == 5);
  p.v.on_trade(0, 98.9, 100, -1);    // trades through: the rest fills, at our price
  CHECK(p.filled() == 10 && p.fills.back().px == 99.0);
  p.v.on_trade(0, 100.5, 3, +1);     // at our improving offer: immediate
  CHECK(p.fills.back().side == Side::Sell && p.fills.back().qty == 3);
  p.v.on_trade(0, 100.4, 5, +1);     // a buyer who paid less than our offer does not fill us
  CHECK(p.filled() == 13);
  p.v.on_trade(0, 99.0, 5, -1);      // wrong side for the offer, and the bid is gone
  CHECK(p.filled() == 13);
}

TEST(paper_crossing_quotes_and_ioc) {
  PaperFix p;
  p.v.submit(0, Side::Buy, 100.0, 10, false, false);
  p.v.on_time(ns(0.001));
  p.v.on_book(0, TopOfBook{99.0, 100.0, 50, 4, ns(0.002)});  // an offer arrives at our bid
  CHECK(p.fills.size() == 1 && p.fills[0].qty == 4 && p.fills[0].px == 100.0);
  p.v.on_book(0, TopOfBook{99.0, 100.0, 50, 4, ns(0.003)});  // same quote again: no double count
  CHECK(p.fills.size() == 1);
  p.v.on_book(0, TopOfBook{99.0, 99.9, 50, 100, ns(0.004)}); // a new, deeper crossing
  CHECK(p.filled() == 10 && p.v.resting(0) == 0);
  // IOC takes the displayed size at the market's price and never rests.
  p.v.submit(0, Side::Buy, 100.2, 500, true, true);
  p.v.on_time(ns(0.01));
  CHECK(p.fills.back().qty == 100 && p.fills.back().px == 99.9 && p.fills.back().hedge);
  CHECK((p.fills.back().oid & kHedgeTag) != 0 && p.v.resting(0) == 0);
  p.v.submit(0, Side::Sell, 99.5, 5, true, false);  // not marketable against a 99.0 bid
  p.v.on_time(ns(0.02));
  CHECK(p.filled() == 110);
}

static std::string sim_journal(uint64_t seed, double secs_) {
  const Universe u = default_universe();
  SimConfig sc;
  sc.seed = seed; sc.duration_s = secs_;
  Sim sim(u, sc);
  std::ostringstream os;
  sim.tap = [&](TimeNs ts, char type, InstrumentId i, double a, double b, double c, double d) {
    MdEvent ev;
    ev.ts = ts + 1; ev.type = type; ev.sym = u.insts[i].in.symbol; ev.a = a; ev.b = b; ev.c = c; ev.d = d;
    os << format_event(ev) << '\n';
  };
  sim.run();
  return os.str();
}

TEST(runner_replays_a_journal_deterministically) {
  const std::string journal = sim_journal(9, 120);
  CHECK(journal.size() > 100000);
  const LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/sim.ini");
  double pnl[2]; uint64_t fills[2];
  std::string recorded;
  for (int k = 0; k < 2; ++k) {
    std::istringstream in(journal);
    std::ostringstream rec;
    RunnerOptions opt;
    opt.record = k == 0 ? &rec : nullptr;
    Runner r(cfg, opt);
    r.run(in);
    pnl[k] = r.pnl(); fills[k] = r.fills();
    if (k == 0) recorded = rec.str();
    CHECK(r.warm() && r.fills() > 50);
    NEAR(r.elapsed_s(), 120.0, 0.5);
    // The runner's account of fills and the engine's own book must agree on positions.
    for (size_t i = 0; i < r.engine().instruments(); ++i) NEAR(r.position(i), r.engine().position(i), 1e-9);
    NEAR(r.engine().pnl(), r.pnl(), 0.1 * std::fabs(r.pnl()) + 2000.0);
    std::ostringstream sum;
    r.summary(sum);
    CHECK(sum.str().find("paper P&L") != std::string::npos);
  }
  CHECK(pnl[0] == pnl[1] && fills[0] == fills[1]);
  // A journal recorded during a run replays to the same result.
  std::istringstream again(recorded);
  Runner r2(cfg, RunnerOptions{});
  r2.run(again);
  CHECK(r2.pnl() == pnl[0] && r2.fills() == fills[0]);
}

TEST(runner_waits_for_every_market_before_quoting) {
  const LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/crypto.ini");
  Runner r(cfg, RunnerOptions{});
  auto book = [&](const char* sym, double bid, double ask, TimeNs ts) {
    MdEvent e; e.ts = ts; e.type = 'B'; e.sym = sym; e.a = bid; e.b = 1.0; e.c = ask; e.d = 1.0;
    r.on_event(e);
  };
  book("BINANCE:BTCUSDT", 97000.00, 97000.50, ns(1.0));
  book("BINANCE:ETHUSDT", 3500.00, 3500.10, ns(1.1));
  book("SOMETHING:ELSE", 1, 2, ns(1.15));
  CHECK(!r.warm() && r.venue().orders_sent() == 0);
  std::ostringstream early;
  r.summary(early);
  CHECK(early.str().find("BTC-USDT-PERP") != std::string::npos);  // says what it is waiting for
  book("BINANCE-PERP:BTCUSDT", 97030.0, 97030.5, ns(1.2));
  CHECK(r.warm());
  // Factor levels come from the market, not from the config's rough guess.
  NEAR(r.engine().factors().level(0), 97000.25, 0.5);
  NEAR(r.engine().fair(2), 97030.25, 0.5);  // perp = spot + learned basis
  CHECK(r.venue().orders_sent() >= 6);
  // Spot moves; the perp is repriced with it before its own book changes.
  book("BINANCE:BTCUSDT", 97100.00, 97100.50, ns(1.3));
  NEAR(r.engine().fair(2), 97130.25, 1.0);
}

// A stream like the websocket feed's: bursts of book updates sharing one
// receipt timestamp that make the quote flicker, then the market jumps
// through our resting price. Cancelled-but-in-flight orders can still fill,
// so the engine must count them against the position limit.
static std::string flicker_stream(int bursts) {
  std::ostringstream os;
  TimeNs t = ns(1000.0);
  double mid = 65000.0;
  uint64_t rng = 7;
  auto u = [&] { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (rng >> 11) * (1.0 / 9007199254740992.0); };
  auto bk = [&](const char* s, double m, double half) {
    os << t << ",B," << s << ',' << m - half << ",1.5," << m + half << ",1.5\n";
  };
  bk("BINANCE-PERP:ETHUSDT", 1800.0, 0.005);
  for (int b = 0; b < bursts; ++b) {
    for (int k = 0; k < 15; ++k) { mid += (u() < 0.5 ? -0.3 : 0.3); bk("BINANCE-PERP:BTCUSDT", mid, 0.05); }
    t += ns(0.09);  // the next read comes after the paper latency
    const double jump = (b % 2 ? -1.0 : 1.0) * 40.0;
    mid += jump;
    bk("BINANCE-PERP:BTCUSDT", mid, 0.05);  // runs over every resting quote on one side
    os << t << ",T,BINANCE-PERP:BTCUSDT," << mid << ",5," << (jump > 0 ? 'B' : 'S') << '\n';
    t += ns(0.01);
  }
  return os.str();
}

TEST(runner_respects_position_limit_when_quotes_flicker) {
  const LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/binance_perp.ini");
  for (double life : {0.0, 0.25}) {
    LiveConfig c = cfg;
    c.engine.min_quote_life_s = life;
    c.engine.hedge_band = 1e12;  // no hedger: the quotes alone must respect the limit
    std::istringstream in(flicker_stream(40));
    Runner r(c, RunnerOptions{});
    r.run(in);
    CHECK(r.warm() && r.fills() > 10);
    for (size_t i = 0; i < r.engine().instruments(); ++i)
      CHECK(r.peak_position(i) <= r.engine().instrument(i).max_pos + 1e-9);
  }
}

TEST(cancelled_orders_count_against_the_limit_until_confirmed) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 1e12;
  Full f(c);
  const InstrumentId s = f.id.stock;
  const double max_pos = f.u.insts[s].in.max_pos, size = f.u.insts[s].in.quote_size;
  // Worst case if everything still able to fill on the bid side does fill.
  auto worst = [&] {
    const Rec* b = f.v.live(s, Side::Buy);
    return f.e.position(s) + f.e.closing(s, Side::Buy) + (b ? b->qty : 0.0);
  };
  const Rec* first = f.v.live(s, Side::Buy);
  CHECK(first != nullptr && first->qty == size);
  const OrderId old = first ? first->id : 0;
  f.e.on_fill(Fill{s, Side::Buy, f.e.fair(s), max_pos - size, f.ts += 1000, 0, false});
  CHECK(worst() <= max_pos + 1e-9);
  const double fv = f.e.fair(s);
  // Market drops (and the inventory skew moved it already): the bid is
  // cancelled. Until the venue confirms, it may still fill, so no
  // replacement is placed.
  f.e.on_book(s, book(fv - 1.0, f.u.insts[s].bg_half, f.u.insts[s].in.tick, f.ts += 1000));
  CHECK(f.v.live(s, Side::Buy) == nullptr);
  NEAR(f.e.closing(s, Side::Buy), size, 1e-9);
  CHECK(f.v.live(s, Side::Buy) == nullptr);
  CHECK(worst() <= max_pos + 1e-9);
  f.e.on_fill(Fill{s, Side::Buy, fv, 0.4 * size, f.ts += 1000, old, false});  // it did fill, in part
  NEAR(f.e.closing(s, Side::Buy), 0.6 * size, 1e-9);
  CHECK(worst() <= max_pos + 1e-9);
  f.e.on_cancel_done(s, old);               // venue confirms: the rest can no longer fill
  NEAR(f.e.closing(s, Side::Buy), 0.0, 1e-9);
  const Rec* nb = f.v.live(s, Side::Buy);
  CHECK(nb && nb->id != old);               // room is back, and used
  CHECK(worst() <= max_pos + 1e-9);
  f.e.on_cancel_done(s, old);               // a duplicate confirmation changes nothing
  NEAR(f.e.closing(s, Side::Buy), 0.0, 1e-9);
}

TEST(young_quotes_are_kept_unless_they_must_go) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 1e12; c.min_quote_life_s = 1.0;
  Full f(c);
  const InstrumentId s = f.id.stock;
  const double tick = f.u.insts[s].in.tick, half = f.u.insts[s].bg_half;
  f.ts += ns(2.0);
  const double fv = f.e.fair(s);
  f.e.on_book(s, book(fv + 0.5 * tick, half, tick, f.ts += 1000));  // fresh quotes, now young
  const size_t n0 = f.v.orders.size();
  // Market ticks up: our quotes would only become more aggressive. Young: wait.
  for (int k = 1; k <= 3; ++k) f.e.on_book(s, book(fv + (0.5 + k) * tick, half, tick, f.ts += 1000));
  const Rec* bid = f.v.live(s, Side::Buy);
  size_t bids = 0, asks = 0;
  for (size_t k = n0; k < f.v.orders.size(); ++k)
    if (f.v.orders[k].inst == s) (f.v.orders[k].side == Side::Buy ? bids : asks)++;
  CHECK(bid != nullptr && bids == 0);       // the bid waited
  CHECK(asks > 0);                          // the ask, now too low, did not
  // Market drops: our bid is now above where we want it. Replace at once.
  f.e.on_book(s, book(fv - 20 * tick, half, tick, f.ts += 1000));
  const Rec* nb = f.v.live(s, Side::Buy);
  CHECK(nb && bid && nb->id != bid->id && nb->px < bid->px);
  // Once old enough, small moves are followed again.
  f.ts += ns(1.5);
  const OrderId before = f.v.live(s, Side::Buy) ? f.v.live(s, Side::Buy)->id : 0;
  f.e.on_book(s, book(fv - 17 * tick, half, tick, f.ts += 1000));
  const Rec* later = f.v.live(s, Side::Buy);
  CHECK(later && later->id != before);
}

TEST(calibration_recovers_volatility_and_fill_curve) {
  // Synthetic tape with known parameters: a random-walk mid, and trades that
  // print at exponentially distributed depths from it.
  std::mt19937_64 rng(4);
  std::normal_distribution<double> N;
  std::uniform_real_distribution<double> U(0.0, 1.0);
  const double vol = 0.02, A = 1.5, k = 40.0, dt = 0.05;
  std::ostringstream os;
  double mid = 100.0, mid2 = 50.0;
  for (int i = 0; i < 12000 * 2; ++i) {
    const TimeNs ts = ns(i * dt) + 1;
    const double z = N(rng);
    mid += vol * std::sqrt(dt) * z;
    mid2 += 0.5 * vol * std::sqrt(dt) * (0.6 * z + 0.8 * N(rng));
    MdEvent b; b.ts = ts; b.type = 'B';
    b.sym = "X"; b.a = mid - 0.01; b.b = 5; b.c = mid + 0.01; b.d = 5; os << format_event(b) << '\n';
    b.sym = "Y"; b.a = mid2 - 0.01; b.c = mid2 + 0.01; os << format_event(b) << '\n';
    for (double side : {-1.0, 1.0})
      if (U(rng) < A * dt) {
        MdEvent t; t.ts = ts; t.type = 'T'; t.sym = "X"; t.b = 1; t.c = side;
        t.a = mid + side * (-std::log(1.0 - U(rng)) / k);
        os << format_event(t) << '\n';
      }
  }
  std::istringstream in(os.str());
  CalibrationOptions opt;
  opt.horizon_s = 2.0;  // short, so drift of the mid does not blur the depths
  const Calibration c = calibrate(in, opt);
  CHECK(c.symbols.size() == 2 && c.symbols[0].sym == "X");
  const SymbolCalibration& x = c.symbols[0];
  NEAR(x.vol, vol, 0.15 * vol);
  NEAR(x.mean_spread, 0.02, 1e-6);
  NEAR(x.trade_rate, A, 0.15 * A);
  NEAR(x.k, k, 0.3 * k);
  NEAR(x.A, A, 0.35 * A);
  NEAR(c.corr[1], 0.6, 0.1);
  CHECK(c.symbols[1].A == 0.0);  // no trades on Y: reported as not identifiable
}

TEST(calibration_vol_survives_a_mid_that_mostly_sits_still) {
  // Real tapes: the mid is unchanged in about half of all 1 s samples and moves
  // by several ticks in the rest, with occasional jumps on top. A median-based
  // scale collapses to zero or one tick there; truncation must not.
  std::mt19937_64 rng(11);
  std::normal_distribution<double> N;
  std::uniform_real_distribution<double> U(0.0, 1.0);
  const double vol = 0.2, p_move = 0.4, jump = 15 * vol, jump_rate = 1.0 / 200, tick = 0.01;
  const int secs_n = 8000;
  std::ostringstream os;
  double mid = 1800.0;
  int still = 0, jumps = 0;
  for (int i = 0; i < secs_n; ++i) {
    const double before = mid;
    if (U(rng) < p_move) mid += vol / std::sqrt(p_move) * N(rng);
    if (U(rng) < jump_rate) { mid += U(rng) < 0.5 ? -jump : jump; ++jumps; }
    const double q = std::round(mid / (0.5 * tick)) * 0.5 * tick;
    if (q == std::round(before / (0.5 * tick)) * 0.5 * tick) ++still;
    MdEvent b; b.ts = ns(i) + 1; b.type = 'B'; b.sym = "E";
    b.a = q - 0.5 * tick; b.b = 5; b.c = q + 0.5 * tick; b.d = 5;
    os << format_event(b) << '\n';
  }
  CHECK(still > secs_n / 2);  // the regime that broke the median estimator
  std::istringstream in(os.str());
  const Calibration c = calibrate(in);
  CHECK(c.symbols.size() == 1);
  const SymbolCalibration& e = c.symbols[0];
  NEAR(e.vol, vol, 0.1 * vol);
  NEAR(e.jump_rate, jumps / double(secs_n), 0.3 * jumps / double(secs_n));
  NEAR(e.jump_sd, jump, 0.15 * jump);
  CHECK(e.vol_total > e.vol);
}

TEST(sweep_overrides_config_keys_by_section) {
  const std::string base =
      "[engine]\ngamma = 0.05   ; risk aversion\n[factor BTC]\nlevel = 1\nvol = 2\n"
      "[instrument A]\nfeed = X:A\nquote_size = 1\npricer = linear BTC\nref_for = BTC\n"
      "[instrument B]\nfeed = X:B\npricer = linear BTC\n";
  auto parse = [](const std::string& t) { std::istringstream is(t); return parse_config(is); };
  // replaced in place, comment and all
  LiveConfig c = parse(set_ini_value(base, "engine", "gamma", "0.2"));
  NEAR(c.engine.gamma, 0.2, 1e-12);
  // added when the section lacks the key; ':' stands in for the space
  c = parse(set_ini_value(base, "engine", "min_quote_life_ms", "500"));
  NEAR(c.engine.min_quote_life_s, 0.5, 1e-12);
  NEAR(c.engine.gamma, 0.05, 1e-12);
  c = parse(set_ini_value(base, "factor:BTC", "vol", "7"));
  NEAR(c.factors[0].vol, 7.0, 1e-12);
  // one named instrument, or all of them; B has no quote_size line to replace
  c = parse(set_ini_value(base, "instrument:B", "quote_size", "3"));
  NEAR(c.instruments[0].in.quote_size, 1.0, 1e-12);
  NEAR(c.instruments[1].in.quote_size, 3.0, 1e-12);
  c = parse(set_ini_value(base, "instrument *", "max_pos", "9"));
  NEAR(c.instruments[0].in.max_pos, 9.0, 1e-12);
  NEAR(c.instruments[1].in.max_pos, 9.0, 1e-12);
  CHECK(c.instruments[1].feed == "X:B");
  bool threw = false;
  try { set_ini_value(base, "instrument:C", "tick", "1"); } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);  // a typo must not silently sweep nothing
}

TEST(risk_gate_refuses_what_it_should_and_passes_cancels) {
  RecVenue v;
  RiskLimits lim;
  lim.collar_bps = 100; lim.max_orders_per_s = 2; lim.burst = 3; lim.max_open = 2; lim.max_order_notional = 1000;
  RiskGate g(&v, lim);
  g.resize(2, {5.0, 0.0}, {1.0, 1.0});
  g.set_time(ns(1.0));
  CHECK(g.submit(0, Side::Buy, 99, 1, false, false) == 0);  // no reference price yet
  CHECK(g.rejected(Reject::NoReference) == 1);
  g.set_reference(0, 100.0);
  g.set_reference(1, 100.0);
  CHECK(g.submit(0, Side::Buy, 98.9, 1, false, false) == 0);  // 110 bp away
  CHECK(g.rejected(Reject::Collar) == 1);
  CHECK(g.submit(0, Side::Buy, 99.5, 6, false, false) == 0);  // above max_order_qty 5
  CHECK(g.submit(1, Side::Buy, 99.5, 11, false, false) == 0);  // 1094.5 > max_order_notional
  CHECK(g.rejected(Reject::Size) == 2);
  const OrderId b1 = g.submit(0, Side::Buy, 99.5, 1, false, false);
  const OrderId a1 = g.submit(0, Side::Sell, 100.5, 1, false, false);
  CHECK(b1 && a1 && g.open(0) == 2);
  CHECK(g.submit(0, Side::Buy, 99.4, 1, false, false) == 0);  // third resting order
  CHECK(g.rejected(Reject::Open) == 1);
  g.on_fill(b1, 0.4);
  CHECK(g.open(0) == 2);  // partly filled: still resting
  g.on_fill(b1, 0.6);
  CHECK(g.open(0) == 1);
  CHECK(g.submit(0, Side::Buy, 100.6, 1, false, false) == 0);  // would trade with our own offer
  CHECK(g.rejected(Reject::SelfTrade) == 1);
  // burst of 3 used up (two orders and one more below); refill at 2/s
  CHECK(g.submit(1, Side::Buy, 99.5, 1, true, false) != 0);
  CHECK(g.submit(1, Side::Buy, 99.5, 1, true, false) == 0);
  CHECK(g.rejected(Reject::Rate) == 1);
  g.set_time(ns(1.6));
  CHECK(g.submit(1, Side::Buy, 99.5, 1, true, false) != 0);
  CHECK(g.submit(0, Side::Buy, 100.6, 1, true, true) == 0);   // tokens gone again
  g.set_time(ns(3.0));
  CHECK(g.submit(0, Side::Buy, 100.6, 1, true, true) != 0);   // an IOC hedge may cross our offer...
  CHECK(g.ioc_self_cross() == 1);                              // ...and is counted
  const size_t sent = v.orders.size();
  g.kill("test");
  CHECK(g.submit(0, Side::Buy, 99.5, 1, false, false) == 0 && g.rejected(Reject::Killed) == 1);
  g.cancel(0, a1);  // cancels always pass
  CHECK(v.orders.size() == sent && !v.orders[1].live);
  g.on_done(a1);
  CHECK(g.open(0) == 0);
}

TEST(engine_handles_refused_orders_and_orders_the_venue_drops) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 1e12;
  Full f(c);
  const InstrumentId s = f.id.stock;
  const double tick = f.u.insts[s].in.tick, half = f.u.insts[s].bg_half;
  const double fv = f.e.fair(s);
  // The venue (a risk gate) refuses: the engine must not believe it is quoting.
  const Rec* bid = f.v.live(s, Side::Buy);
  CHECK(bid != nullptr);
  const OrderId old_bid = bid->id;
  f.v.refuse = true;
  f.e.on_book(s, book(fv - 20 * tick, half, tick, f.ts += 1000));  // old bid now too high: cancelled
  CHECK(f.e.stats().refused > 0);
  f.v.refuse = false;
  f.e.on_book(s, book(fv - 20 * tick, half, tick, f.ts += 1000));
  const Rec* nb = f.v.live(s, Side::Buy);
  CHECK(nb && nb->id != old_bid);  // retried once the gate let it through
  // The venue expires or rejects a resting quote on its own: requote.
  const OrderId gone = nb->id;
  f.v.orders.back().live = false;
  for (auto& o : f.v.orders) if (o.id == gone) o.live = false;
  f.e.on_cancel_done(s, gone);
  const Rec* again = f.v.live(s, Side::Buy);
  CHECK(again && again->id != gone);
}

TEST(one_order_in_flight_per_side_when_waiting_for_cancel_acks) {
  EngineConfig c = mode_config(Mode::Unified);
  c.hedge_band = 1e12; c.risk_limit = 1e12; c.wait_cancel_ack = true;
  Full f(c);
  const InstrumentId s = f.id.stock;
  const double tick = f.u.insts[s].in.tick, half = f.u.insts[s].bg_half;
  const double fv = f.e.fair(s);
  const Rec* bid = f.v.live(s, Side::Buy);
  CHECK(bid != nullptr);
  const OrderId first = bid->id;
  f.e.on_book(s, book(fv - 20 * tick, half, tick, f.ts += 1000));  // bid must go
  CHECK(f.v.live(s, Side::Buy) == nullptr);                          // cancelled, not replaced yet
  CHECK(f.e.closing(s, Side::Buy) > 0);
  f.e.on_book(s, book(fv - 25 * tick, half, tick, f.ts += 1000));
  CHECK(f.v.live(s, Side::Buy) == nullptr);                          // still waiting
  f.e.on_cancel_done(s, first);
  const Rec* nb = f.v.live(s, Side::Buy);
  CHECK(nb && nb->id != first);                                      // replaced on the ack
}

// Order lines the runner sent to the gateway, parsed: {type, id, symbol, side, px, qty, tif}.
static std::vector<std::vector<std::string>> order_lines(const std::string& text) {
  std::vector<std::vector<std::string>> out;
  std::istringstream is(text);
  for (std::string l; std::getline(is, l);) {
    std::vector<std::string> f;
    std::stringstream ls(l);
    for (std::string x; std::getline(ls, x, ',');) f.push_back(x);
    out.push_back(f);
  }
  return out;
}

TEST(gateway_mode_round_trip) {
  LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/binance_perp.ini");
  std::ostringstream orders, state, log;
  RunnerOptions opt;
  opt.orders = &orders; opt.state = &state; opt.log = &log;
  opt.kill_file = std::string(AMM_SOURCE_DIR) + "/build/test_kill_file_that_does_not_exist";
  Runner r(cfg, opt);
  const std::string BTC = "BINANCE-PERP:BTCUSDT", ETH = "BINANCE-PERP:ETHUSDT";
  TimeNs ts = ns(1000.0);
  auto ev = [&](const std::string& line) { MdEvent e; CHECK(parse_event(line, e)); r.on_event(e); };
  auto t = [&](double dt = 0.01) { ts += ns(dt); return std::to_string(ts); };
  ev(t() + ",B," + BTC + ",65000,1,65000.1,1");
  ev(t() + ",B," + ETH + ",1800,10,1800.01,10");
  CHECK(!r.warm() && orders.str().empty());  // books alone are not enough: gateway not ready
  ev(t() + ",P," + ETH + ",0.5");           // the venue already holds 0.5 ETH
  ev(t() + ",G,ready");
  CHECK(r.warm());
  CHECK(std::fabs(r.position(1) - 0.5) < 1e-12);
  CHECK(std::fabs(r.engine().position(1) - 0.5) < 1e-12);
  auto lines = order_lines(orders.str());
  CHECK(lines.size() >= 2);
  std::string bid_id;
  for (const auto& f : lines) {
    CHECK(f.size() == 7 && f[0] == "N" && (f[6] == "POST" || f[6] == "IOC"));
    if (f[2] == BTC && f[3] == "B") bid_id = f[1];
  }
  CHECK(!bid_id.empty());
  // accepted, then filled in part: position moves, the order still rests
  ev(t() + ",A," + bid_id);
  double bid_px = 0;
  for (const auto& f : lines) if (f[1] == bid_id) bid_px = std::stod(f[4]);
  ev(t() + ",F," + bid_id + "," + std::to_string(bid_px) + ",0.002");
  CHECK(std::fabs(r.position(0) - 0.002) < 1e-12);
  CHECK(std::fabs(r.engine().position(0) - 0.002) < 1e-12);
  // our own resting size is taken out of the displayed book
  CHECK(r.gateway().own_at(0, Side::Buy, bid_px) > 0);
  // the venue drops it on its own: the engine requotes
  const size_t before = order_lines(orders.str()).size();
  ev(t() + ",D," + bid_id + ",expired");
  ev(t() + ",B," + BTC + ",65000,1,65000.1,1");
  CHECK(order_lines(orders.str()).size() > before);
  // reports for ids we never sent are counted, not acted on
  ev(t() + ",F,999999,65000,1");
  CHECK(r.gateway().unknown_reports() == 1 && std::fabs(r.position(0) - 0.002) < 1e-12);
  // state snapshots for the reconciler
  for (int k = 0; k < 20; ++k) ev(t(0.1) + ",B," + ETH + ",1800,10,1800.01,10");
  CHECK(state.str().find("\"positions\":{\"BTC-USDT-PERP\":0.002,\"ETH-USDT-PERP\":0.5}") != std::string::npos);
  CHECK(state.str().find("\"killed\":false") != std::string::npos);
  // the venue reports a position we do not hold: stop, cancel everything
  ev(t() + ",P," + BTC + ",0.05");
  CHECK(r.gate().killed());
  auto all = order_lines(orders.str());
  CHECK(!all.empty() && all.back()[0] == "K");
  bool cancels = false;
  for (const auto& f : all) if (f[0] == "X") cancels = true;
  CHECK(cancels);
  const size_t after_kill = all.size();
  for (int k = 0; k < 5; ++k) ev(t(0.2) + ",B," + BTC + ",64990,1,64990.1,1");
  size_t news = 0;
  auto again = order_lines(orders.str());
  for (size_t k = after_kill; k < again.size(); ++k) if (again[k][0] == "N") ++news;
  CHECK(news == 0);  // nothing new after the kill
  CHECK(log.str().find("KILL: position mismatch") != std::string::npos);
}

TEST(kill_file_stops_trading) {
  LiveConfig cfg = load_config(std::string(AMM_SOURCE_DIR) + "/config/binance_perp.ini");
  const std::string kf = std::string(AMM_SOURCE_DIR) + "/build/test_kill_file";
  std::remove(kf.c_str());
  std::ostringstream orders;
  RunnerOptions opt;
  opt.orders = &orders; opt.kill_file = kf;
  Runner r(cfg, opt);
  TimeNs ts = ns(1000.0);
  auto ev = [&](const std::string& line) { MdEvent e; CHECK(parse_event(line, e)); r.on_event(e); };
  auto t = [&]() { ts += ns(0.2); return std::to_string(ts); };
  ev(t() + ",G,ready");
  ev(t() + ",B,BINANCE-PERP:BTCUSDT,65000,1,65000.1,1");
  ev(t() + ",B,BINANCE-PERP:ETHUSDT,1800,10,1800.01,10");
  CHECK(r.warm() && !r.gate().killed());
  { std::ofstream f(kf); f << "stop\n"; }
  ev(t() + ",B,BINANCE-PERP:ETHUSDT,1800,10,1800.01,10");
  CHECK(r.gate().killed() && r.gate().kill_reason().find("kill file") == 0);
  std::remove(kf.c_str());
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
