// Estimate engine inputs from a recorded journal and print them as a table
// and as config lines to paste into an INI file.
//
//   amm_calibrate data/session.csv [--horizon SECONDS] [--sample SECONDS]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include "amm/live/calibrate.hpp"

int main(int argc, char** argv) {
  std::string path;
  amm::live::CalibrationOptions opt;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--horizon") && i + 1 < argc) opt.horizon_s = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--sample") && i + 1 < argc) opt.sample_s = std::atof(argv[++i]);
    else path = argv[i];
  }
  std::ifstream in(path);
  if (path.empty() || !in) {
    std::cerr << "usage: amm_calibrate JOURNAL.csv [--horizon SECONDS] [--sample SECONDS]\n";
    return 2;
  }
  const amm::live::Calibration c = amm::live::calibrate(in, opt);
  if (c.symbols.empty()) { std::cerr << "no usable book data in " << path << '\n'; return 1; }
  std::printf("%-22s %8s %7s %7s %11s %11s %11s %9s %9s %9s %10s\n", "symbol", "secs", "books", "trades",
              "mid", "spread", "vol/sqrt(s)", "jumps/s", "jump sd", "A (1/s)", "k (1/px)");
  for (const auto& s : c.symbols)
    std::printf("%-22s %8.0f %7.0f %7.0f %11.6g %11.4g %11.4g %9.4f %9.4g %9.3g %10.4g\n", s.sym.c_str(),
                s.duration_s, s.books, s.trades, s.mid, s.mean_spread, s.vol, s.jump_rate, s.jump_sd, s.A, s.k);
  const size_t n = c.symbols.size();
  if (n > 1) {
    std::printf("\ncorrelation of %.0f s mid changes\n%-22s", opt.sample_s, "");
    for (size_t b = 0; b < n; ++b) std::printf(" %8.8s", c.symbols[b].sym.c_str());
    std::printf("\n");
    for (size_t a = 0; a < n; ++a) {
      std::printf("%-22s", c.symbols[a].sym.c_str());
      for (size_t b = 0; b < n; ++b) std::printf(" %8.2f", c.corr[a * n + b]);
      std::printf("\n");
    }
  }
  std::printf("\n# config lines (vol goes on the factor the symbol is the reference for)\n");
  for (const auto& s : c.symbols) {
    std::printf("# %s\nvol = %.6g\n", s.sym.c_str(), s.vol_total > 0 ? s.vol_total : s.vol);
    if (s.A > 0) std::printf("A0 = %.4g\nk0 = %.6g\n", s.A, s.k);
    else std::printf("# A0/k0 not identifiable from this journal (%d usable depth levels); keep priors\n", s.intensity_points);
  }
  std::printf("\nA and k describe fills at and beyond the touch as seen in public prints. Inside the spread\n"
              "they are an extrapolation; the engine's online estimator corrects them from its own fills.\n");
  return 0;
}
