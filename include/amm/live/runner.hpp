// Drives the engine from a stream of normalised events, with orders going to
// either venue:
//
//   paper    (default) a PaperVenue matches them in-process against the market
//            data. Live use pipes events in from a venue adapter; replay reads
//            a recorded file, and re-runs give identical results.
//   gateway  (RunnerOptions::orders set) order lines go to a gateway process
//            that sends them to the venue and writes execution reports back
//            into the event stream (gateway.hpp).
//
// Every order passes the pre-trade RiskGate first, in both modes. In gateway
// mode the runner also: waits for the gateway's "ready" before quoting; takes
// over positions the venue reports at startup; trips the kill switch on a
// position mismatch with the venue, a gateway going down, the engine's loss
// limit or a kill file appearing; filters our own resting orders out of the
// displayed book; and writes a state snapshot for the reconciler.
#pragma once
#include <filesystem>
#include <iomanip>
#include <istream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "amm/live/config.hpp"
#include "amm/live/events.hpp"
#include "amm/live/gateway.hpp"
#include "amm/live/paper.hpp"
#include "amm/live/risk_gate.hpp"

namespace amm::live {

struct RunnerOptions {
  double status_every_s = 0.0;     // 0 = no periodic status
  double timer_s = 0.1;
  std::ostream* record = nullptr;  // journal of every event received
  std::ostream* log = nullptr;     // status lines and fills
  bool log_fills = false;
  // gateway mode
  std::ostream* orders = nullptr;  // order lines to the gateway; null = paper
  std::string kill_file;           // halt as soon as this file exists
  std::ostream* state = nullptr;   // state snapshots (JSON lines) for the reconciler
  double state_every_s = 1.0;
  double position_tolerance = 1e-9;  // venue vs own position, units
};

class Runner {
 public:
  Runner(const LiveConfig& cfg, RunnerOptions opt)
      : opt_(opt), paper_(cfg.paper_latency_s),
        gate_(opt.orders ? static_cast<Venue*>(&gw_) : static_cast<Venue*>(&paper_), cfg.risk),
        eng_(cfg.engine, &gate_) {
    build_engine(cfg, eng_);
    const size_t n = eng_.instruments();
    paper_.resize(n);
    std::vector<double> maxq(n), mult(n);
    std::vector<std::string> feeds(n);
    for (size_t i = 0; i < n; ++i) {
      maxq[i] = cfg.instruments[i].max_order_qty > 0 ? cfg.instruments[i].max_order_qty : cfg.instruments[i].in.max_pos;
      mult[i] = cfg.instruments[i].in.mult;
      feeds[i] = cfg.instruments[i].feed;
    }
    gate_.resize(n, maxq, mult);
    gw_.attach(opt.orders, feeds);
    tops_.assign(n, TopOfBook{});
    acct_.assign(n, Account{});
    venue_pos_.assign(n, kNaN);
    for (size_t i = 0; i < n; ++i) {
      if (by_feed_.count(cfg.instruments[i].feed))
        throw std::runtime_error("feed symbol used twice: " + cfg.instruments[i].feed);
      by_feed_[cfg.instruments[i].feed] = static_cast<InstrumentId>(i);
    }
    paper_.set_fill_handler([this](const Fill& f) { on_fill(f); });
    paper_.set_cancel_handler([this](InstrumentId i, OrderId o) { on_done(i, o); });
  }

  bool gateway_mode() const { return opt_.orders != nullptr; }

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
    const bool report = ev.type == 'A' || ev.type == 'F' || ev.type == 'D' || ev.type == 'G';
    InstrumentId id = kNoInstrument;
    if (!report) {
      const auto it = by_feed_.find(ev.sym);
      if (it == by_feed_.end()) { ++unknown_; return; }
      id = it->second;
    } else if (!gateway_mode()) {
      return;  // execution reports in a journal replayed on paper: not ours
    }
    if (first_ts_ < 0) first_ts_ = ev.ts;
    now_ = std::max(now_, ev.ts);
    ++events_;
    gate_.set_time(now_);
    if (warm_ && !gateway_mode()) paper_.on_time(now_);
    switch (ev.type) {
      case 'B': {
        TopOfBook t{ev.a, ev.c, ev.b, ev.d, now_};
        tops_[id] = t;
        gate_.set_reference(id, t.mid());
        if (!warm_) { try_start(); break; }
        if (gateway_mode() && !loopback_) {
          // Own orders are part of the displayed size; take them out. If we
          // are the whole touch, the next level is unknown from top of book,
          // and passing the book on would let fair value follow our own quote.
          const double ob = gw_.own_at(id, Side::Buy, t.bid), oa = gw_.own_at(id, Side::Sell, t.ask);
          t.bid_sz -= ob; t.ask_sz -= oa;
          if ((ob > 0 && t.bid_sz <= 1e-12) || (oa > 0 && t.ask_sz <= 1e-12)) { ++alone_at_touch_; break; }
        } else {
          paper_.on_book(id, t);  // the market may run over our resting quotes first
        }
        eng_.on_book(id, t);
        break;
      }
      case 'T':
        if (warm_ && !gateway_mode()) paper_.on_trade(id, ev.a, ev.b, ev.c);
        break;
      case 'C':
        if (warm_) eng_.on_composite(id, ev.a, now_);
        else pending_composite_[id] = ev.a;
        break;
      case 'A': gw_.on_ack(parse_id(ev.sym)); break;
      case 'F': {
        Fill f;
        if (gw_.on_fill(parse_id(ev.sym), ev.a, ev.b, now_, f)) on_fill(f);
        break;
      }
      case 'D': {
        InstrumentId inst;
        const OrderId oid = parse_id(ev.sym);
        if (gw_.on_done(oid, inst)) on_done(inst, oid);
        if (ev.text.rfind("rejected", 0) == 0) ++venue_rejects_;
        break;
      }
      case 'P':
        if (!gateway_mode()) break;
        venue_pos_[id] = ev.a;
        if (warm_ && std::fabs(ev.a - acct_[id].pos) > opt_.position_tolerance)
          trip("position mismatch on " + eng_.instrument(id).symbol + ": venue " + std::to_string(ev.a) +
               ", ours " + std::to_string(acct_[id].pos));
        break;
      case 'G':
        if (ev.sym == "ready") gw_ready_ = true;
        else if (ev.sym == "loopback") loopback_ = true;  // dry run: our orders are not in the market's book
        else if (ev.sym == "down" && warm_) trip("gateway down");
        if (!warm_) try_start();
        break;
      default: break;
    }
    if (!warm_) return;
    if (eng_.halted() && !gate_.killed()) trip("engine loss limit");
    if (now_ >= next_timer_) {
      eng_.on_timer(now_);
      next_timer_ = now_ + ns(opt_.timer_s);
      const double v = pnl();  // drawdown sampled on the timer clock
      peak_pnl_ = std::max(peak_pnl_, v);
      max_dd_ = std::max(max_dd_, peak_pnl_ - v);
      if (!opt_.kill_file.empty() && !gate_.killed()) {
        std::error_code ec;
        if (std::filesystem::exists(opt_.kill_file, ec)) trip("kill file " + opt_.kill_file);
      }
    }
    if (opt_.state && now_ >= next_state_) {
      write_state(*opt_.state);
      next_state_ = now_ + ns(opt_.state_every_s);
    }
    if (opt_.status_every_s > 0 && opt_.log && now_ >= next_status_) {
      status(*opt_.log);
      next_status_ = now_ + ns(opt_.status_every_s);
    }
  }

  // Stop trading: no new orders, cancel everything resting.
  void trip(const std::string& why) {
    if (gate_.killed()) return;
    gate_.kill(why);
    if (!eng_.halted()) eng_.halt();
    if (gateway_mode()) gw_.kill(why);
    if (opt_.log) *opt_.log << "KILL: " << why << '\n';
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
  const Engine& engine() const { return eng_; }
  RiskGate& gate() { return gate_; }
  const RiskGate& gate() const { return gate_; }
  const PaperVenue& venue() const { return paper_; }
  const GatewayVenue& gateway() const { return gw_; }
  uint64_t orders_sent() const { return gateway_mode() ? gw_.orders_sent() : paper_.orders_sent(); }
  uint64_t cancels_sent() const { return gateway_mode() ? gw_.cancels_sent() : paper_.cancels_sent(); }
  uint64_t alone_at_touch() const { return alone_at_touch_; }

  void status(std::ostream& os) const {
    os << std::fixed << std::setprecision(1) << "[t+" << elapsed_s() << "s] pnl " << std::setprecision(2) << pnl()
       << "  risk " << eng_.risk() << "  fills " << fills_ << (eng_.halted() ? "  HALTED" : "") << '\n';
  }

  // One JSON line: what the engine believes it holds and has resting.
  void write_state(std::ostream& os) const {
    os << std::setprecision(12) << "{\"ts\":" << now_ << ",\"killed\":" << (gate_.killed() ? "true" : "false")
       << ",\"pnl\":" << pnl() << ",\"positions\":{";
    for (size_t i = 0; i < acct_.size(); ++i)
      os << (i ? "," : "") << '"' << eng_.instrument(i).symbol << "\":" << acct_[i].pos;
    os << "},\"open\":[";
    bool first = true;
    if (gateway_mode())
      for (const auto& [oid, o] : gw_.orders()) {
        if (o.ioc) continue;
        os << (first ? "" : ",") << "{\"id\":" << oid << ",\"sym\":\"" << eng_.instrument(o.inst).symbol
           << "\",\"side\":\"" << (o.side == Side::Buy ? 'B' : 'S') << "\",\"px\":" << o.px << ",\"left\":" << o.left
           << ",\"acked\":" << (o.acked ? "true" : "false") << '}';
        first = false;
      }
    os << "]}\n";
    os.flush();
  }

  void summary(std::ostream& os) const {
    os << "\nsession: " << std::fixed << std::setprecision(1) << elapsed_s() << " s of market data, " << events_
       << " events (" << unknown_ << " for unknown symbols, " << bad_lines_ << " unparsable lines)\n";
    if (!warm_) {
      os << "engine never started:";
      if (gateway_mode() && !gw_ready_) os << " the gateway never reported ready;";
      os << " no book received yet for";
      for (size_t i = 0; i < tops_.size(); ++i)
        if (eng_.instrument(i).proto == Protocol::Clob && !tops_[i].valid()) os << " [" << eng_.instrument(i).symbol << "]";
      os << '\n';
      return;
    }
    const EngineStats& st = eng_.stats();
    os << (gateway_mode() ? "venue P&L " : "paper P&L ") << std::setprecision(2) << pnl() << "   fills " << fills_
       << " (hedges " << st.hedge_fills << ")   volume " << std::setprecision(0) << volume_ << "   orders "
       << orders_sent() << "   cancels " << cancels_sent() << std::setprecision(1) << " ("
       << orders_sent() / std::max(1e-9, elapsed_s()) << " orders/s)"
       << (gate_.killed() ? "   KILLED: " + gate_.kill_reason() : "") << '\n';
    if (gate_.rejected_total() || gate_.ioc_self_cross()) {
      os << "pre-trade refusals:";
      for (int r = 1; r < static_cast<int>(Reject::Count); ++r)
        if (gate_.rejected(static_cast<Reject>(r))) os << ' ' << reject_name(static_cast<Reject>(r)) << ' ' << gate_.rejected(static_cast<Reject>(r));
      if (gate_.ioc_self_cross()) os << "   hedges crossing our own quote " << gate_.ioc_self_cross();
      os << '\n';
    }
    if (gateway_mode())
      os << "gateway: venue rejects " << venue_rejects_ << "   unknown reports " << gw_.unknown_reports()
         << "   overfills " << gw_.overfills() << "   book updates held (alone at touch) " << alone_at_touch_ << '\n';
    os << '\n';
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

  static OrderId parse_id(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

  // The engine starts once every order-book instrument has a market, so no
  // instrument is ever priced off a configured (stale) factor level. In
  // gateway mode the gateway must also be ready (it has cancelled leftover
  // orders and reported positions); those positions are taken over first.
  void try_start() {
    for (size_t i = 0; i < tops_.size(); ++i)
      if (eng_.instrument(i).proto == Protocol::Clob && !tops_[i].valid()) return;
    if (gateway_mode() && !gw_ready_) return;
    warm_ = true;
    eng_.start(now_);
    for (size_t i = 0; i < tops_.size(); ++i) {
      const double q = venue_pos_[i];
      if (!(q == q) || q == 0.0) continue;
      const double mark = tops_[i].valid() ? tops_[i].mid() : eng_.fair(static_cast<InstrumentId>(i));
      eng_.seed_position(static_cast<InstrumentId>(i), q, mark);
      acct_[i].pos = q;
      acct_[i].peak = std::fabs(q);
      acct_[i].cash = -q * mark * eng_.instrument(i).mult;
      if (opt_.log) *opt_.log << "taking over position " << eng_.instrument(i).symbol << ' ' << q << " at " << mark << '\n';
    }
    next_timer_ = now_ + ns(opt_.timer_s);
    next_status_ = now_ + ns(opt_.status_every_s);
    next_state_ = now_;
    if (!gateway_mode()) paper_.on_time(now_);
    for (int pass = 0; pass < 2; ++pass)  // factor reference markets first
      for (size_t i = 0; i < tops_.size(); ++i) {
        const Instrument& in = eng_.instrument(i);
        if (in.proto != Protocol::Clob || (in.ref_for_factor != kNoFactor) != (pass == 0)) continue;
        TopOfBook t = tops_[i];
        t.ts = now_;
        if (!gateway_mode()) paper_.on_book(static_cast<InstrumentId>(i), t);
        eng_.on_book(static_cast<InstrumentId>(i), t);
      }
    for (const auto& [id, px] : pending_composite_) eng_.on_composite(id, px, now_);
    pending_composite_.clear();
  }

  void on_done(InstrumentId i, OrderId o) {
    gate_.on_done(o);
    if (warm_) eng_.on_cancel_done(i, o);
  }

  void on_fill(const Fill& f) {
    gate_.on_fill(f.oid, f.qty);
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
    if (warm_) eng_.on_fill(f);
  }

  RunnerOptions opt_;
  PaperVenue paper_;
  GatewayVenue gw_;
  RiskGate gate_;
  Engine eng_;
  std::map<std::string, InstrumentId> by_feed_;
  std::map<InstrumentId, double> pending_composite_;
  std::vector<TopOfBook> tops_;
  std::vector<Account> acct_;
  std::vector<double> venue_pos_;
  TimeNs now_ = 0, first_ts_ = -1, next_timer_ = 0, next_status_ = 0, next_state_ = 0;
  bool warm_ = false, gw_ready_ = false, loopback_ = false;
  uint64_t events_ = 0, unknown_ = 0, bad_lines_ = 0, fills_ = 0, venue_rejects_ = 0, alone_at_touch_ = 0;
  double volume_ = 0.0, peak_pnl_ = 0.0, max_dd_ = 0.0;
};

}  // namespace amm::live
