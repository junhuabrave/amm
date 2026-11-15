// Tick-to-quote latency of the engine in isolation: time from a book update
// entering on_book() to all resulting order actions having been issued to a
// no-op venue. Two cases: an update on a leaf market (reprices one
// instrument) and an update on the equity underlying (reprices and requotes
// the stock, four options and the warrant).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "amm/sim/scenario.hpp"

using namespace amm;
using namespace amm::sim;

struct NullVenue final : Venue {
  OrderId next = 0;
  uint64_t n = 0;
  OrderId submit(InstrumentId, Side, double, double, bool, bool) override { ++n; return ++next; }
  void cancel(InstrumentId, OrderId) override { ++n; }
};

static void report(const char* name, std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto q = [&](double p) { return v[static_cast<size_t>(p * (v.size() - 1))]; };
  std::printf("%-34s p50 %7.0f ns   p99 %7.0f ns   p99.9 %7.0f ns\n", name, q(0.5), q(0.99), q(0.999));
}

int main() {
  const Universe u = default_universe();
  const Ids id;
  NullVenue venue;
  Engine eng(mode_config(Mode::Unified), &venue);
  build_engine(u, eng);
  eng.start(0);
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  TimeNs ts = 1000;
  // Seed every book once.
  for (size_t i = 0; i < u.insts.size(); ++i) {
    if (u.insts[i].in.proto != Protocol::Clob) continue;
    const double p = eng.fair(static_cast<InstrumentId>(i)), t = u.insts[i].in.tick;
    eng.on_book(static_cast<InstrumentId>(i), TopOfBook{floor_to(p - u.insts[i].bg_half, t), ceil_to(p + u.insts[i].bg_half, t), 100, 100, ts += 1000});
  }
  const int N = 400000;
  std::vector<double> leaf, under;
  leaf.reserve(N); under.reserve(N);
  double s = 100.0, w = eng.fair(id.warrant);
  for (int i = 0; i < 2 * N; ++i) {
    const bool is_under = i & 1;
    TopOfBook b;
    if (is_under) {
      s += (U(rng) - 0.5) * 0.02;
      b = TopOfBook{floor_to(s - 0.03, 0.01), ceil_to(s + 0.03, 0.01), 100 + 400 * U(rng), 100 + 400 * U(rng), ts += 50000};
    } else {
      w += (U(rng) - 0.5) * 0.001;
      b = TopOfBook{floor_to(w - 0.006, 0.001), ceil_to(w + 0.006, 0.001), 5e4 * (0.5 + U(rng)), 5e4 * (0.5 + U(rng)), ts += 50000};
    }
    const auto t0 = std::chrono::steady_clock::now();
    eng.on_book(is_under ? id.stock : id.warrant, b);
    const auto t1 = std::chrono::steady_clock::now();
    (is_under ? under : leaf).push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    if (i % 20 == 0) eng.on_timer(ts);
  }
  std::printf("tick-to-quote, %d samples each, unified mode, 12 instruments\n", N);
  report("leaf update (1 instrument)", leaf);
  report("underlying update (6 instruments)", under);
  std::printf("order actions issued: %llu\n", static_cast<unsigned long long>(venue.n));
  return 0;
}
