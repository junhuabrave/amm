// The market-making engine: a single-threaded, deterministic state machine.
// Inputs are events (book update, fill, timer, RFQ, composite price); outputs
// are orders on a Venue and RFQ prices. Same inputs in the same order always
// produce the same outputs, which is what makes replay and backtests exact.
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "amm/core.hpp"
#include "amm/factor.hpp"
#include "amm/linalg.hpp"
#include "amm/pricing.hpp"
#include "amm/quoter.hpp"
#include "amm/signals.hpp"

namespace amm {

struct EngineConfig {
  double gamma = 0.05;            // risk aversion, 1/currency. One number for all assets.
  bool glft = true;               // false: fixed fraction of market spread, no skew
  bool cross_asset_fair = true;   // fair = model(factors) + basis; false: own market only
  bool portfolio_skew = true;     // skew on covariance with whole book; false: own inventory
  bool adapt_tox = true;          // widen by measured adverse selection
  bool adapt_intensity = true;    // learn fill-intensity curve online
  double naive_half_frac = 0.6;   // glft=false: our half-spread / market half-spread
  double basis_tau_s = 2.0;       // speed at which model-vs-market basis is absorbed
  double tox_coef = 1.0;
  double skew_cap = 2.5;          // |skew| <= skew_cap * half-spread
  double hedge_band = 40.0;       // per-factor risk (currency / sqrt s) that triggers a hedge
  double hedge_target_frac = 0.5; // hedge back to this fraction of the band
  double hedge_cooldown_s = 0.02;
  int hedge_slip_ticks = 2;
  double risk_limit = 400.0;      // portfolio st.dev (currency / sqrt s): beyond it only reduce
  double max_loss = 1e9;          // kill switch on mark-to-fair P&L
  int requote_ticks = 1;          // price change that forces a cancel/replace
  bool wait_cancel_ack = false;   // no new order on a side while a cancel there is unconfirmed
  double min_quote_life_s = 0.0;  // a younger quote is kept unless it must go (see reconcile)
  double secs_per_year = 252.0 * 6.5 * 3600.0;
};

struct EngineStats {
  uint64_t book_events = 0, fills = 0, hedge_fills = 0, orders = 0, cancels = 0;
  uint64_t refused = 0;       // quotes the venue (or a risk gate in front of it) refused to send
  uint64_t rfq_seen = 0, rfq_quoted = 0, rfq_won = 0;
  double spread_pnl = 0.0;   // edge vs fair captured on quote fills
  double hedge_pnl = 0.0;    // edge vs fair paid on hedges (negative)
  double max_risk = 0.0, risk_time = 0.0, risk_integral = 0.0;
};

class Engine {
 public:
  Engine(EngineConfig cfg, Venue* venue) : cfg_(cfg), venue_(venue) {}

  FactorModel& factors() { return fm_; }
  const FactorModel& factors() const { return fm_; }
  int add_pricer(std::unique_ptr<Pricer> p) {
    pricers_.push_back(std::move(p));
    return static_cast<int>(pricers_.size()) - 1;
  }
  InstrumentId add_instrument(Instrument in) {
    in.id = static_cast<InstrumentId>(inst_.size());
    State s;
    s.inten.init(in.A0, in.k0);
    s.win.init(in.rfq_a0, in.rfq_b0);
    inst_.push_back(std::move(in));
    st_.push_back(s);
    return inst_.back().id;
  }

  void start(TimeNs ts) {
    now_ = t0_ = last_timer_ = ts;
    const size_t nf = fm_.size();
    E_.assign(nf, 0.0); SE_.assign(nf, 0.0); WE_.assign(nf, 0.0); W_.assign(nf * nf, 0.0);
    hedge_inst_.assign(nf, kNoInstrument);
    hedge_block_.assign(nf, 0);
    deps_.assign(nf, {});
    lead_deps_.assign(nf, {});
    lead_inst_deps_.assign(inst_.size(), {});
    for (auto& in : inst_) {
      if (in.hedges_factor != kNoFactor) hedge_inst_[in.hedges_factor] = in.id;
      reprice(in.id);
      for (uint8_t j = 0; j < st_[in.id].ex.n; ++j) deps_[st_[in.id].ex.e[j].f].push_back(in.id);
      st_[in.id].fair = st_[in.id].model;
      st_[in.id].dirty = true;
      if (in.lead_gain != 0.0) {
        if (in.lead_inst != kNoInstrument) lead_inst_deps_[in.lead_inst].push_back(in.id);
        else if (in.lead_factor != kNoFactor) lead_deps_[in.lead_factor].push_back(in.id);
      }
    }
    refresh_liquidity();
    refresh_risk();
  }

  // ---- inbound events ----------------------------------------------------

  void on_book(InstrumentId id, const TopOfBook& top) {
    now_ = std::max(now_, top.ts);
    ++stats_.book_events;
    State& s = st_[id];
    const Instrument& in = inst_[id];
    const TimeNs prev_ts = s.top.ts;
    s.top = top;
    if (!top.valid()) { s.dirty = true; flush(); return; }
    const double mp = s.micro.update(top);
    // 1. If this market is the observation of a factor, move the factor so the
    //    model reprices this instrument to its own market (one damped Newton
    //    step), then reprice everything else that depends on that factor.
    if (in.ref_for_factor != kNoFactor) {
      const FactorId f = in.ref_for_factor;
      const double d = s.ex.to(f);
      if (std::fabs(d) > 1e-12) {
        const double lv = fm_.level(f) + in.ref_gain * (mp - s.basis - s.model) / d;
        fm_.observe(f, lv, top.ts);
        for (InstrumentId j : deps_[f]) { reprice(j); st_[j].dirty = true; }
      }
    }
    // 2. Absorb what the model does not explain into a slow basis.
    const double dt = prev_ts > 0 ? std::max(0.0, secs(top.ts - prev_ts)) : 1e9;
    const double w = 1.0 - std::exp(-dt / cfg_.basis_tau_s);
    s.basis += w * (mp - s.model - s.basis);
    // 3. Lead-lag: followers of a factor this market observes, and this market
    //    if it follows one.
    if (in.ref_for_factor != kNoFactor)
      for (InstrumentId j : lead_deps_[in.ref_for_factor]) update_lead(j);
    for (InstrumentId j : lead_inst_deps_[id]) update_lead(j);
    if ((in.lead_factor != kNoFactor || in.lead_inst != kNoInstrument) && in.lead_gain != 0.0) update_lead(id);
    s.dirty = true;
    flush();
  }

  // Indicative/composite price for instruments with no order book.
  void on_composite(InstrumentId id, double px, TimeNs ts) {
    now_ = std::max(now_, ts);
    State& s = st_[id];
    reprice(id);
    s.composite = px;
    s.basis += 0.6 * (px - s.model - s.basis);
    s.tox.settle(now_, fair(id));
    s.dirty = true;
  }

  // Take over a position that exists at the venue (a restart, or one opened
  // outside this session), valued at `px`: P&L counts from here.
  void seed_position(InstrumentId id, double qty, double px) {
    State& s = st_[id];
    cash_ -= (qty - s.pos) * px * inst_[id].mult;
    s.pos = qty;
    refresh_risk();
    for (auto& x : st_) x.dirty = true;
  }

  // The venue reports that an order can no longer fill: a cancel took effect,
  // or the venue cancelled, expired or rejected it on its own. Until a
  // cancel is confirmed the order can still fill, so its remaining size
  // counts against the position limit.
  void on_cancel_done(InstrumentId id, OrderId oid) {
    for (Live* l : {&st_[id].bid, &st_[id].ask})
      if (l->id == oid && oid != 0) {  // not cancelled by us: the quote is simply gone
        *l = Live{};
        st_[id].dirty = true;
        flush();
        return;
      }
    for (size_t k = 0; k < closing_.size(); ++k)
      if (closing_[k].id == oid) {
        State& s = st_[closing_[k].inst];
        (closing_[k].side == Side::Buy ? s.closing_bid : s.closing_ask) -= closing_[k].qty;
        closing_[k] = closing_.back();
        closing_.pop_back();
        st_[id].dirty = true;
        flush();
        return;
      }
  }
  // Size of cancelled orders not yet confirmed gone, per side.
  double closing(InstrumentId id, Side side) const {
    return side == Side::Buy ? st_[id].closing_bid : st_[id].closing_ask;
  }

  void on_fill(const Fill& f) {
    now_ = std::max(now_, f.ts);
    State& s = st_[f.inst];
    const Instrument& in = inst_[f.inst];
    const double sg = sgn(f.side), fv = fair(f.inst);
    s.pos += sg * f.qty;
    cash_ -= sg * f.qty * f.px * in.mult;
    const double edge = sg * (fv - f.px) * f.qty * in.mult;
    if (f.hedge) { stats_.hedge_pnl += edge; ++stats_.hedge_fills; s.hedge_pos += sg * f.qty; }
    else {
      stats_.spread_pnl += edge; ++stats_.fills;
      s.tox.record(f.ts, f.side, fv);
      if (in.proto == Protocol::Clob) s.inten.fill(sg * (fv - f.px));
    }
    Live& l = f.side == Side::Buy ? s.bid : s.ask;
    if (l.id == f.oid && f.oid != 0) {
      l.qty -= f.qty;
      if (l.qty < 0.5 * in.lot) l = Live{};
    } else if (f.oid != 0) {
      for (size_t k = 0; k < closing_.size(); ++k)
        if (closing_[k].id == f.oid) {
          const double q = std::min(f.qty, closing_[k].qty);
          closing_[k].qty -= q;
          (f.side == Side::Buy ? s.closing_bid : s.closing_ask) -= q;
          break;
        }
    }
    refresh_risk();
    for (auto& x : st_) x.dirty = true;  // inventory moved: every skew may change
    hedge();
    flush();
  }

  void on_timer(TimeNs ts) {
    now_ = std::max(now_, ts);
    const double dt = secs(now_ - last_timer_);
    last_timer_ = now_;
    for (size_t i = 0; i < inst_.size(); ++i) {
      State& s = st_[i];
      reprice(static_cast<InstrumentId>(i));  // greeks and time decay
      const double fv = fair(static_cast<InstrumentId>(i));
      s.tox.settle(now_, fv);
      if (cfg_.adapt_intensity && inst_[i].proto == Protocol::Clob && fv == fv) {
        if (s.bid.id) s.inten.accrue(fv - s.bid.px, dt);
        if (s.ask.id) s.inten.accrue(s.ask.px - fv, dt);
        s.inten.fit(dt);
      }
      s.dirty = true;
    }
    refresh_liquidity();
    refresh_risk();
    stats_.risk_time += dt;
    stats_.risk_integral += risk_ * dt;
    if (!halted_ && pnl() < -cfg_.max_loss) halt();
    hedge();
    flush();
  }

  // Client asks for a price. client_side is what the client wants to do.
  // Returns the price we show, or nothing if we decline.
  std::optional<double> on_rfq(InstrumentId id, Side client_side, double qty, TimeNs ts) {
    now_ = std::max(now_, ts);
    ++stats_.rfq_seen;
    State& s = st_[id];
    const Instrument& in = inst_[id];
    if (halted_) return std::nullopt;
    reprice(id);
    const double fv = fair(id);
    if (!(fv == fv)) return std::nullopt;
    const Side our = opp(client_side);
    const double sg = sgn(our);
    if (std::fabs(s.pos + sg * qty) > in.max_pos + 1e-9) return std::nullopt;
    if (risk_ > cfg_.risk_limit && sg * s.m > 0) return std::nullopt;
    double margin;
    if (!cfg_.glft) {
      margin = 1.0 / s.win.b();
    } else {
      const double de = qty * in.mult;
      const Glft g = glft(cfg_.gamma, in.A0, s.win.b(), de);
      const Risk r = risk_terms(id, g.C);
      // Cost of the trade relative to fair: size risk, inventory (sign makes
      // risk-reducing trades cheap), measured adverse selection, fees.
      const double c = 0.5 * de * r.self + sg * r.skew + in.fee +
                       (cfg_.adapt_tox ? cfg_.tox_coef * s.tox.adverse() : 0.0);
      margin = rfq_margin(s.win.a(), s.win.b(), c);
    }
    const double raw = fv - sg * margin;  // we buy below fair, sell above
    ++stats_.rfq_quoted;
    return our == Side::Buy ? floor_to(raw, in.tick) : ceil_to(raw, in.tick);
  }

  void on_rfq_result(InstrumentId id, Side client_side, double px, bool won) {
    State& s = st_[id];
    const double margin = sgn(client_side) * (px - fair(id));
    s.win.learn(margin, won);
    if (won) ++stats_.rfq_won;
  }

  // ---- inspection --------------------------------------------------------

  double fair(InstrumentId id) const {
    const State& s = st_[id];
    if (cfg_.cross_asset_fair) return s.model + s.basis + s.lead_adj;
    if (inst_[id].proto == Protocol::Clob) return s.micro.ready() ? s.micro.fair() : s.model;
    return s.composite == s.composite ? s.composite : s.model;
  }
  double position(InstrumentId id) const { return st_[id].pos; }
  double sigma(InstrumentId id) const { return st_[id].sigma; }
  double marginal_risk(InstrumentId id) const { return st_[id].m; }
  double factor_exposure(FactorId f) const { return E_[f]; }
  double risk() const { return risk_; }
  double cash() const { return cash_; }
  double pnl() const {
    double v = cash_;
    for (size_t i = 0; i < inst_.size(); ++i) {
      const double fv = fair(static_cast<InstrumentId>(i));
      if (st_[i].pos != 0.0 && fv == fv) v += st_[i].pos * inst_[i].mult * fv;
    }
    return v;
  }
  bool halted() const { return halted_; }
  const EngineStats& stats() const { return stats_; }
  const Instrument& instrument(InstrumentId id) const { return inst_[id]; }
  size_t instruments() const { return inst_.size(); }
  const IntensityEstimator& intensity(InstrumentId id) const { return st_[id].inten; }
  const ToxicityTracker& toxicity(InstrumentId id) const { return st_[id].tox; }
  const MicroPrice& micro(InstrumentId id) const { return st_[id].micro; }

  Quote make_quote(InstrumentId id) const {
    const Instrument& in = inst_[id];
    const State& s = st_[id];
    Quote q;
    const double fv = fair(id);
    if (halted_ || !in.quote || in.proto != Protocol::Clob || !s.top.valid() || !(fv == fv)) return q;
    const double tick = in.tick;
    if (!cfg_.glft) {
      q.half = std::max(0.5 * tick, cfg_.naive_half_frac * 0.5 * s.top.spread());
    } else {
      const double de = in.quote_size * in.mult;
      const Glft g = glft(cfg_.gamma, s.inten.A(), s.inten.k(), de);
      const Risk r = risk_terms(id, g.C);
      q.half = g.base + 0.5 * de * r.self + in.fee +
               (cfg_.adapt_tox ? cfg_.tox_coef * s.tox.adverse() : 0.0);
      q.skew = r.skew;
      q.skew = std::clamp(q.skew, -cfg_.skew_cap * q.half, cfg_.skew_cap * q.half);
    }
    q.bid = floor_to(fv - q.half - q.skew, tick);
    q.ask = ceil_to(fv + q.half - q.skew, tick);
    // Passive only: never lock or cross the market we see.
    if (q.bid >= s.top.ask - 1e-12) q.bid = s.top.ask - tick;
    if (q.ask <= s.top.bid + 1e-12) q.ask = s.top.bid + tick;
    if (q.ask - q.bid < tick - 1e-12) q.ask = q.bid + tick;
    if (in.max_spread > 0 && q.ask - q.bid > in.max_spread + 1e-12) {
      const double mid = 0.5 * (q.ask + q.bid);
      q.bid = floor_to(mid - 0.5 * in.max_spread, tick);
      q.ask = q.bid + floor_to(in.max_spread, tick);
    }
    // Worst case: every cancelled order still in flight fills as well.
    q.bid_sz = floor_to(std::min(in.quote_size, in.max_pos - s.pos - s.closing_bid), in.lot);
    q.ask_sz = floor_to(std::min(in.quote_size, in.max_pos + s.pos - s.closing_ask), in.lot);
    q.bid_on = q.bid_sz >= in.lot && q.bid > 0;
    q.ask_on = q.ask_sz >= in.lot;
    if (risk_ > cfg_.risk_limit) {  // only show the side that reduces portfolio variance
      if (s.m > 0) q.bid_on = false;
      else if (s.m < 0) q.ask_on = false;
    }
    return q;
  }

  void halt() {
    halted_ = true;
    for (size_t i = 0; i < inst_.size(); ++i) {
      cancel_live(static_cast<InstrumentId>(i), st_[i].bid);
      cancel_live(static_cast<InstrumentId>(i), st_[i].ask);
    }
  }

 private:
  struct Live { OrderId id = 0; double px = 0.0, qty = 0.0; TimeNs ts = 0; };
  struct Closing { OrderId id; InstrumentId inst; Side side; double qty; };
  struct State {
    TopOfBook top;
    MicroPrice micro;
    IntensityEstimator inten;
    ToxicityTracker tox;
    WinCurve win;
    Exposures ex;
    double model = kNaN, basis = 0.0, composite = kNaN, fair = kNaN;
    double lead_ema = kNaN, lead_x = kNaN, lead_adj = 0.0;  // lead-lag: average, last value, fair value shift
    TimeNs lead_ts = 0;
    double sigma = 0.0;  // price st.dev per sqrt(s)
    double m = 0.0;      // cov(price, portfolio value) per s
    double pos = 0.0;
    double hedge_pos = 0.0;  // part of pos acquired by the hedger
    double closing_bid = 0.0, closing_ask = 0.0;  // cancelled, not yet confirmed gone
    double w_skew = 0.0;     // sqrt(gamma/2) * b' W E   (portfolio skew, price units)
    double w_self = 0.0;     // sqrt(gamma/2) * b' W b   (own-size risk per effective unit)
    Live bid, ask;
    bool dirty = false;
  };

  void update_lead(InstrumentId id) {
    State& s = st_[id];
    const Instrument& in = inst_[id];
    const double lead = in.lead_inst != kNoInstrument
                            ? (st_[in.lead_inst].top.valid() ? st_[in.lead_inst].top.mid() : kNaN)
                            : fm_.level(in.lead_factor);
    if (!s.top.valid() || !(lead > 0)) return;
    const double m = s.top.mid();
    const double x = in.lead_momentum ? in.lead_beta * std::log(lead)
                                       : std::log(m) - in.lead_beta * std::log(lead);
    if (!(s.lead_ema == s.lead_ema)) { s.lead_ema = s.lead_x = x; s.lead_ts = now_; s.lead_adj = 0.0; return; }
    // the series is piecewise constant: it held its previous value since the last update
    const double dt = std::max(0.0, secs(now_ - s.lead_ts));
    s.lead_ts = now_;
    s.lead_ema += (1.0 - std::exp(-dt / in.lead_tau_s)) * (s.lead_x - s.lead_ema);
    s.lead_x = x;
    // gap: x below its average means the leader has risen and we have not (yet).
    // momentum: x above its average means the leader has just risen.
    const double dev = x - s.lead_ema;
    s.lead_adj = m * (std::exp((in.lead_momentum ? 1.0 : -1.0) * in.lead_gain * dev) - 1.0);
    s.dirty = true;
  }

  void reprice(InstrumentId id) {
    State& s = st_[id];
    const Instrument& in = inst_[id];
    const PriceOut o = pricers_[in.pricer]->price(fm_.levels(), secs(now_ - t0_) / cfg_.secs_per_year);
    s.model = o.px;
    s.ex = o.ex;
    s.sigma = std::sqrt(fm_.variance(o.ex) + in.idio_vol * in.idio_vol);
    double m = in.idio_vol * in.idio_vol * s.pos * in.mult;
    for (uint8_t j = 0; j < o.ex.n; ++j) m += o.ex.e[j].d * SE_[o.ex.e[j].f];
    s.m = m;
  }

  struct Risk { double self, skew; };
  // Inventory terms of the quote, in price units.
  //   self: risk charge per effective unit of trade size (multiplied by size/2)
  //   skew: shift of both quotes away from the side that adds risk
  // Siloed: classic single-asset GLFT on the instrument's own quoting inventory.
  // Portfolio: multi-asset closed form evaluated in factor space, see
  // refresh_liquidity(); idiosyncratic risk is handled per instrument.
  Risk risk_terms(InstrumentId id, double C) const {
    const State& s = st_[id];
    const Instrument& in = inst_[id];
    if (!cfg_.portfolio_skew)
      return Risk{s.sigma * C, s.sigma * C * (s.pos - s.hedge_pos) * in.mult};
    return Risk{s.w_self + in.idio_vol * C, s.w_skew + in.idio_vol * C * s.pos * in.mult};
  }

  // Multi-asset inventory penalty (Bergault, Evangelista, Gueant, Vieira).
  // The value function is approximately -q'Aq with A solving A D A = (gamma/2) Sigma,
  // D = diag(d_i) the liquidity of each instrument (d_i = gamma / (2 C_i^2),
  // i.e. A_i * size_i * k_i up to a constant). With Sigma = B Sigma_F B' the
  // solution collapses to factor space:
  //     A q = sqrt(gamma/2) * B W E,   W = L (L' H L)^(-1/2) L',
  // where E = B'q is the book's factor exposure, L the Cholesky factor of
  // Sigma_F and H = sum_i d_i b_i b_i' measures how much liquidity the
  // universe offers along each factor direction. Cost is O(F^3) for any
  // number of instruments, and a risk that many instruments can shed is
  // (correctly) charged less than one only a single instrument can shed.
  void refresh_liquidity() {
    const size_t nf = fm_.size();
    if (nf == 0) return;
    std::vector<double> H(nf * nf, 0.0), S(nf * nf);
    for (size_t i = 0; i < inst_.size(); ++i) {
      const Instrument& in = inst_[i];
      const State& s = st_[i];
      const bool rfq = in.proto == Protocol::Rfq;
      const Glft g = glft(cfg_.gamma, rfq ? in.A0 : s.inten.A(), rfq ? s.win.b() : s.inten.k(),
                          in.quote_size * in.mult);
      const double d = cfg_.gamma / (2.0 * g.C * g.C);
      for (uint8_t a = 0; a < s.ex.n; ++a)
        for (uint8_t b = 0; b < s.ex.n; ++b)
          H[s.ex.e[a].f * nf + s.ex.e[b].f] += d * s.ex.e[a].d * s.ex.e[b].d;
    }
    for (size_t a = 0; a < nf; ++a)
      for (size_t b = 0; b < nf; ++b) S[a * nf + b] = fm_.cov(a, b);
    const std::vector<double> L = cholesky(S, nf);
    std::vector<double> T(nf * nf, 0.0), G(nf * nf, 0.0);
    for (size_t a = 0; a < nf; ++a)  // T = H L
      for (size_t b = 0; b < nf; ++b)
        for (size_t k = b; k < nf; ++k) T[a * nf + b] += H[a * nf + k] * L[k * nf + b];
    for (size_t a = 0; a < nf; ++a)  // G = L' T
      for (size_t b = 0; b < nf; ++b)
        for (size_t k = a; k < nf; ++k) G[a * nf + b] += L[k * nf + a] * T[k * nf + b];
    const std::vector<double> R = inv_sqrt_sym(G, nf);
    std::fill(T.begin(), T.end(), 0.0);
    for (size_t a = 0; a < nf; ++a)  // T = L R
      for (size_t b = 0; b < nf; ++b)
        for (size_t k = 0; k <= a; ++k) T[a * nf + b] += L[a * nf + k] * R[k * nf + b];
    std::fill(W_.begin(), W_.end(), 0.0);
    for (size_t a = 0; a < nf; ++a)  // W = T L'
      for (size_t b = 0; b < nf; ++b)
        for (size_t k = 0; k <= b; ++k) W_[a * nf + b] += T[a * nf + k] * L[b * nf + k];
  }

  // Factor exposures of the whole book, Sigma * E, per-instrument marginal
  // risk, and portfolio st.dev. O(instruments + factors^2).
  void refresh_risk() {
    std::fill(E_.begin(), E_.end(), 0.0);
    double idio = 0.0;
    for (size_t i = 0; i < inst_.size(); ++i) {
      const double q = st_[i].pos * inst_[i].mult;
      if (q == 0.0) continue;
      for (uint8_t j = 0; j < st_[i].ex.n; ++j) E_[st_[i].ex.e[j].f] += q * st_[i].ex.e[j].d;
      idio += inst_[i].idio_vol * inst_[i].idio_vol * q * q;
    }
    fm_.mul(E_.data(), SE_.data());
    double var = idio;
    for (size_t f = 0; f < E_.size(); ++f) var += E_[f] * SE_[f];
    risk_ = std::sqrt(std::max(0.0, var));
    stats_.max_risk = std::max(stats_.max_risk, risk_);
    const size_t nf = E_.size();
    const double sg2 = std::sqrt(0.5 * cfg_.gamma);
    for (size_t a = 0; a < nf; ++a) {
      double x = 0.0;
      for (size_t b = 0; b < nf; ++b) x += W_[a * nf + b] * E_[b];
      WE_[a] = x;
    }
    for (size_t i = 0; i < inst_.size(); ++i) {
      State& s = st_[i];
      double m = inst_[i].idio_vol * inst_[i].idio_vol * s.pos * inst_[i].mult;
      double ws = 0.0, wb = 0.0;
      for (uint8_t j = 0; j < s.ex.n; ++j) {
        m += s.ex.e[j].d * SE_[s.ex.e[j].f];
        ws += s.ex.e[j].d * WE_[s.ex.e[j].f];
        for (uint8_t k = 0; k < s.ex.n; ++k)
          wb += s.ex.e[j].d * W_[s.ex.e[j].f * nf + s.ex.e[k].f] * s.ex.e[k].d;
      }
      s.m = m;
      s.w_skew = sg2 * ws;
      s.w_self = sg2 * wb;
    }
  }

  // Band hedging in factor space: when the net exposure to a factor carries
  // more risk than the band, trade that factor's hedge instrument back to a
  // fraction of the band. Netting across asset classes happens before this.
  void hedge() {
    if (halted_) return;
    for (size_t f = 0; f < E_.size(); ++f) {
      const InstrumentId h = hedge_inst_[f];
      if (h == kNoInstrument || now_ < hedge_block_[f]) continue;
      const double vol = fm_.vol(static_cast<FactorId>(f));
      if (std::fabs(E_[f]) * vol <= cfg_.hedge_band) continue;
      const State& s = st_[h];
      const Instrument& in = inst_[h];
      const double d = s.ex.to(static_cast<FactorId>(f)) * in.mult;
      if (!s.top.valid() || std::fabs(d) < 1e-12) continue;
      const double target = std::copysign(cfg_.hedge_band * cfg_.hedge_target_frac / vol, E_[f]);
      const double want = -(E_[f] - target) / d;
      const Side side = want > 0 ? Side::Buy : Side::Sell;
      const double room = in.max_pos - sgn(side) * s.pos;  // hedges obey the position limit too
      const double qty = floor_to(std::min(std::fabs(want), room), in.lot);
      if (qty < in.lot) continue;
      const double px = side == Side::Buy ? s.top.ask + cfg_.hedge_slip_ticks * in.tick
                                          : s.top.bid - cfg_.hedge_slip_ticks * in.tick;
      venue_->submit(h, side, px, qty, true, true);
      ++stats_.orders;
      hedge_block_[f] = now_ + ns(cfg_.hedge_cooldown_s);
    }
  }

  void cancel_live(InstrumentId id, Live& l) {
    if (!l.id) return;
    State& s = st_[id];
    const Side side = &l == &s.bid ? Side::Buy : Side::Sell;
    if (l.qty > 0) {
      closing_.push_back(Closing{l.id, id, side, l.qty});
      (side == Side::Buy ? s.closing_bid : s.closing_ask) += l.qty;
    }
    venue_->cancel(id, l.id);
    ++stats_.cancels;
    l = Live{};
  }

  void reconcile(InstrumentId id, Side side, bool on, double px, double qty) {
    State& s = st_[id];
    const Instrument& in = inst_[id];
    Live& l = side == Side::Buy ? s.bid : s.ask;
    if (l.id) {
      const bool moved = std::fabs(l.px - px) > (cfg_.requote_ticks - 0.5) * in.tick;
      const bool thin = l.qty < 0.5 * qty;  // keep queue position unless badly depleted
      const bool too_big = l.qty > qty + 0.5 * in.lot;  // position room shrank
      // Resting more aggressively than we now want: never wait to fix that.
      const bool aggressive = side == Side::Buy ? l.px > px + 0.5 * in.tick : l.px < px - 0.5 * in.tick;
      const bool young = now_ - l.ts < ns(cfg_.min_quote_life_s);
      if (!on || too_big || aggressive || (!young && (moved || thin))) cancel_live(id, l);
    }
    // Room left once every order still able to fill (including one cancelled
    // just above) is counted as filled.
    const double room = side == Side::Buy ? in.max_pos - s.pos - s.closing_bid : in.max_pos + s.pos - s.closing_ask;
    qty = floor_to(std::min(qty, room), in.lot);
    if (cfg_.wait_cancel_ack && on && !l.id)  // one order in flight per side: replace once the cancel lands
      for (const Closing& c : closing_)
        if (c.inst == id && c.side == side) return;
    if (on && !l.id && qty >= in.lot) {
      const OrderId oid = venue_->submit(id, side, px, qty, false, false);
      ++stats_.orders;
      if (oid) { l.id = oid; l.px = px; l.qty = qty; l.ts = now_; }  // 0: refused before sending
      else ++stats_.refused;
    }
  }

  void flush() {
    for (size_t i = 0; i < inst_.size(); ++i) {
      State& s = st_[i];
      if (!s.dirty) continue;
      s.dirty = false;
      if (inst_[i].proto != Protocol::Clob) continue;
      const Quote q = make_quote(static_cast<InstrumentId>(i));
      reconcile(static_cast<InstrumentId>(i), Side::Buy, q.bid_on, q.bid, q.bid_sz);
      reconcile(static_cast<InstrumentId>(i), Side::Sell, q.ask_on, q.ask, q.ask_sz);
    }
  }

  EngineConfig cfg_;
  Venue* venue_;
  FactorModel fm_;
  std::vector<std::unique_ptr<Pricer>> pricers_;
  std::vector<Instrument> inst_;
  std::vector<State> st_;
  std::vector<std::vector<InstrumentId>> deps_;
  std::vector<std::vector<InstrumentId>> lead_deps_;  // per factor: instruments that follow it
  std::vector<std::vector<InstrumentId>> lead_inst_deps_;  // per instrument: instruments that follow it
  std::vector<InstrumentId> hedge_inst_;
  std::vector<TimeNs> hedge_block_;
  std::vector<double> E_, SE_, WE_, W_;
  std::vector<Closing> closing_;
  double cash_ = 0.0, risk_ = 0.0;
  TimeNs now_ = 0, t0_ = 0, last_timer_ = 0;
  bool halted_ = false;
  EngineStats stats_;
};

}  // namespace amm
