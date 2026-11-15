// Reference cross-asset universe used by the simulator, tests and benchmark:
// cash equity, listed options, a covered warrant with a quoting obligation,
// crypto spot and perpetual, a rate future, a credit index, and two corporate
// bonds traded by RFQ. All volatilities are per sqrt(second).
#pragma once
#include "amm/sim/sim.hpp"

namespace amm::sim {

struct Ids {
  FactorId EQ = 0, VOL = 1, BTC = 2, RATE = 3, CREDIT = 4;
  InstrumentId stock = 0, c100 = 1, p100 = 2, c110 = 3, p90 = 4, warrant = 5,
               btc = 6, perp = 7, fut = 8, cdx = 9, bond_a = 10, bond_b = 11;
};

inline Universe default_universe() {
  Universe u;
  u.insts.reserve(32);  // pointers returned by the helpers below stay valid
  //            name      level    vol      jumps/s   jump sd
  u.factors = {{"EQ",     100.0,   0.0124,  1.0 / 60,  0.15},
               {"VOL",    0.30,    1.2e-4,  1.0 / 120, 0.004},
               {"BTC",    60000.0, 6.4,     1.0 / 60,  40.0},
               {"RATE",   400.0,   0.033,   1.0 / 120, 1.0},
               {"CREDIT", 120.0,   0.0165,  1.0 / 120, 1.5}};
  u.corr = {{0, 1, -0.7}, {0, 2, 0.3}, {0, 4, -0.5}, {3, 4, -0.2}};
  const FactorId EQ = 0, VOL = 1, BTC = 2, RATE = 3, CREDIT = 4;

  auto clob = [&](std::string sym, AssetClass cls, double tick, double lot, double mult,
                  double qsize, double max_pos, double A, double k, double bg_half,
                  double bg_size, bool fast, std::function<std::unique_ptr<Pricer>()> mk) {
    InstDef d;
    d.in.symbol = std::move(sym); d.in.cls = cls; d.in.proto = Protocol::Clob;
    d.in.tick = tick; d.in.lot = lot; d.in.mult = mult;
    d.in.quote_size = qsize; d.in.max_pos = max_pos;
    d.in.A0 = 0.7 * A; d.in.k0 = 1.3 * k;  // engine starts from deliberately wrong priors
    d.taker_A = A; d.taker_k = k; d.taker_size = qsize;
    d.bg_half = bg_half; d.bg_size = bg_size; d.fast = fast;
    d.make_pricer = std::move(mk);
    u.insts.push_back(std::move(d));
    return &u.insts.back();
  };
  auto opt = [&](std::string sym, double K, double T, bool call) {
    return clob(std::move(sym), AssetClass::Option, 0.01, 1, 100, 10, 200, 0.5, 40, 0.05, 50, false,
                [=] { return std::make_unique<OptionPricer>(EQ, VOL, K, T, call, 1.0, -0.1); });
  };

  auto* s = clob("ACME", AssetClass::Equity, 0.01, 1, 1, 100, 50000, 2.0, 80, 0.03, 500, true,
                 [=] { return std::make_unique<LinearPricer>(EQ); });
  s->in.ref_for_factor = EQ; s->in.hedges_factor = EQ;

  auto* o = opt("ACME C100 3M", 100, 0.25, true);
  o->in.ref_for_factor = VOL; o->in.ref_gain = 0.3;
  opt("ACME P100 3M", 100, 0.25, false);
  opt("ACME C110 6M", 110, 0.50, true);
  opt("ACME P90 6M", 90, 0.50, false);

  auto* w = clob("ACME WNT C110 1Y", AssetClass::Warrant, 0.001, 100, 1, 10000, 200000, 0.5, 400,
                 0.006, 50000, false,
                 [=] { return std::make_unique<OptionPricer>(EQ, VOL, 110, 1.0, true, 0.1, -0.1); });
  w->in.max_spread = 0.02;

  auto* b = clob("BTC-USD", AssetClass::CryptoSpot, 0.5, 0.001, 1, 0.05, 5, 2.0, 0.2, 8, 0.5, true,
                 [=] { return std::make_unique<LinearPricer>(BTC); });
  b->in.ref_for_factor = BTC; b->in.hedges_factor = BTC;
  auto* p = clob("BTC-PERP", AssetClass::CryptoPerp, 0.5, 0.001, 1, 0.05, 2, 2.0, 0.2, 8, 0.5, false,
                 [=] { return std::make_unique<LinearPricer>(BTC, 12.0); });
  p->idio_vol_true = 0.3; p->in.idio_vol = 0.3;

  auto* f = clob("RATE FUT", AssetClass::Future, 0.005, 1, 1000, 5, 500, 1.0, 250, 0.0075, 50, true,
                 [=] { return std::make_unique<LinearPricer>(RATE, 110.0, -0.08, 400.0); });
  f->in.ref_for_factor = RATE; f->in.hedges_factor = RATE;
  auto* c = clob("CDX IG", AssetClass::CreditIndex, 0.0025, 1, 10000, 1, 100, 0.5, 400, 0.005, 10, true,
                 [=] { return std::make_unique<LinearPricer>(CREDIT, 100.0, -0.045, 120.0); });
  c->in.ref_for_factor = CREDIT; c->in.hedges_factor = CREDIT;

  auto bond = [&](std::string sym, double cpn, double mat, double beta) {
    InstDef d;
    d.in.symbol = std::move(sym); d.in.cls = AssetClass::Bond; d.in.proto = Protocol::Rfq;
    d.in.tick = 0.005; d.in.lot = 1; d.in.mult = 10000;  // 1 unit = 1mm face
    d.in.quote_size = 1; d.in.max_pos = 20;
    d.in.A0 = 0.2; d.in.rfq_a0 = -2.4; d.in.rfq_b0 = 60;
    d.in.idio_vol = 0.0008; d.idio_vol_true = 0.0008;
    d.rfq_rate = 0.2; d.n_comp = 3; d.comp_margin = 0.06; d.comp_noise = 0.02;
    d.p_informed = 0.25; d.composite_every_s = 20; d.composite_noise = 0.02;
    d.make_pricer = [=] { return std::make_unique<BondPricer>(RATE, CREDIT, cpn, mat, beta); };
    u.insts.push_back(std::move(d));
  };
  bond("ACME 5.0 2034", 5.0, 8.0, 1.0);
  bond("ACME 3.5 2030", 3.5, 4.0, 1.5);
  return u;
}

enum class Mode { Naive, Siloed, FairOnly, SkewOnly, Unified };
inline const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Naive: return "naive";
    case Mode::Siloed: return "siloed";
    case Mode::FairOnly: return "+fair";
    case Mode::SkewOnly: return "+skew";
    default: return "unified";
  }
}
// Naive:    fixed fraction of the market spread, no inventory skew.
// Siloed:   optimal single-instrument quoting (GLFT with online calibration),
//           each instrument priced off its own market and skewed on its own
//           inventory only. Factor exposure is still hedged net.
// FairOnly: siloed + cross-asset factor-model fair values.
// SkewOnly: siloed + skew on covariance with the whole portfolio.
// Unified:  both - the full engine.
inline EngineConfig mode_config(Mode m) {
  EngineConfig c;
  if (m == Mode::Naive) {
    c.glft = false; c.adapt_tox = false; c.adapt_intensity = false;
  }
  c.cross_asset_fair = m == Mode::FairOnly || m == Mode::Unified;
  c.portfolio_skew = m == Mode::SkewOnly || m == Mode::Unified;
  return c;
}

}  // namespace amm::sim
