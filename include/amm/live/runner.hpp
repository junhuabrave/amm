// Drives the engine from a stream of normalised market-data events with a
// paper venue attached. The same code path serves live use (events piped in
// from a venue adapter) and replay (events read from a recorded file), so a
// session can be recorded once and re-run exactly.
#pragma once
#include <iomanip>
#include <istream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "amm/live/config.hpp"
#include "amm/live/events.hpp"
#include "amm/live/paper.hpp"

namespace amm::live {

struct RunnerOptions {
  double status_every_s = 0.0;     // 0 = no periodic status
  double timer_s = 0.1;
  std::ostream* record = nullptr;  // journal of every event received
  std::ostream* log = nullptr;     // status lines and fills
  bool log_fills = false;
};

class Runner {
 public:
  Runner(const LiveConfig& cfg, RunnerOptions opt)
      : opt_(opt), paper_(cfg.paper_latency_s), eng_(cfg.engine, &paper_) {
    build_engine(cfg, eng_);
    const size_t n = eng_.instruments();
    paper_.resize(n);
    tops_.assign(n, TopOfBook{});
    acct_.assign(n, Account{});
    for (size_t i = 0; i < n; ++i) {
      if (by_feed_.count(cfg.instruments[i].feed))
        throw std::runtime_error("feed symbol used twice: " + cfg.instruments[i].feed);
      by_feed_[cfg.instruments[i].feed] = static_cast<InstrumentId>(i);
    }
    paper_.set_fill_handler([this](const Fill& f) { on_fill(f); });
    paper_.set_cancel_handler([this](InstrumentId i, OrderId o) { if (warm_) eng_.on_cancel_done(i, o); });
  }

  void run(std::istream& in) {
    std::string line;
    MdEvent ev;
    while (std::getline(in, line)) {
      if (parse_event(line, ev)) on_event(ev);
      else if (!line.empty() && line[0] != '#') ++bad_lines_;
    }
  }

  void on_event(const MdEvent& ev) {
    if (opt_.record) *opt_.record << format_event(ev) << '\n';
    const auto it = by_feed_.find(ev.sym);
    if (it == by_feed_.end()) { ++unknown_; return; }
    const InstrumentId id = it->second;
    if (first_ts_ < 0) first_ts_ = ev.ts;
    now_ = std::max(now_, ev.ts);
    ++events_;
    if (warm_) paper_.on_time(now_);
    switch (ev.type) {
      case 'B': {
        TopOfBook t{ev.a, ev.c, ev.b, ev.d, now_};
        tops_[id] = t;
        if (!warm_) { try_start(); break; }
        paper_.on_book(id, t);  // the market may run over our resting quotes first
        eng_.on_book(id, t);
        break;
      }
      case 'T':
        if (warm_) paper_.on_trade(id, ev.a, ev.b, ev.c);
        break;
      case 'C':
        if (warm_) eng_.on_composite(id, ev.a, now_);
        else pending_composite_[id] = ev.a;
        break;
      default: break;
    }
    if (!warm_) return;
    if (now_ >= next_timer_) {
      eng_.on_timer(now_);
      next_timer_ = now_ + ns(opt_.timer_s);
      const double v = pnl();  // drawdown sampled on the timer clock
      peak_pnl_ = std::max(peak_pnl_, v);
      max_dd_ = std::max(max_dd_, peak_pnl_ - v);
    }
    if (opt_.status_every_s > 0 && opt_.log && now_ >= next_status_) {
      status(*opt_.log);
      next_status_ = now_ + ns(opt_.status_every_s);
    }
  }

  // Mark-to-market P&L from the runner's own account of the fills, valued at
  // the market mid (engine fair value for instruments without a book).
  double pnl() const {
    double v = 0.0;
    for (size_t i = 0; i < acct_.size(); ++i) v += inst_pnl(static_cast<InstrumentId>(i));
    return v;
  }
  double inst_pnl(InstrumentId i) const {
    const double mark = tops_[i].valid() ? tops_[i].mid() : (warm_ ? eng_.fair(i) : 0.0);
    return acct_[i].cash + (acct_[i].pos != 0.0 ? acct_[i].pos * eng_.instrument(i).mult * mark : 0.0);
  }
  double position(InstrumentId i) const { return acct_[i].pos; }
  double peak_position(InstrumentId i) const { return acct_[i].peak; }  // largest |position| reached
  uint64_t fills() const { return fills_; }
  double volume() const { return volume_; }
  double max_drawdown() const { return max_dd_; }  // of P&L sampled every timer_s
  uint64_t events() const { return events_; }
  bool warm() const { return warm_; }
  double elapsed_s() const { return first_ts_ < 0 ? 0.0 : secs(now_ - first_ts_); }
  Engine& engine() { return eng_; }
  const PaperVenue& venue() const { return paper_; }

  void status(std::ostream& os) const {
    os << std::fixed << std::setprecision(1) << "[t+" << elapsed_s() << "s] pnl " << std::setprecision(2) << pnl()
       << "  risk " << eng_.risk() << "  fills " << fills_ << (eng_.halted() ? "  HALTED" : "") << '\n';
  }

  void summary(std::ostream& os) const {
    os << "\nsession: " << std::fixed << std::setprecision(1) << elapsed_s() << " s of market data, " << events_
       << " events (" << unknown_ << " for unknown symbols, " << bad_lines_ << " unparsable lines)\n";
    if (!warm_) {
      os << "engine never started: no book received yet for";
      for (size_t i = 0; i < tops_.size(); ++i)
        if (eng_.instrument(i).proto == Protocol::Clob && !tops_[i].valid()) os << " [" << eng_.instrument(i).symbol << "]";
      os << '\n';
      return;
    }
    const EngineStats& st = eng_.stats();
    os << "paper P&L " << std::setprecision(2) << pnl() << "   fills " << fills_ << " (hedges " << st.hedge_fills
       << ")   volume " << std::setprecision(0) << volume_ << "   orders " << paper_.orders_sent() << "   cancels "
       << paper_.cancels_sent() << std::setprecision(1) << " (" << paper_.orders_sent() / std::max(1e-9, elapsed_s())
       << " orders/s)" << (eng_.halted() ? "   KILL SWITCH FIRED" : "") << "\n\n";
    os << std::left << std::setw(22) << "instrument" << std::right << std::setw(12) << "position" << std::setw(12) << "peak |pos|" << std::setw(8)
       << "fills" << std::setw(14) << "P&L" << std::setw(14) << "mkt bid" << std::setw(14) << "mkt ask"
       << std::setw(14) << "our bid" << std::setw(14) << "our ask" << '\n';
    for (size_t i = 0; i < acct_.size(); ++i) {
      const InstrumentId id = static_cast<InstrumentId>(i);
      const Quote q = eng_.make_quote(id);
      os << std::left << std::setw(22) << eng_.instrument(id).symbol << std::right << std::setprecision(4)
         << std::setw(12) << acct_[i].pos << std::setw(12) << acct_[i].peak << std::setw(8) << acct_[i].fills << std::setprecision(2) << std::setw(14)
         << inst_pnl(id) << std::setprecision(6) << std::defaultfloat;
      auto px = [&](bool on, double v) { os << std::setw(14); if (on) os << v; else os << "-"; };
      px(tops_[i].valid(), tops_[i].bid); px(tops_[i].valid(), tops_[i].ask);
      px(q.bid_on, q.bid); px(q.ask_on, q.ask);
      os << std::fixed << '\n';
    }
  }

 private:
  struct Account { double pos = 0.0, cash = 0.0, peak = 0.0; uint64_t fills = 0; };

  // The engine starts once every order-book instrument has a market, so no
  // instrument is ever priced off a configured (stale) factor level.
  void try_start() {
    for (size_t i = 0; i < tops_.size(); ++i)
      if (eng_.instrument(i).proto == Protocol::Clob && !tops_[i].valid()) return;
    warm_ = true;
    eng_.start(now_);
    next_timer_ = now_ + ns(opt_.timer_s);
    next_status_ = now_ + ns(opt_.status_every_s);
    paper_.on_time(now_);
    for (int pass = 0; pass < 2; ++pass)  // factor reference markets first
      for (size_t i = 0; i < tops_.size(); ++i) {
        const Instrument& in = eng_.instrument(i);
        if (in.proto != Protocol::Clob || (in.ref_for_factor != kNoFactor) != (pass == 0)) continue;
        TopOfBook t = tops_[i];
        t.ts = now_;
        paper_.on_book(static_cast<InstrumentId>(i), t);
        eng_.on_book(static_cast<InstrumentId>(i), t);
      }
    for (const auto& [id, px] : pending_composite_) eng_.on_composite(id, px, now_);
    pending_composite_.clear();
  }

  void on_fill(const Fill& f) {
    Account& a = acct_[f.inst];
    const double mult = eng_.instrument(f.inst).mult;
    a.pos += sgn(f.side) * f.qty;
    a.peak = std::max(a.peak, std::fabs(a.pos));
    a.cash -= sgn(f.side) * f.qty * f.px * mult;
    ++a.fills;
    ++fills_;
    volume_ += f.qty * f.px * mult;
    if (opt_.log && opt_.log_fills)
      *opt_.log << "fill " << eng_.instrument(f.inst).symbol << (f.side == Side::Buy ? " BUY " : " SELL ") << f.qty
                << " @ " << f.px << (f.hedge ? " (hedge)" : "") << '\n';
    eng_.on_fill(f);
  }

  RunnerOptions opt_;
  PaperVenue paper_;
  Engine eng_;
  std::map<std::string, InstrumentId> by_feed_;
  std::map<InstrumentId, double> pending_composite_;
  std::vector<TopOfBook> tops_;
  std::vector<Account> acct_;
  TimeNs now_ = 0, first_ts_ = -1, next_timer_ = 0, next_status_ = 0;
  bool warm_ = false;
  uint64_t events_ = 0, unknown_ = 0, bad_lines_ = 0, fills_ = 0;
  double volume_ = 0.0, peak_pnl_ = 0.0, max_dd_ = 0.0;
};

}  // namespace amm::live
