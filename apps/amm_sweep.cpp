// Replay one recorded session under a grid of parameter settings and tabulate
// the paper results, so every setting is judged on the same market data.
//
//   amm_sweep --config config/binance_perp.ini --replay data/session.csv \
//             --set engine.gamma=0.01,0.05,0.2 --set engine.min_quote_life_ms=0,250,1000
//
// Each --set is SECTION.KEY=V1,V2,...; the grid is every combination. SECTION is
// engine, paper, factor:NAME, instrument:SYMBOL or instrument:* (all of them).
// Runs go in parallel (--jobs, default: all cores); results are sorted by P&L.
// --split N also reports P&L in N equal time slices, to show whether a setting
// wins throughout or in one stretch. --csv FILE writes one row per run.
// Same caveats as any paper result: fills come from public prints, so spread
// capture is understated (DESIGN.md §11), and a grid picked on one session is
// fitted to it: confirm the choice on a different session.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "amm/live/runner.hpp"

namespace {
using namespace amm::live;

struct Axis { std::string section, key; std::vector<std::string> values; };
struct Result {
  std::vector<std::string> values;
  double pnl = 0, dd = 0, volume = 0, orders_s = 0;
  uint64_t fills = 0;
  std::vector<double> peak_frac;  // peak |position| / max_pos per instrument
  std::vector<double> slices;     // P&L earned in each time slice
  bool halted = false, warm = false;
  std::string error;
};

Axis parse_axis(const std::string& s) {
  const size_t eq = s.find('=');
  const size_t dot = eq == std::string::npos ? std::string::npos : s.substr(0, eq).rfind('.');
  if (eq == std::string::npos || dot == std::string::npos)
    throw std::runtime_error("--set expects SECTION.KEY=V1,V2,... got '" + s + "'");
  Axis a{s.substr(0, dot), s.substr(dot + 1, eq - dot - 1), {}};
  std::stringstream vs(s.substr(eq + 1));
  for (std::string v; std::getline(vs, v, ',');)
    if (!v.empty()) a.values.push_back(v);
  if (a.values.empty()) throw std::runtime_error("--set " + s + ": no values");
  return a;
}

// First and last event time, for slicing.
std::pair<amm::TimeNs, amm::TimeNs> time_span(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  MdEvent ev;
  amm::TimeNs lo = -1, hi = -1;
  while (std::getline(in, line))
    if (parse_event(line, ev)) { if (lo < 0) lo = ev.ts; hi = std::max(hi, ev.ts); }
  return {lo, hi};
}

Result run_one(const LiveConfig& cfg, const std::string& replay, int split, amm::TimeNs t0, amm::TimeNs t1) {
  Result r;
  RunnerOptions opt;
  Runner runner(cfg, opt);
  std::ifstream in(replay);
  std::string line;
  MdEvent ev;
  const double width = split > 0 && t1 > t0 ? double(t1 - t0) / split : 0.0;
  int slice = 0;
  double at_slice_start = 0.0;
  while (std::getline(in, line)) {
    if (!parse_event(line, ev)) continue;
    while (width > 0 && slice < split - 1 && ev.ts >= t0 + amm::TimeNs((slice + 1) * width)) {
      r.slices.push_back(runner.pnl() - at_slice_start);
      at_slice_start = runner.pnl();
      ++slice;
    }
    runner.on_event(ev);
  }
  if (width > 0) r.slices.push_back(runner.pnl() - at_slice_start);
  r.warm = runner.warm();
  r.pnl = runner.pnl();
  r.dd = runner.max_drawdown();
  r.volume = runner.volume();
  r.fills = runner.fills();
  r.orders_s = runner.venue().orders_sent() / std::max(1e-9, runner.elapsed_s());
  r.halted = runner.engine().halted();
  for (size_t i = 0; i < cfg.instruments.size(); ++i) {
    const double mp = cfg.instruments[i].in.max_pos;
    r.peak_frac.push_back(mp > 0 ? runner.peak_position(static_cast<amm::InstrumentId>(i)) / mp : 0.0);
  }
  return r;
}
}  // namespace

int main(int argc, char** argv) {
  std::string config, replay, csv;
  std::vector<Axis> axes;
  int split = 0;
  unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
  try {
    for (int i = 1; i < argc; ++i) {
      const auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
      if (arg("--config")) config = argv[++i];
      else if (arg("--replay")) replay = argv[++i];
      else if (arg("--set")) axes.push_back(parse_axis(argv[++i]));
      else if (arg("--csv")) csv = argv[++i];
      else if (arg("--split")) split = std::max(0, std::atoi(argv[++i]));
      else if (arg("--jobs")) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
      else throw std::runtime_error(std::string("unknown argument ") + argv[i]);
    }
    if (config.empty() || replay.empty())
      throw std::runtime_error("usage: amm_sweep --config FILE --replay FILE [--set SECTION.KEY=V1,V2,...]... "
                               "[--split N] [--jobs N] [--csv FILE]");
    std::ifstream cf(config);
    if (!cf) throw std::runtime_error("cannot open config file: " + config);
    const std::string base((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
    if (!std::ifstream(replay)) throw std::runtime_error("cannot open replay file: " + replay);

    // Every combination, first axis varying slowest. All configs are built and
    // parsed before any run, so a bad key fails in a second, not after an hour.
    std::vector<std::vector<std::string>> grid{{}};
    for (const Axis& a : axes) {
      std::vector<std::vector<std::string>> next;
      for (const auto& g : grid)
        for (const auto& v : a.values) { auto h = g; h.push_back(v); next.push_back(h); }
      grid = next;
    }
    std::vector<LiveConfig> cfgs;
    for (const auto& g : grid) {
      std::string text = base;
      for (size_t k = 0; k < axes.size(); ++k) text = set_ini_value(text, axes[k].section, axes[k].key, g[k]);
      std::istringstream is(text);
      cfgs.push_back(parse_config(is, config));
    }

    amm::TimeNs t0 = 0, t1 = 0;
    if (split > 0) std::tie(t0, t1) = time_span(replay);
    std::vector<Result> res(grid.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < std::min<size_t>(jobs, grid.size()); ++w)
      pool.emplace_back([&] {
        for (size_t i; (i = next++) < grid.size();) {
          try { res[i] = run_one(cfgs[i], replay, split, t0, t1); }
          catch (const std::exception& e) { res[i].error = e.what(); }
          res[i].values = grid[i];
        }
      });
    for (auto& t : pool) t.join();

    std::vector<size_t> order(res.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return res[a].pnl > res[b].pnl; });

    const LiveConfig& c0 = cfgs.front();
    std::printf("%zu runs on %s\n\n", res.size(), replay.c_str());
    for (const Axis& a : axes) std::printf("%-*s ", int(std::max<size_t>(10, (a.section + "." + a.key).size())), (a.section + "." + a.key).c_str());
    std::printf("%10s %9s %8s %12s %9s", "P&L", "max dd", "fills", "volume", "orders/s");
    for (const auto& ic : c0.instruments) std::printf(" %10.10s", ("peak " + ic.in.symbol).c_str());
    for (int s = 0; s < split; ++s) std::printf(" %8s", ("slice" + std::to_string(s + 1)).c_str());
    std::printf("\n");
    for (size_t i : order) {
      const Result& r = res[i];
      for (size_t k = 0; k < axes.size(); ++k)
        std::printf("%-*s ", int(std::max<size_t>(10, (axes[k].section + "." + axes[k].key).size())), r.values[k].c_str());
      if (!r.error.empty()) { std::printf("error: %s\n", r.error.c_str()); continue; }
      if (!r.warm) { std::printf("engine never started (no book for every instrument)\n"); continue; }
      std::printf("%10.2f %9.2f %8llu %12.0f %9.1f", r.pnl, r.dd, static_cast<unsigned long long>(r.fills), r.volume, r.orders_s);
      for (double p : r.peak_frac) std::printf(" %9.0f%%", 100 * p);
      for (double s : r.slices) std::printf(" %8.2f", s);
      std::printf("%s\n", r.halted ? "  HALTED" : "");
    }
    std::printf("\npeak = largest |position| as a share of max_pos. Paper P&L understates spread capture;\n"
                "a setting chosen on one session is fitted to it, so confirm it on another.\n");

    if (!csv.empty()) {
      std::ofstream out(csv);
      if (!out) throw std::runtime_error("cannot write " + csv);
      for (const Axis& a : axes) out << a.section << '.' << a.key << ',';
      out << "pnl,max_dd,fills,volume,orders_per_s,halted";
      for (const auto& ic : c0.instruments) out << ",peak_frac_" << ic.in.symbol;
      for (int s = 0; s < split; ++s) out << ",slice" << s + 1;
      out << ",error\n";
      for (size_t i : order) {
        const Result& r = res[i];
        for (const auto& v : r.values) out << v << ',';
        out << r.pnl << ',' << r.dd << ',' << r.fills << ',' << r.volume << ',' << r.orders_s << ',' << r.halted;
        for (size_t k = 0; k < c0.instruments.size(); ++k) out << ',' << (k < r.peak_frac.size() ? r.peak_frac[k] : 0.0);
        for (int s = 0; s < split; ++s) out << ',' << (size_t(s) < r.slices.size() ? r.slices[s] : 0.0);
        out << ',' << r.error << '\n';
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "amm_sweep: " << e.what() << '\n';
    return 1;
  }
}
