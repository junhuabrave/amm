// Price-time priority limit order book used by the simulator.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "amm/core.hpp"

namespace amm::sim {

struct Exec {
  OrderId maker_id;
  int maker_owner;
  int64_t px_ticks;
  double qty;
};

class MatchBook {
 public:
  explicit MatchBook(double tick = 0.01) : tick_(tick) {}
  double tick() const { return tick_; }
  int64_t to_ticks(double px) const { return std::llround(px / tick_); }
  double to_px(int64_t t) const { return static_cast<double>(t) * tick_; }

  // Match against the opposite side up to the limit, then rest the remainder
  // unless IOC. Orders never trade against the same owner (they are skipped).
  void add(OrderId id, int owner, Side side, int64_t px, double qty, bool ioc,
           std::vector<Exec>& out) {
    auto match = [&](auto& levels, auto crosses) {
      for (auto it = levels.begin(); it != levels.end() && qty > 1e-12 && crosses(it->first);) {
        auto& dq = it->second;
        for (auto o = dq.begin(); o != dq.end() && qty > 1e-12;) {
          if (o->owner == owner) { ++o; continue; }
          const double q = std::min(qty, o->qty);
          out.push_back(Exec{o->id, o->owner, it->first, q});
          qty -= q;
          o->qty -= q;
          if (o->qty <= 1e-12) { idx_.erase(o->id); o = dq.erase(o); }
          else ++o;
        }
        if (dq.empty()) it = levels.erase(it);
        else ++it;
      }
    };
    if (side == Side::Buy) match(asks_, [&](int64_t p) { return p <= px; });
    else match(bids_, [&](int64_t p) { return p >= px; });
    if (!ioc && qty > 1e-12) {
      if (side == Side::Buy) bids_[px].push_back(Resting{id, owner, qty});
      else asks_[px].push_back(Resting{id, owner, qty});
      idx_[id] = Loc{side, px};
    }
  }

  bool cancel(OrderId id) {
    auto it = idx_.find(id);
    if (it == idx_.end()) return false;
    const Loc loc = it->second;
    idx_.erase(it);
    auto drop = [&](auto& levels) {
      auto lv = levels.find(loc.px);
      if (lv == levels.end()) return;
      auto& dq = lv->second;
      for (auto o = dq.begin(); o != dq.end(); ++o)
        if (o->id == id) { dq.erase(o); break; }
      if (dq.empty()) levels.erase(lv);
    };
    if (loc.side == Side::Buy) drop(bids_); else drop(asks_);
    return true;
  }

  // Best bid/ask ignoring one owner's orders (what that owner sees as "the market").
  TopOfBook top_excluding(int owner) const {
    TopOfBook t;
    auto scan = [&](const auto& levels, double& px, double& sz) {
      for (const auto& [p, dq] : levels) {
        double s = 0.0;
        for (const auto& o : dq) if (o.owner != owner) s += o.qty;
        if (s > 0) { px = to_px(p); sz = s; return; }
      }
    };
    scan(bids_, t.bid, t.bid_sz);
    scan(asks_, t.ask, t.ask_sz);
    return t;
  }
  size_t resting() const { return idx_.size(); }

 private:
  struct Resting { OrderId id; int owner; double qty; };
  struct Loc { Side side; int64_t px; };
  std::map<int64_t, std::deque<Resting>, std::greater<int64_t>> bids_;
  std::map<int64_t, std::deque<Resting>> asks_;
  std::unordered_map<OrderId, Loc> idx_;
  double tick_;
};

}  // namespace amm::sim
