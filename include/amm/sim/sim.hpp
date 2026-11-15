// Event-driven multi-asset market simulator.
//
// The world has latent risk factors (correlated diffusion + jumps). Every
// instrument's true value is its pricer applied to the true factors plus an
// idiosyncratic walk. Around that truth the simulator runs:
//   * background liquidity providers that requote each grid step, and after a
//     jump only with a lag (short on "fast" primary markets, long on "slow"
//     derivative markets);
//   * noise takers with exponentially distributed willingness to pay, which
//     produce the fill-intensity curve A*exp(-k*d) the literature assumes;
//   * snipers that pick off stale quotes after a jump, arriving before the
//     background repricing does;
//   * RFQ clients (partly informed) choosing the best of our price and
//     competing dealers, plus sparse noisy composite prices.
// The engine under test is connected through the same Venue interface and
// event callbacks it would use in production, with latency in both directions.
#pragma once
#include <cmath>
#include <functional>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "amm/engine.hpp"
#include "amm/sim/matching.hpp"

namespace amm::sim {

struct FactorDef {
  std::string name;
  double level, vol;         // vol per sqrt(s)
  double jump_rate, jump_sd; // jumps per second, jump size st.dev
};

struct InstDef {
  Instrument in;  // what the engine is told (priors may differ from the truth below)
  std::function<std::unique_ptr<Pricer>()> make_pricer;
  bool fast = true;          // primary market: repriced quickly after jumps
  double bg_half = 0.0, bg_size = 0.0;
  double taker_A = 0.0, taker_k = 1.0, taker_size = 0.0;
  double idio_vol_true = 0.0;
  // RFQ instruments
  double rfq_rate = 0.0;
  int n_comp = 3;
  double comp_margin = 0.0, comp_noise = 0.0, p_informed = 0.0;
  double composite_every_s = 20.0, composite_noise = 0.0;
};

struct Universe {
  struct Corr { FactorId a, b; double rho; };
  std::vector<FactorDef> factors;
  std::vector<Corr> corr;
  std::vector<InstDef> insts;
};

struct SimConfig {
  uint64_t seed = 1;
  double duration_s = 600.0;
  double dt = 0.05;
  double md_lat_s = 100e-6, ord_lat_s = 100e-6;
  double snipe_fast_s = 50e-6, bg_fast_s = 1e-3;
  double snipe_slow_s = 3e-3, bg_slow_s = 5e-3;
  double p_snipe = 0.7;
  double timer_s = 0.1;
};

struct SimResult {
  double pnl = 0.0, pnl_sd_1s = 0.0, sharpe = 0.0, max_drawdown = 0.0;
  uint64_t fills = 0, hedge_fills = 0, sniped_fills = 0, rfq = 0, rfq_won = 0;
  double sniped_loss = 0.0, hedge_cost = 0.0, volume = 0.0;
  std::vector<double> pnl_path, inst_pnl, inst_sniped, inst_edge;
};

inline void build_engine(const Universe& u, Engine& eng) {
  for (const auto& f : u.factors) eng.factors().add(f.name, f.level, f.vol);
  for (const auto& c : u.corr) eng.factors().set_corr(c.a, c.b, c.rho);
  for (const auto& d : u.insts) {
    Instrument in = d.in;
    in.pricer = eng.add_pricer(d.make_pricer());
    eng.add_instrument(in);
  }
}

class Sim final : public Venue {
 public:
  Sim(const Universe& u, SimConfig cfg) : u_(u), cfg_(cfg), rng_(cfg.seed) {
    const size_t nf = u.factors.size();
    for (const auto& f : u.factors) truth_.add(f.name, f.level, f.vol);
    for (const auto& c : u.corr) truth_.set_corr(c.a, c.b, c.rho);
    truth_.adapt_vol = false;
    chol_.assign(nf * nf, 0.0);  // Cholesky of the factor covariance
    for (size_t i = 0; i < nf; ++i)
      for (size_t j = 0; j <= i; ++j) {
        double s = truth_.cov(i, j);
        for (size_t k = 0; k < j; ++k) s -= chol_[i * nf + k] * chol_[j * nf + k];
        chol_[i * nf + j] = i == j ? std::sqrt(std::max(s, 0.0)) : s / chol_[j * nf + j];
      }
    deps_.assign(nf, {});
    for (size_t i = 0; i < u.insts.size(); ++i) {
      const auto& d = u.insts[i];
      Inst x;
      x.pricer = d.make_pricer();
      x.book = MatchBook(d.in.tick);
      x.next_composite = d.composite_every_s * uni();
      const PriceOut o = x.pricer->price(truth_.levels(), 0.0);
      for (uint8_t j = 0; j < o.ex.n; ++j) deps_[o.ex.e[j].f].push_back(static_cast<InstrumentId>(i));
      inst_.push_back(std::move(x));
    }
  }

  void attach(Engine* e) { eng_ = e; }
  // Observer of the public market (books excluding the engine's own orders,
  // trade prints, composites): type 'B' (bid, bid_sz, ask, ask_sz), 'T' (px,
  // qty, +1/-1 aggressor), 'C' (px). With no engine attached the simulator
  // runs the market alone, which is how sample journals are recorded.
  std::function<void(TimeNs, char, InstrumentId, double, double, double, double)> tap;
  double now_s() const { return secs(now_); }
  double true_px(InstrumentId i) const {
    return inst_[i].pricer->price(truth_.levels(), secs(now_) / secs_per_year).px + inst_[i].idio;
  }
  const FactorModel& truth() const { return truth_; }
  double mm_pos(InstrumentId i) const { return inst_[i].pos; }
  double mm_pnl() const {
    double v = 0.0;
    for (size_t i = 0; i < inst_.size(); ++i) v += inst_pnl(static_cast<InstrumentId>(i));
    return v;
  }
  double inst_pnl(InstrumentId i) const {
    return inst_[i].cash + inst_[i].pos * u_.insts[i].in.mult * true_px(i);
  }

  SimResult run() {
    res_.inst_sniped.assign(inst_.size(), 0.0);
    res_.inst_edge.assign(inst_.size(), 0.0);
    if (eng_) eng_->start(0);
    at(0, [this] { grid(0); });
    const TimeNs end = ns(cfg_.duration_s);
    while (!q_.empty()) {
      Ev ev = std::move(const_cast<Ev&>(q_.top()));
      q_.pop();
      if (ev.t > end) break;
      now_ = ev.t;
      ev.fn();
    }
    now_ = end;
    res_.pnl = mm_pnl();
    for (size_t i = 0; i < inst_.size(); ++i) res_.inst_pnl.push_back(inst_pnl(static_cast<InstrumentId>(i)));
    double m = 0, v = 0, peak = 0;
    const auto& p = res_.pnl_path;
    const size_t n = p.size() > 1 ? p.size() - 1 : 0;
    for (size_t i = 1; i < p.size(); ++i) m += (p[i] - p[i - 1]);
    if (n) m /= n;
    for (size_t i = 1; i < p.size(); ++i) { const double d = p[i] - p[i - 1] - m; v += d * d; }
    for (double x : p) { peak = std::max(peak, x); res_.max_drawdown = std::max(res_.max_drawdown, peak - x); }
    res_.pnl_sd_1s = n > 1 ? std::sqrt(v / (n - 1)) : 0.0;
    res_.sharpe = res_.pnl_sd_1s > 0 ? m / res_.pnl_sd_1s * std::sqrt(static_cast<double>(n)) : 0.0;
    return res_;
  }

  // ---- Venue (called by the engine) --------------------------------------
  OrderId submit(InstrumentId i, Side side, double px, double qty, bool ioc, bool hedge) override {
    const OrderId id = (++next_id_) | (hedge ? kHedgeTag : 0);
    at(now_ + ns(cfg_.ord_lat_s), [=, this] {
      std::vector<Exec> ex;
      inst_[i].book.add(id, kMM, side, inst_[i].book.to_ticks(px), qty, ioc, ex);
      for (const auto& e : ex) mm_fill(i, side, inst_[i].book.to_px(e.px_ticks), e.qty, id, hedge, false);
      publish(i);
    });
    return id;
  }
  void cancel(InstrumentId i, OrderId id) override {
    at(now_ + ns(cfg_.ord_lat_s), [=, this] {
      inst_[i].book.cancel(id);
      // Confirmation reaches the engine after market-data latency, behind any fill notice.
      at(now_ + ns(cfg_.md_lat_s), [=, this] { eng_->on_cancel_done(i, id); });
    });
  }

 private:
  static constexpr int kBG = 0, kMM = 1, kTaker = 2;
  static constexpr double secs_per_year = 252.0 * 6.5 * 3600.0;
  struct Ev {
    TimeNs t; uint64_t seq; std::function<void()> fn;
    bool operator<(const Ev& o) const { return t > o.t || (t == o.t && seq > o.seq); }
  };
  struct Inst {
    std::unique_ptr<Pricer> pricer;
    MatchBook book;
    std::vector<OrderId> bg;
    TopOfBook pub;
    double idio = 0.0, pos = 0.0, cash = 0.0, next_composite = 0.0;
  };

  void at(TimeNs t, std::function<void()> fn) { q_.push(Ev{t, seq_++, std::move(fn)}); }
  double uni() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
  double gauss() { return std::normal_distribution<double>(0.0, 1.0)(rng_); }
  double expo(double rate) { return -std::log(1.0 - uni()) / rate; }
  int poisson(double mean) { return mean > 0 ? std::poisson_distribution<int>(mean)(rng_) : 0; }

  void grid(uint64_t k) {
    const size_t nf = truth_.size();
    const double dt = cfg_.dt, sq = std::sqrt(dt);
    if (k > 0) {
      std::vector<double> z(nf);
      for (auto& x : z) x = gauss();
      for (size_t i = 0; i < nf; ++i) {
        double d = 0.0;
        for (size_t j = 0; j <= i; ++j) d += chol_[i * nf + j] * z[j];
        truth_.set_level(i, truth_.level(i) + d * sq);
      }
      for (size_t i = 0; i < inst_.size(); ++i)
        if (u_.insts[i].idio_vol_true > 0) inst_[i].idio += u_.insts[i].idio_vol_true * sq * gauss();
    }
    for (size_t f = 0; f < nf; ++f)
      for (int n = poisson(u_.factors[f].jump_rate * dt); n > 0; --n)
        at(now_ + ns(uni() * dt), [this, f] { jump(static_cast<FactorId>(f)); });
    for (size_t i = 0; i < inst_.size(); ++i) {
      const auto& d = u_.insts[i];
      const InstrumentId id = static_cast<InstrumentId>(i);
      if (d.in.proto == Protocol::Clob) {
        bg_reprice(id);
        for (Side s : {Side::Buy, Side::Sell})
          for (int n = poisson(d.taker_A * dt); n > 0; --n)
            at(now_ + ns(uni() * dt), [this, id, s] { taker(id, s); });
      } else {
        for (int n = poisson(d.rfq_rate * dt); n > 0; --n)
          at(now_ + ns(uni() * dt), [this, id] { rfq(id); });
        if (secs(now_) >= inst_[i].next_composite) {
          inst_[i].next_composite += d.composite_every_s;
          const double cpx = true_px(id) + d.composite_noise * gauss();
          if (tap) tap(now_, 'C', id, cpx, 0, 0, 0);
          if (eng_) eng_->on_composite(id, cpx, now_);
        }
      }
    }
    const uint64_t timer_steps = std::max<uint64_t>(1, std::llround(cfg_.timer_s / dt));
    if (eng_ && k % timer_steps == 0) at(now_ + ns(cfg_.md_lat_s), [this] { eng_->on_timer(now_); });
    const uint64_t sec_steps = std::max<uint64_t>(1, std::llround(1.0 / dt));
    if (k % sec_steps == 0) res_.pnl_path.push_back(mm_pnl());
    at(ns((k + 1) * dt), [this, k] { grid(k + 1); });
  }

  void jump(FactorId f) {
    double lv = truth_.level(f) + u_.factors[f].jump_sd * gauss();
    if (lv < 0.2 * u_.factors[f].level) lv = 0.2 * u_.factors[f].level;
    truth_.set_level(f, lv);
    for (InstrumentId i : deps_[f]) {
      const auto& d = u_.insts[i];
      if (d.in.proto != Protocol::Clob) continue;
      if (uni() < cfg_.p_snipe)
        at(now_ + ns(d.fast ? cfg_.snipe_fast_s : cfg_.snipe_slow_s), [this, i] { snipe(i); });
      at(now_ + ns(d.fast ? cfg_.bg_fast_s : cfg_.bg_slow_s), [this, i] { bg_reprice(i); });
    }
  }

  void bg_reprice(InstrumentId i) {
    Inst& x = inst_[i];
    const auto& d = u_.insts[i];
    for (OrderId id : x.bg) x.book.cancel(id);
    x.bg.clear();
    const double p = true_px(i), t = d.in.tick;
    std::vector<Exec> ex;
    auto post = [&](Side s, double px, double sz) {
      const OrderId id = ++next_id_;
      ex.clear();
      x.book.add(id, kBG, s, x.book.to_ticks(px), std::max(d.in.lot, floor_to(sz, d.in.lot)), false, ex);
      settle(i, s, ex, false);
      x.bg.push_back(id);
    };
    post(Side::Buy, floor_to(p - d.bg_half, t), d.bg_size * (0.5 + uni()));
    post(Side::Sell, ceil_to(p + d.bg_half, t), d.bg_size * (0.5 + uni()));
    post(Side::Buy, floor_to(p - 3 * d.bg_half, t), 10 * d.bg_size);
    post(Side::Sell, ceil_to(p + 3 * d.bg_half, t), 10 * d.bg_size);
    publish(i);
  }

  void taker(InstrumentId i, Side s) {
    Inst& x = inst_[i];
    const auto& d = u_.insts[i];
    const double p = true_px(i), w = expo(d.taker_k);
    const double lim = s == Side::Buy ? floor_to(p + w, d.in.tick) : ceil_to(p - w, d.in.tick);
    const double qty = std::max(d.in.lot, floor_to(d.taker_size * (0.5 + uni()), d.in.lot));
    std::vector<Exec> ex;
    x.book.add(++next_id_, kTaker, s, x.book.to_ticks(lim), qty, true, ex);
    settle(i, s, ex, false);
    publish(i);
  }

  // Latency arbitrageur: lifts every quote that is through the new true value.
  void snipe(InstrumentId i) {
    Inst& x = inst_[i];
    const auto& d = u_.insts[i];
    const double p = true_px(i), t = d.in.tick;
    std::vector<Exec> ex;
    x.book.add(++next_id_, kTaker, Side::Buy, x.book.to_ticks(floor_to(p, t)) - 1, 1e12, true, ex);
    settle(i, Side::Buy, ex, true);
    ex.clear();
    x.book.add(++next_id_, kTaker, Side::Sell, x.book.to_ticks(ceil_to(p, t)) + 1, 1e12, true, ex);
    settle(i, Side::Sell, ex, true);
    publish(i);
  }

  void settle(InstrumentId i, Side taker_side, const std::vector<Exec>& ex, bool sniped) {
    for (const auto& e : ex) {
      if (tap) tap(now_, 'T', i, inst_[i].book.to_px(e.px_ticks), e.qty, sgn(taker_side), 0);
      if (e.maker_owner == kMM)
        mm_fill(i, opp(taker_side), inst_[i].book.to_px(e.px_ticks), e.qty, e.maker_id, false, sniped);
    }
  }

  void mm_fill(InstrumentId i, Side side, double px, double qty, OrderId oid, bool hedge, bool sniped) {
    Inst& x = inst_[i];
    const double mult = u_.insts[i].in.mult, sg = sgn(side);
    x.pos += sg * qty;
    x.cash -= sg * qty * px * mult;
    const double edge = sg * (true_px(i) - px) * qty * mult;
    res_.volume += qty * px * mult;
    if (hedge) { ++res_.hedge_fills; res_.hedge_cost -= edge; }
    else ++res_.fills;
    if (sniped) { ++res_.sniped_fills; res_.sniped_loss -= edge; res_.inst_sniped[i] -= edge; }
    if (!hedge) res_.inst_edge[i] += edge;
    Fill f{i, side, px, qty, 0, oid, hedge};
    at(now_ + ns(cfg_.md_lat_s), [this, f]() mutable { f.ts = now_; eng_->on_fill(f); });
  }

  void publish(InstrumentId i) {
    Inst& x = inst_[i];
    TopOfBook t = x.book.top_excluding(kMM);
    if (t.bid == x.pub.bid && t.ask == x.pub.ask && t.bid_sz == x.pub.bid_sz && t.ask_sz == x.pub.ask_sz) return;
    x.pub = t;
    if (tap && t.valid()) tap(now_, 'B', i, t.bid, t.bid_sz, t.ask, t.ask_sz);
    if (eng_) at(now_ + ns(cfg_.md_lat_s), [this, i, t]() mutable { t.ts = now_; eng_->on_book(i, t); });
  }

  void rfq(InstrumentId i) {
    const auto& d = u_.insts[i];
    const double p = true_px(i);
    const Side cs = uni() < 0.5 ? Side::Buy : Side::Sell;
    const double qty = d.in.quote_size * (uni() < 0.7 ? 1.0 : 2.0);
    const bool informed = uni() < d.p_informed;
    const double sg = sgn(cs);  // client buys: lower price is better for them
    double best = sg * 1e300;
    for (int c = 0; c < d.n_comp; ++c) {
      const double q = p + d.comp_noise * gauss() + sg * d.comp_margin;
      best = cs == Side::Buy ? std::min(best, q) : std::max(best, q);
    }
    ++res_.rfq;
    if (!eng_) return;
    const auto ours = eng_->on_rfq(i, cs, qty, now_);
    if (!ours) return;
    const bool better = cs == Side::Buy ? *ours < best : *ours > best;
    const bool has_edge = cs == Side::Buy ? *ours < p - d.in.tick : *ours > p + d.in.tick;
    const bool won = better && (!informed || has_edge);
    eng_->on_rfq_result(i, cs, *ours, won);
    if (won) {
      ++res_.rfq_won;
      mm_fill(i, opp(cs), *ours, qty, 0, false, false);
    }
  }

  const Universe& u_;
  SimConfig cfg_;
  std::mt19937_64 rng_;
  FactorModel truth_;
  std::vector<double> chol_;
  std::vector<std::vector<InstrumentId>> deps_;
  std::vector<Inst> inst_;
  std::priority_queue<Ev> q_;
  uint64_t seq_ = 0;
  OrderId next_id_ = 0;
  TimeNs now_ = 0;
  Engine* eng_ = nullptr;
  SimResult res_;
};

}  // namespace amm::sim
