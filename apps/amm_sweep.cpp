// Replay one recorded session under a grid of parameter settings and tabulate
// the paper results, so every setting is judged on the same market data.
//
//   amm_sweep --config config/binance_perp.ini --replay data/session.csv \
//             --set engine.gamma=0.01,0.05,0.2 --set engine.min_quote_life_ms=0,250,1000
//
// Each --set is SECTION.KEY=V1,V2,...; the grid is every combination. SECTION is
// engine, paper, factor:NAME, instrument:SYMBOL or instrument:* (all of them).
// The journal is read once and every run advances through it in lockstep,
// spread over --jobs threads (default: all cores); results are sorted by P&L.
// --split N also reports P&L in N equal time slices, to show whether a setting
// wins throughout or in one stretch. --csv FILE writes one row per run.
// --jitter N replays every setting N more times with each instrument's A0 and
// k0 scaled by a fixed random factor within +-1%, and summarises each setting
// by mean and spread. Paper P&L is strongly path dependent (one different fill
// changes every later one), so on a long session two runs that differ by a
// rounding can end 50 apart; compare settings on the ensemble, not one path.
// Same caveats as any paper result: fills come from public prints, so spread
// capture is understated (DESIGN.md §11), and a grid picked on one session is
// fitted to it: confirm the choice on a different session.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cmath>
#include <random>
#include <memory>
#include <sstream>
#include <thread>

#include "amm/live/runner.hpp"

namespace {
using namespace amm::live;

struct Axis { std::string section, key; std::vector<std::string> values; };
struct Result {
  std::vector<std::string> values;
  size_t point = 0;  // grid point; jittered copies share it
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

// Time of the last event, read from the end of the file, for slicing.
amm::TimeNs last_ts(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const std::streamoff size = in.tellg();
  for (std::streamoff back = 4096; ; back *= 4) {
    in.clear();
    in.seekg(std::max<std::streamoff>(0, size - back));
    std::string line;
    MdEvent ev;
    amm::TimeNs t = -1;
    if (back < size) std::getline(in, line);  // probably partial
    while (std::getline(in, line)) if (parse_event(line, ev)) t = std::max(t, ev.ts);
    if (t >= 0 || back >= size) return t;
  }
}

// One configuration being replayed, with its time-slice bookkeeping.
struct Run {
  std::unique_ptr<Runner> runner;
  int slice = 0;
  double at_slice_start = 0.0;
  Result r;
};

void step(Run& run, const MdEvent& ev, int split, amm::TimeNs t0, double width) {
  if (!run.r.error.empty()) return;
  try {
    while (width > 0 && run.slice < split - 1 && ev.ts >= t0 + amm::TimeNs((run.slice + 1) * width)) {
      const double v = run.runner->pnl();
      run.r.slices.push_back(v - run.at_slice_start);
      run.at_slice_start = v;
      ++run.slice;
    }
    run.runner->on_event(ev);
  } catch (const std::exception& e) {
    run.r.error = e.what();
  }
}

void finish(Run& run, const LiveConfig& cfg, double width) {
  Result& r = run.r;
  if (!r.error.empty()) return;
  const Runner& runner = *run.runner;
  if (width > 0) r.slices.push_back(runner.pnl() - run.at_slice_start);
  r.warm = runner.warm();
  r.pnl = runner.pnl();
  r.dd = runner.max_drawdown();
  r.volume = runner.volume();
  r.fills = runner.fills();
  r.orders_s = runner.venue().orders_sent() / std::max(1e-9, runner.elapsed_s());
  r.halted = run.runner->engine().halted();
  for (size_t i = 0; i < cfg.instruments.size(); ++i) {
    const double mp = cfg.instruments[i].in.max_pos;
    r.peak_frac.push_back(mp > 0 ? runner.peak_position(static_cast<amm::InstrumentId>(i)) / mp : 0.0);
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string config, replay, csv;
  std::vector<Axis> axes;
  int split = 0, jitter = 0;
  unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
  try {
    for (int i = 1; i < argc; ++i) {
      const auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
      if (arg("--config")) config = argv[++i];
      else if (arg("--replay")) replay = argv[++i];
      else if (arg("--set")) axes.push_back(parse_axis(argv[++i]));
      else if (arg("--csv")) csv = argv[++i];
      else if (arg("--split")) split = std::max(0, std::atoi(argv[++i]));
      else if (arg("--jitter")) jitter = std::max(0, std::atoi(argv[++i]));
      else if (arg("--jobs")) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
      else throw std::runtime_error(std::string("unknown argument ") + argv[i]);
    }
    if (config.empty() || replay.empty())
      throw std::runtime_error("usage: amm_sweep --config FILE --replay FILE [--set SECTION.KEY=V1,V2,...]... "
                               "[--split N] [--jitter N] [--jobs N] [--csv FILE]");
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
    std::vector<size_t> point_of;
    for (size_t gi = 0; gi < grid.size(); ++gi) {
      std::string text = base;
      for (size_t k = 0; k < axes.size(); ++k) text = set_ini_value(text, axes[k].section, axes[k].key, grid[gi][k]);
      std::istringstream is(text);
      const LiveConfig c = parse_config(is, config);
      for (int j = 0; j <= jitter; ++j) {
        LiveConfig cj = c;
        if (j > 0) {  // the same perturbation j at every grid point
          std::mt19937_64 rng(static_cast<uint64_t>(j));
          std::uniform_real_distribution<double> u(-0.01, 0.01);
          for (auto& ic : cj.instruments) { ic.in.A0 *= 1.0 + u(rng); ic.in.k0 *= 1.0 + u(rng); }
        }
        cfgs.push_back(cj);
        point_of.push_back(gi);
      }
    }

    // Lockstep: the journal is read and parsed once, and each batch of events
    // is fed to every run, the runs spread over the worker threads. Separate
    // readers per run thrash the file cache once a journal outgrows free memory.
    std::vector<Run> runs(cfgs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
      runs[i].r.values = grid[point_of[i]];
      runs[i].r.point = point_of[i];
      try { runs[i].runner = std::make_unique<Runner>(cfgs[i], RunnerOptions{}); }
      catch (const std::exception& e) { runs[i].r.error = e.what(); }
    }
    std::ifstream in(replay);
    std::vector<MdEvent> batch(1 << 15);
    size_t n = 0;
    std::string line;
    amm::TimeNs t0 = -1;
    double width = 0.0;
    const unsigned workers = static_cast<unsigned>(std::min<size_t>(jobs, runs.size()));
    auto feed = [&] {
      std::vector<std::thread> pool;
      for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&, w] {
          for (size_t i = w; i < runs.size(); i += workers)
            for (size_t e = 0; e < n; ++e) step(runs[i], batch[e], split, t0, width);
        });
      for (auto& t : pool) t.join();
      n = 0;
    };
    while (std::getline(in, line)) {
      if (!parse_event(line, batch[n])) continue;
      if (t0 < 0) {
        t0 = batch[n].ts;
        const amm::TimeNs t1 = split > 0 ? last_ts(replay) : t0;
        width = split > 0 && t1 > t0 ? double(t1 - t0) / split : 0.0;
      }
      if (++n == batch.size()) feed();
    }
    feed();
    std::vector<Result> res;
    for (size_t i = 0; i < runs.size(); ++i) {
      finish(runs[i], cfgs[i], width);
      res.push_back(std::move(runs[i].r));
    }

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
    if (jitter > 0) {
      struct Agg { double sum = 0, sq = 0, lo = 1e300, dd = 0, ord = 0; int n = 0; };
      std::vector<Agg> agg(grid.size());
      for (const Result& r : res) {
        if (!r.error.empty() || !r.warm) continue;
        Agg& a = agg[r.point];
        a.sum += r.pnl; a.sq += r.pnl * r.pnl; a.lo = std::min(a.lo, r.pnl); a.dd += r.dd; a.ord += r.orders_s; ++a.n;
      }
      std::vector<size_t> pts(grid.size());
      for (size_t i = 0; i < pts.size(); ++i) pts[i] = i;
      auto mean = [&](size_t i) { return agg[i].n ? agg[i].sum / agg[i].n : -1e300; };
      std::stable_sort(pts.begin(), pts.end(), [&](size_t a, size_t b) { return mean(a) > mean(b); });
      std::printf("\nby setting, over %d jittered copies each (A0, k0 within +-1%%):\n", jitter + 1);
      for (const Axis& a : axes) std::printf("%-*s ", int(std::max<size_t>(10, (a.section + "." + a.key).size())), (a.section + "." + a.key).c_str());
      std::printf("%10s %9s %9s %9s %9s %6s\n", "mean P&L", "st.dev", "worst", "mean dd", "orders/s", "runs");
      for (size_t i : pts) {
        const Agg& a = agg[i];
        for (size_t k = 0; k < axes.size(); ++k)
          std::printf("%-*s ", int(std::max<size_t>(10, (axes[k].section + "." + axes[k].key).size())), grid[i][k].c_str());
        if (!a.n) { std::printf("no completed runs\n"); continue; }
        const double m = a.sum / a.n, sd = a.n > 1 ? std::sqrt(std::max(0.0, (a.sq - a.n * m * m) / (a.n - 1))) : 0.0;
        std::printf("%10.2f %9.2f %9.2f %9.2f %9.1f %6d\n", m, sd, a.lo, a.dd / a.n, a.ord / a.n, a.n);
      }
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
