// Paper-trading venue: our orders are matched locally against the real
// market's top of book and trade prints. No order ever leaves the process.
//
// Fill rules (deliberately conservative):
//   * An order becomes active `latency` after it is sent; cancels take the
//     same time. Until then it cannot fill, and a cancel cannot save it.
//   * A trade that prints THROUGH our price fills us (we had price priority).
//   * A trade AT our price fills us only after the size that was displayed
//     ahead of us when we joined has traded (queue position). Joining inside
//     the spread means nobody is ahead.
//   * If the market quote crosses our resting price, we fill once against
//     the displayed size at that crossing.
//   * IOC orders take the displayed size at the opposite touch and no more.
//     A resting order that is marketable on arrival is rejected when
//     post-only is on (the default, as the gateway sends quotes: Binance
//     GTX), and otherwise takes the displayed size like an IOC.
//   * An order and its cancel sent at the same moment arrive in that order:
//     a cancelled order that is marketable on arrival still fills, as it
//     would on a real venue. Each processed cancel is confirmed back (the
//     cancel handler), so the engine knows when the order can no longer fill.
// What it cannot know: flow that would have traded with our better price but
// never printed because nobody was quoting there. Paper results therefore
// understate fills inside the spread.
#pragma once
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>

#include "amm/core.hpp"

namespace amm::live {

class PaperVenue final : public Venue {
 public:
  explicit PaperVenue(double latency_s = 0.005) : latency_(ns(latency_s)) {}
  void resize(size_t instruments) { books_.assign(instruments, Book{}); }
  void set_fill_handler(std::function<void(const Fill&)> h) { on_fill_ = std::move(h); }
  void set_cancel_handler(std::function<void(InstrumentId, OrderId)> h) { on_cancel_ = std::move(h); }
  void set_post_only(bool on) { post_only_ = on; }
  uint64_t post_only_rejects() const { return post_only_rejects_; }
  uint64_t orders_sent() const { return sent_; }
  uint64_t cancels_sent() const { return cancels_; }
  size_t resting(InstrumentId i) const { return books_[i].orders.size(); }

  OrderId submit(InstrumentId inst, Side side, double px, double qty, bool ioc, bool hedge) override {
    const OrderId id = (++next_id_) | (hedge ? kHedgeTag : 0);
    pending_.push_back(Action{now_ + latency_, false, Order{id, inst, side, px, qty, ioc, hedge, 0.0, kNaN}});
    ++sent_;
    return id;
  }
  void cancel(InstrumentId inst, OrderId id) override {
    Action a{now_ + latency_, true, Order{}};
    a.o.id = id; a.o.inst = inst;
    pending_.push_back(a);
    ++cancels_;
  }

  // Advance time: orders and cancels whose latency has elapsed take effect.
  void on_time(TimeNs now) {
    now_ = std::max(now_, now);
    while (!pending_.empty() && pending_.front().at <= now_) {
      const Action a = pending_.front();
      pending_.pop_front();
      if (a.is_cancel) {
        auto& v = books_[a.o.inst].orders;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Order& o) { return o.id == a.o.id; }), v.end());
        if (on_cancel_) on_cancel_(a.o.inst, a.o.id);  // may re-enter submit/cancel (they only queue)
      } else {
        activate(a.o);
      }
    }
  }

  void on_book(InstrumentId inst, const TopOfBook& top) {
    Book& b = books_[inst];
    b.top = top;
    if (!top.valid()) return;
    for (size_t i = 0; i < b.orders.size();) {
      Order& o = b.orders[i];
      const bool buy = o.side == Side::Buy;
      const double opp_px = buy ? top.ask : top.bid, opp_sz = buy ? top.ask_sz : top.bid_sz;
      const bool crossed = buy ? opp_px <= o.px + eps(o.px) : opp_px >= o.px - eps(o.px);
      if (crossed && !(o.cross_px == opp_px)) {
        o.cross_px = opp_px;  // fill once per crossing quote
        if (fill(b, i, std::min(o.qty, opp_sz), o.px)) continue;
      } else if (!crossed) {
        o.cross_px = kNaN;
      }
      ++i;
    }
  }

  // aggressor: +1 buyer lifted the offer, -1 seller hit the bid.
  void on_trade(InstrumentId inst, double px, double qty, double aggressor) {
    Book& b = books_[inst];
    const Side resting_side = aggressor > 0 ? Side::Sell : Side::Buy;
    double left = qty;
    for (size_t i = 0; i < b.orders.size() && left > 0;) {
      Order& o = b.orders[i];
      if (o.side != resting_side) { ++i; continue; }
      const double e = eps(o.px);
      const bool through = resting_side == Side::Buy ? px < o.px - e : px > o.px + e;
      const bool at = std::fabs(px - o.px) <= e;
      if (!through && !at) { ++i; continue; }
      if (at) {
        const double ahead = std::min(o.queue_ahead, left);
        o.queue_ahead -= ahead;
        left -= ahead;
        if (left <= 0) break;
      }
      const double q = std::min(o.qty, left);
      left -= q;
      if (fill(b, i, q, o.px)) continue;
      ++i;
    }
  }

 private:
  struct Order {
    OrderId id = 0; InstrumentId inst = 0; Side side = Side::Buy;
    double px = 0, qty = 0; bool ioc = false, hedge = false;
    double queue_ahead = 0; double cross_px = kNaN;
  };
  struct Action { TimeNs at; bool is_cancel; Order o; };
  struct Book { TopOfBook top; std::vector<Order> orders; };
  static double eps(double px) { return 1e-9 * std::max(1.0, std::fabs(px)); }

  void emit(const Order& o, double qty, double px, bool taker = false) {
    if (on_fill_ && qty > 0) on_fill_(Fill{o.inst, o.side, px, qty, now_, o.id, o.hedge, taker});
  }
  // Fill part of resting order i; returns true if the order was removed.
  bool fill(Book& b, size_t i, double qty, double px) {
    if (qty <= 0) return false;
    const Order snapshot = b.orders[i];
    b.orders[i].qty -= qty;
    const bool done = b.orders[i].qty <= 1e-12;
    if (done) b.orders.erase(b.orders.begin() + static_cast<long>(i));
    emit(snapshot, qty, px);  // may re-enter submit/cancel (they only queue)
    return done;
  }
  void activate(Order o) {
    Book& b = books_[o.inst];
    const TopOfBook& t = b.top;
    if (!t.valid()) { if (!o.ioc) b.orders.push_back(o); return; }
    const bool buy = o.side == Side::Buy;
    const double opp_px = buy ? t.ask : t.bid, opp_sz = buy ? t.ask_sz : t.bid_sz;
    const bool marketable = buy ? o.px >= opp_px - eps(o.px) : o.px <= opp_px + eps(o.px);
    if (marketable && !o.ioc && post_only_) {  // would take liquidity: the venue rejects it
      ++post_only_rejects_;
      if (on_cancel_) on_cancel_(o.inst, o.id);  // done: can no longer fill
      return;
    }
    if (marketable) {
      const double q = std::min(o.qty, opp_sz);
      o.qty -= q;
      emit(o, q, opp_px, true);  // taker: pays the market's price and the taker fee
      o.cross_px = opp_px;
    }
    if (o.ioc || o.qty <= 1e-12) return;
    const double same_px = buy ? t.bid : t.ask, same_sz = buy ? t.bid_sz : t.ask_sz;
    const bool improves = buy ? o.px > same_px + eps(o.px) : o.px < same_px - eps(o.px);
    o.queue_ahead = improves ? 0.0 : same_sz;  // join at or behind the touch: wait your turn
    b.orders.push_back(o);
  }

  TimeNs latency_, now_ = 0;
  OrderId next_id_ = 0;
  uint64_t sent_ = 0, cancels_ = 0, post_only_rejects_ = 0;
  bool post_only_ = true;
  std::deque<Action> pending_;
  std::vector<Book> books_;
  std::function<void(const Fill&)> on_fill_;
  std::function<void(InstrumentId, OrderId)> on_cancel_;
};

}  // namespace amm::live
