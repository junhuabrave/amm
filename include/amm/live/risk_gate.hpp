// Pre-trade controls between the engine and any venue (paper or real).
//
// Every order the engine sends passes through here first. An order that fails
// a check is not sent and `submit` returns 0, which the engine treats as "no
// order". Cancels are never blocked: they only reduce risk. Checks:
//
//   * kill switch: once tripped (by the engine's loss limit, a reconciliation
//     mismatch or an operator), nothing new goes out until reset.
//   * price collar: a limit price more than `collar_bps` away from the last
//     market mid is refused (fat-finger and stale-reference protection).
//   * order size: no single order above the instrument's max_order_qty or
//     `max_order_notional`.
//   * message rate: a token bucket of `max_orders_per_s` with `burst`
//     capacity, set inside the venue's own limit so the venue never has to
//     throttle us (Binance USD-M: 300 orders per 10 s per account).
//   * open orders: at most `max_open` resting orders per instrument.
//   * self-trade: a passive order that would cross one of our own resting
//     orders is refused. IOC hedges are let through and counted; the gateway
//     asks the venue to cancel our resting side instead of matching us with
//     ourselves (Binance selfTradePreventionMode=EXPIRE_MAKER).
//
// The gate learns what is resting from submits, fills and done notices, so
// the runner must forward both to it (see runner.hpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "amm/core.hpp"

namespace amm::live {

struct RiskLimits {
  double collar_bps = 100.0;        // 0 = off
  double max_order_notional = 0.0;  // currency; 0 = off
  double max_orders_per_s = 25.0;   // sustained submit rate, all instruments
  double burst = 50.0;              // token bucket capacity
  int max_open = 20;                // resting orders per instrument, cancels in flight included
};

enum class Reject : int { None = 0, Killed, Collar, Size, Rate, Open, SelfTrade, NoReference, Count };
inline const char* reject_name(Reject r) {
  static const char* n[] = {"none", "killed", "price collar", "size", "message rate", "open orders",
                            "self-trade", "no reference price"};
  return n[static_cast<int>(r)];
}

class RiskGate final : public Venue {
 public:
  RiskGate(Venue* inner, RiskLimits lim = {}) : inner_(inner), lim_(lim), tokens_(lim.burst) {}

  // max_order_qty per instrument (0 = no extra cap beyond max_order_notional)
  void resize(size_t n, std::vector<double> max_order_qty = {}, std::vector<double> mult = {}) {
    ref_.assign(n, kNaN);
    max_qty_ = max_order_qty; max_qty_.resize(n, 0.0);
    mult_ = mult; mult_.resize(n, 1.0);
    open_.assign(n, 0);
  }
  void set_time(TimeNs now) {
    if (last_ > 0 && now > last_) tokens_ = std::min(lim_.burst, tokens_ + secs(now - last_) * lim_.max_orders_per_s);
    last_ = std::max(last_, now);
  }
  void set_reference(InstrumentId i, double mid) { ref_[i] = mid; }

  void kill(const std::string& why) { if (!killed_) { killed_ = true; kill_reason_ = why; } }
  void reset_kill() { killed_ = false; kill_reason_.clear(); }
  bool killed() const { return killed_; }
  const std::string& kill_reason() const { return kill_reason_; }

  OrderId submit(InstrumentId inst, Side side, double px, double qty, bool ioc, bool hedge) override {
    const Reject r = check(inst, side, px, qty, ioc);
    if (r != Reject::None) { ++rejects_[static_cast<int>(r)]; return 0; }
    if (ioc) for (const auto& [id, o] : live_) if (o.inst == inst && o.side != side && crosses(side, px, o.px)) { ++ioc_self_cross_; break; }
    tokens_ -= 1.0;
    const OrderId id = inner_->submit(inst, side, px, qty, ioc, hedge);
    if (id && !ioc) { live_[id] = Open{inst, side, px, qty}; ++open_[inst]; }
    ++passed_;
    return id;
  }
  void cancel(InstrumentId inst, OrderId id) override { inner_->cancel(inst, id); }

  // The venue reports a fill or that an order can no longer fill.
  void on_fill(OrderId id, double qty) {
    auto it = live_.find(id);
    if (it == live_.end()) return;
    it->second.qty -= qty;
    if (it->second.qty <= 1e-12) erase(it);
  }
  void on_done(OrderId id) {
    auto it = live_.find(id);
    if (it != live_.end()) erase(it);
  }

  // Refused submit attempts; the engine retries on its next update, so one
  // quote held back for a while counts many times.
  uint64_t rejected(Reject r) const { return rejects_[static_cast<int>(r)]; }
  uint64_t rejected_total() const { uint64_t s = 0; for (auto x : rejects_) s += x; return s; }
  uint64_t passed() const { return passed_; }
  uint64_t ioc_self_cross() const { return ioc_self_cross_; }
  size_t open(InstrumentId i) const { return open_[i]; }

 private:
  struct Open { InstrumentId inst; Side side; double px, qty; };

  static bool crosses(Side side, double px, double their_px) {
    return side == Side::Buy ? px >= their_px - 1e-12 : px <= their_px + 1e-12;
  }
  void erase(std::unordered_map<OrderId, Open>::iterator it) {
    --open_[it->second.inst];
    live_.erase(it);
  }

  Reject check(InstrumentId inst, Side side, double px, double qty, bool ioc) const {
    if (killed_) return Reject::Killed;
    const double ref = ref_[inst];
    if (lim_.collar_bps > 0) {
      if (!(ref == ref) || ref <= 0) return Reject::NoReference;
      if (std::fabs(px - ref) > lim_.collar_bps * 1e-4 * ref) return Reject::Collar;
    }
    if (max_qty_[inst] > 0 && qty > max_qty_[inst] * (1 + 1e-9)) return Reject::Size;
    if (lim_.max_order_notional > 0 && qty * px * mult_[inst] > lim_.max_order_notional) return Reject::Size;
    if (tokens_ < 1.0) return Reject::Rate;
    if (!ioc) {
      if (static_cast<int>(open_[inst]) >= lim_.max_open) return Reject::Open;
      for (const auto& [id, o] : live_)
        if (o.inst == inst && o.side != side && crosses(side, px, o.px)) return Reject::SelfTrade;
    }
    return Reject::None;
  }

  Venue* inner_;
  RiskLimits lim_;
  double tokens_;
  TimeNs last_ = 0;
  std::vector<double> ref_, max_qty_, mult_;
  std::vector<size_t> open_;
  std::unordered_map<OrderId, Open> live_;
  bool killed_ = false;
  std::string kill_reason_;
  uint64_t rejects_[static_cast<int>(Reject::Count)] = {};
  uint64_t passed_ = 0, ioc_self_cross_ = 0;
};

}  // namespace amm::live
