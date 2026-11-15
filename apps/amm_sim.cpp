// Runs the reference cross-asset scenario for each engine mode over several
// seeds and prints a comparison. Every mode sees the identical market: same
// seed means the same factor paths, jumps, taker flow and RFQ clients.
//
//   amm_sim [--seeds N] [--duration SECONDS] [--gamma G] [--csv FILE] [--detail]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "amm/sim/scenario.hpp"

using namespace amm;
using namespace amm::sim;

struct Agg {
  double pnl = 0, pnl2 = 0, sharpe = 0, sd1 = 0, sniped = 0, hedge = 0, dd = 0, risk = 0, vol = 0;
  double fills = 0, rfq_hit = 0, msgs = 0;
  std::vector<double> inst, snp, edge;
};

int main(int argc, char** argv) {
  int seeds = 8;
  double duration = 1800.0, gamma = -1.0;
  const char* csv = nullptr;
  bool detail = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) duration = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--gamma") && i + 1 < argc) gamma = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
    else if (!std::strcmp(argv[i], "--detail")) detail = true;
    else { std::printf("usage: amm_sim [--seeds N] [--duration S] [--gamma G] [--csv FILE] [--detail]\n"); return 1; }
  }
  const Universe u = default_universe();
  const Mode modes[] = {Mode::Naive, Mode::Siloed, Mode::FairOnly, Mode::SkewOnly, Mode::Unified};
  const int M = 5;
  Agg agg[5];
  std::ofstream out;
  if (csv) { out.open(csv); out << "mode,seed,second,pnl\n"; }

  for (int m = 0; m < M; ++m) {
    agg[m].inst.assign(u.insts.size(), 0.0);
    agg[m].snp.assign(u.insts.size(), 0.0);
    agg[m].edge.assign(u.insts.size(), 0.0);
    for (int s = 1; s <= seeds; ++s) {
      SimConfig sc;
      sc.seed = static_cast<uint64_t>(s);
      sc.duration_s = duration;
      Sim sim(u, sc);
      EngineConfig ec = mode_config(modes[m]);
      if (gamma > 0) ec.gamma = gamma;
      Engine eng(ec, &sim);
      build_engine(u, eng);
      sim.attach(&eng);
      const SimResult r = sim.run();
      Agg& a = agg[m];
      a.pnl += r.pnl; a.pnl2 += r.pnl * r.pnl; a.sharpe += r.sharpe; a.sd1 += r.pnl_sd_1s;
      a.sniped += r.sniped_loss; a.hedge += r.hedge_cost; a.dd += r.max_drawdown;
      a.fills += static_cast<double>(r.fills); a.vol += r.volume;
      a.rfq_hit += r.rfq ? static_cast<double>(r.rfq_won) / static_cast<double>(r.rfq) : 0.0;
      const EngineStats& st = eng.stats();
      a.risk += st.risk_time > 0 ? st.risk_integral / st.risk_time : 0.0;
      a.msgs += static_cast<double>(st.orders + st.cancels);
      for (size_t i = 0; i < r.inst_pnl.size(); ++i) {
        a.inst[i] += r.inst_pnl[i]; a.snp[i] += r.inst_sniped[i]; a.edge[i] += r.inst_edge[i];
      }
      if (csv)
        for (size_t t = 0; t < r.pnl_path.size(); ++t)
          out << mode_name(modes[m]) << ',' << s << ',' << t << ',' << r.pnl_path[t] << '\n';
    }
  }

  const double n = seeds;
  std::printf("\nCross-asset market-making simulation: %d seeds x %.0f s, 12 instruments, 5 risk factors\n\n", seeds, duration);
  std::printf("%-9s %10s %10s %8s %9s %9s %9s %10s %9s %7s %8s\n", "mode", "mean P&L", "sd(P&L)",
              "Sharpe*", "P&L sd/s", "avg risk", "sniped", "hedge cost", "max DD", "fills", "RFQ hit");
  for (int m = 0; m < M; ++m) {
    const Agg& a = agg[m];
    const double mean = a.pnl / n;
    const double sd = n > 1 ? std::sqrt(std::max(0.0, (a.pnl2 - n * mean * mean) / (n - 1))) : 0.0;
    std::printf("%-9s %10.0f %10.0f %8.1f %9.0f %9.1f %9.0f %10.0f %9.0f %7.0f %7.1f%%\n", mode_name(modes[m]),
                mean, sd, a.sharpe / n, a.sd1 / n, a.risk / n, a.sniped / n, a.hedge / n, a.dd / n,
                a.fills / n, 100.0 * a.rfq_hit / n);
  }
  std::printf("\n* Sharpe = mean / st.dev of 1-second P&L increments x sqrt(seconds), per run, not annualised.\n"
              "  P&L sd/s = realised st.dev of 1-second P&L. avg risk = engine's own time-average portfolio\n"
              "  st.dev estimate, currency per sqrt(second). sniped = loss on fills taken by latency\n"
              "  arbitrageurs after jumps. P&L is marked to the simulator's true values.\n");
  if (detail) {
    std::printf("\nPer instrument, mean per run: edge = fill price vs true value at the fill, summed over\n"
                "quote fills (includes sniped fills); sniped = the part lost to latency arbitrage.\n\n");
    for (int pass = 0; pass < 2; ++pass) {
      std::printf("%s\n%-20s", pass ? "\nsniped loss" : "edge captured", "instrument");
      for (int m = 0; m < M; ++m) std::printf(" %9s", mode_name(modes[m]));
      std::printf("\n");
      for (size_t i = 0; i < u.insts.size(); ++i) {
        std::printf("%-20s", u.insts[i].in.symbol.c_str());
        for (int m = 0; m < M; ++m) std::printf(" %9.0f", (pass ? agg[m].snp[i] : agg[m].edge[i]) / n);
        std::printf("\n");
      }
    }
  }
  return 0;
}
