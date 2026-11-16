// Record a journal of the simulated market (no market maker present) in the
// normalised event format, for offline testing of amm_run and amm_calibrate.
//
//   amm_record_sim --out data/sim.csv [--seed N] [--duration SECONDS]
#include <cstring>
#include <fstream>
#include <iostream>

#include "amm/live/events.hpp"
#include "amm/sim/scenario.hpp"

int main(int argc, char** argv) {
  std::string out;
  amm::sim::SimConfig sc;
  sc.duration_s = 600;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) sc.seed = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--duration") && i + 1 < argc) sc.duration_s = std::atof(argv[++i]);
  }
  std::ofstream f(out);
  if (out.empty() || !f) { std::cerr << "usage: amm_record_sim --out FILE [--seed N] [--duration SECONDS]\n"; return 2; }
  const amm::sim::Universe u = amm::sim::default_universe();
  amm::sim::Sim sim(u, sc);
  unsigned long long n = 0;
  f << "# simulated market, seed " << sc.seed << ", " << sc.duration_s << " s\n";
  sim.tap = [&](amm::TimeNs ts, char type, amm::InstrumentId i, double a, double b, double c, double d) {
    amm::live::MdEvent ev;
    ev.ts = ts + 1; ev.type = type; ev.sym = u.insts[i].in.symbol;  // +1: timestamps start above zero
    ev.a = a; ev.b = b; ev.c = c; ev.d = d;
    f << amm::live::format_event(ev) << '\n';
    ++n;
  };
  sim.run();
  std::cout << "wrote " << n << " events to " << out << '\n';
  return 0;
}
