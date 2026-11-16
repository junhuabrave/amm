// Engine side of a real order gateway.
//
// Orders leave the engine as text lines to a separate gateway process (one
// per venue, like the market-data adapters), which signs and sends them and
// writes execution reports back into the runner's event stream (events.hpp):
//
//   engine -> gateway
//     N,<id>,<symbol>,<B|S>,<price>,<qty>,<POST|IOC>     new order
//     X,<id>,<symbol>                                    cancel
//     K,<reason>                                         kill: cancel all, accept nothing more
//   gateway -> engine
//     A / F / D / P / G event lines (see events.hpp)
//
// <id> is the engine's numeric order id; the gateway maps it to the venue's
// client order id. Quotes go as POST (post-only: rejected rather than taking
// liquidity), hedges as IOC. A quote is resting only once accepted ('A'), and
// it can fill until the venue says it is done ('D'), whatever cancels were
// sent; the runner passes both to the engine and the risk gate.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "amm/core.hpp"

namespace amm::live {

class GatewayVenue final : public Venue {
 public:
  struct Order {
    InstrumentId inst; Side side; double px, qty, left; bool ioc, hedge, acked = false, cancel_sent = false;
  };

  void attach(std::ostream* out, std::vector<std::string> symbols) { out_ = out; sym_ = std::move(symbols); }

  OrderId submit(InstrumentId inst, Side side, double px, double qty, bool ioc, bool hedge) override {
    const OrderId id = (++next_) | (hedge ? kHedgeTag : 0);
    orders_[id] = Order{inst, side, px, qty, qty, ioc, hedge};
    char buf[160];
    std::snprintf(buf, sizeof buf, "N,%llu,%s,%c,%.12g,%.12g,%s\n", static_cast<unsigned long long>(id),
                  sym_[inst].c_str(), side == Side::Buy ? 'B' : 'S', px, qty, ioc ? "IOC" : "POST");
    write(buf);
    ++sent_;
    return id;
  }
  void cancel(InstrumentId inst, OrderId id) override {
    auto it = orders_.find(id);
    if (it == orders_.end() || it->second.cancel_sent) return;
    it->second.cancel_sent = true;
    write("X," + std::to_string(id) + "," + sym_[inst] + "\n");
    ++cancels_;
  }
  void kill(const std::string& reason) {
    if (killed_) return;
    killed_ = true;
    std::string r = reason;
    for (char& c : r) if (c == ',' || c == '\n') c = ' ';
    write("K," + r + "\n");
  }

  // Execution reports. Each returns false for an id this session never sent.
  bool on_ack(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) { ++unknown_; return false; }
    it->second.acked = true;
    return true;
  }
  // Fills larger than what is left are capped (and counted): the venue's
  // report wins for position keeping only via the reconciler, not here.
  bool on_fill(OrderId id, double px, double qty, TimeNs ts, Fill& f) {
    auto it = orders_.find(id);
    if (it == orders_.end()) { ++unknown_; return false; }
    Order& o = it->second;
    if (qty > o.left + 1e-12) ++overfills_;
    o.left -= qty;
    f = Fill{o.inst, o.side, px, qty, ts, id, o.hedge};
    return true;
  }
  bool on_done(OrderId id, InstrumentId& inst) {
    auto it = orders_.find(id);
    if (it == orders_.end()) { ++unknown_; return false; }
    inst = it->second.inst;
    orders_.erase(it);
    ++done_;
    return true;
  }

  // Our accepted, resting size at exactly this price (for own-order filtering).
  double own_at(InstrumentId inst, Side side, double px) const {
    double q = 0.0;
    for (const auto& [id, o] : orders_)
      if (o.inst == inst && o.side == side && o.acked && !o.ioc && std::fabs(o.px - px) <= 1e-9 * std::max(1.0, px))
        q += std::max(0.0, o.left);
    return q;
  }

  const std::unordered_map<OrderId, Order>& orders() const { return orders_; }
  uint64_t orders_sent() const { return sent_; }
  uint64_t cancels_sent() const { return cancels_; }
  uint64_t unknown_reports() const { return unknown_; }
  uint64_t overfills() const { return overfills_; }
  bool killed() const { return killed_; }

 private:
  void write(const std::string& s) {
    if (!out_) return;
    *out_ << s;
    out_->flush();  // a pipe: every order goes out now, not when a buffer fills
  }

  std::ostream* out_ = nullptr;
  std::vector<std::string> sym_;
  std::unordered_map<OrderId, Order> orders_;
  OrderId next_ = 0;
  uint64_t sent_ = 0, cancels_ = 0, unknown_ = 0, overfills_ = 0, done_ = 0;
  bool killed_ = false;
};

}  // namespace amm::live
