// Calibration of engine inputs from a recorded journal of normalised events:
// volatility, jump statistics, cross-symbol correlation, spreads, and the
// fill-intensity curve A*exp(-k*d).
#pragma once
#include <algorithm>
#include <cmath>
#include <istream>
#include <map>
#include <string>
#include <vector>

#include "amm/live/events.hpp"

namespace amm::live {

struct SymbolCalibration {
  std::string sym;
  double duration_s = 0, books = 0, trades = 0;
  double mid = 0, mean_spread = 0, min_increment = 0;
  double vol = 0;        // diffusive (jump-free) st.dev of the mid per sqrt(second)
  double vol_total = 0;  // including jumps
  double jump_rate = 0, jump_sd = 0;  // sampled moves beyond jump_sigmas diffusive sigmas
  double trade_rate = 0; // prints per second, per side
  double A = 0, k = 0;   // fill intensity per side; 0 if not identifiable
  int intensity_points = 0;
};
struct Calibration {
  std::vector<SymbolCalibration> symbols;
  std::vector<double> corr;  // symbols x symbols, of 1 s mid changes
};

struct CalibrationOptions {
  double sample_s = 1.0;    // sampling interval for volatility and quote starts
  double horizon_s = 10.0;  // how long a hypothetical quote is left to rest
  int levels = 8;           // distances tested
  double jump_sigmas = 5.0; // a sampled move beyond this many diffusive sigmas is a jump
};

// Fill intensity is measured the way a resting quote experiences it: at each
// sample time a hypothetical bid is placed d below the prevailing mid (and an
// offer d above) and left for `horizon`; it counts as filled when a trade
// prints at or through it. lambda(d) = fills / time-at-risk, then a weighted
// log-linear fit gives A and k. Public prints only reveal demand at and
// beyond the touch, so the curve inside the spread is an extrapolation; the
// engine's online estimator refines it from its own fills.
inline Calibration calibrate(std::istream& in, CalibrationOptions opt = {}) {
  struct Series {
    std::vector<TimeNs> bt; std::vector<double> mid, spr;
    std::vector<TimeNs> tt; std::vector<double> tp, ts;
    double min_inc = 0, last_bid = kNaN;
  };
  std::map<std::string, Series> data;
  std::string line;
  MdEvent ev;
  while (std::getline(in, line)) {
    if (!parse_event(line, ev)) continue;
    Series& s = data[ev.sym];
    if (ev.type == 'B' && ev.c > ev.a) {
      s.bt.push_back(ev.ts); s.mid.push_back(0.5 * (ev.a + ev.c)); s.spr.push_back(ev.c - ev.a);
      for (double px : {ev.a, ev.c}) {
        if (s.last_bid == s.last_bid) {
          const double d = std::fabs(px - s.last_bid);
          if (d > 1e-12 && (s.min_inc == 0 || d < s.min_inc)) s.min_inc = d;
        }
        s.last_bid = px;
      }
    } else if (ev.type == 'T') {
      s.tt.push_back(ev.ts); s.tp.push_back(ev.a); s.ts.push_back(ev.c);
    }
  }
  Calibration out;
  std::vector<std::vector<double>> grids;  // 1 s mid changes on a common clock, per symbol
  TimeNs t_lo = 0, t_hi = 0;
  bool have = false;
  for (const auto& [sym, s] : data)
    if (s.bt.size() >= 2) {
      t_lo = have ? std::min(t_lo, s.bt.front()) : s.bt.front();
      t_hi = have ? std::max(t_hi, s.bt.back()) : s.bt.back();
      have = true;
    }
  const TimeNs step = ns(opt.sample_s);
  for (const auto& [sym, s] : data) {
    if (s.bt.size() < 2) continue;
    SymbolCalibration c;
    c.sym = sym;
    c.duration_s = secs(s.bt.back() - s.bt.front());
    c.books = static_cast<double>(s.bt.size());
    c.trades = static_cast<double>(s.tt.size());
    c.min_increment = s.min_inc;
    double sm = 0, ss = 0;
    for (size_t i = 0; i < s.mid.size(); ++i) { sm += s.mid[i]; ss += s.spr[i]; }
    c.mid = sm / s.mid.size();
    c.mean_spread = ss / s.spr.size();
    // --- volatility on a regular grid (last observation carried forward)
    std::vector<double> g, r;
    std::vector<size_t> gi;  // index of the book in force at each grid time
    size_t j = 0;
    for (TimeNs t = t_lo; t <= t_hi; t += step) {
      while (j + 1 < s.bt.size() && s.bt[j + 1] <= t) ++j;
      const bool live = s.bt[j] <= t;
      g.push_back(live ? s.mid[j] : kNaN);
      gi.push_back(j);
    }
    std::vector<double> dg(g.size() > 0 ? g.size() - 1 : 0, 0.0);
    for (size_t i = 1; i < g.size(); ++i)
      if (g[i] == g[i] && g[i - 1] == g[i - 1]) { dg[i - 1] = g[i] - g[i - 1]; r.push_back(dg[i - 1]); }
    grids.push_back(dg);
    if (r.size() >= 10) {
      // Jump-robust volatility by iterated truncation (Mancini): start from the
      // total, drop moves beyond 5 sigma, re-estimate, repeat until the set of
      // jumps stops changing. A median-based scale fails on real tapes, where
      // the mid sits still for half of all 1 s samples: the median move is
      // zero or one tick, and every ordinary move then looks like a jump.
      double v = 0;
      for (double x : r) v += x * x;
      c.vol_total = std::sqrt(v / r.size() / opt.sample_s);
      double sd = std::sqrt(v / r.size());
      int jn = 0;
      for (int it = 0; it < 50 && sd > 0; ++it) {
        double kept = 0; int n_kept = 0, n_jump = 0;
        for (double x : r)
          if (std::fabs(x) > opt.jump_sigmas * sd) ++n_jump;
          else { kept += x * x; ++n_kept; }
        const double next = n_kept ? std::sqrt(kept / n_kept) : sd;
        const bool done = n_jump == jn && std::fabs(next - sd) <= 1e-12 * sd;
        sd = next; jn = n_jump;
        if (done) break;
      }
      c.vol = sd / std::sqrt(opt.sample_s);
      double js = 0; jn = 0;
      for (double x : r) if (std::fabs(x) > opt.jump_sigmas * sd) { js += x * x; ++jn; }
      c.jump_rate = jn / (r.size() * opt.sample_s);
      c.jump_sd = jn ? std::sqrt(js / jn) : 0.0;
    }
    c.trade_rate = c.duration_s > 0 ? 0.5 * c.trades / c.duration_s : 0.0;
    // --- fill intensity
    if (s.tt.size() >= 20) {
      const int L = opt.levels;
      const double dstep = std::max(s.min_inc > 0 ? s.min_inc : 0.0, 0.25 * c.mean_spread);
      std::vector<double> hits(L, 0.0), expo(L, 0.0);
      const TimeNs H = ns(opt.horizon_s);
      size_t ti = 0, bi = 0;
      for (TimeNs t = s.bt.front(); t + H <= s.bt.back(); t += step) {
        while (bi + 1 < s.bt.size() && s.bt[bi + 1] <= t) ++bi;
        while (ti < s.tt.size() && s.tt[ti] < t) ++ti;
        const double m0 = s.mid[bi];
        for (double side : {-1.0, 1.0}) {  // -1: our bid, hit by sell aggressors
          int reached = -1;                // deepest level filled so far
          for (size_t q = ti; q < s.tt.size() && s.tt[q] < t + H && reached < L - 1; ++q) {
            if (s.ts[q] != side) continue;
            const double depth = side < 0 ? m0 - s.tp[q] : s.tp[q] - m0;
            if (depth < 0) continue;
            const int lv = std::min(L - 1, static_cast<int>(std::floor(depth / dstep + 1e-9)));
            for (int l = reached + 1; l <= lv; ++l) { hits[l] += 1.0; expo[l] += secs(s.tt[q] - t); }
            reached = std::max(reached, lv);
          }
          for (int l = reached + 1; l < L; ++l) expo[l] += opt.horizon_s;
        }
      }
      double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
      for (int l = 0; l < L; ++l) {
        if (hits[l] < 5 || expo[l] <= 0) continue;
        const double x = l * dstep, y = std::log(hits[l] / expo[l]), w = hits[l];
        sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
        ++c.intensity_points;
      }
      const double den = sw * sxx - sx * sx;
      if (c.intensity_points >= 3 && den > 0) {
        const double slope = (sw * sxy - sx * sy) / den;
        if (slope < 0) { c.k = -slope; c.A = std::exp((sy - slope * sx) / sw); }
      }
    }
    out.symbols.push_back(c);
  }
  const size_t n = out.symbols.size();
  out.corr.assign(n * n, 0.0);
  for (size_t a = 0; a < n; ++a)
    for (size_t b = 0; b < n; ++b) {
      double sab = 0, saa = 0, sbb = 0;
      const size_t m = std::min(grids[a].size(), grids[b].size());
      for (size_t i = 0; i < m; ++i) { sab += grids[a][i] * grids[b][i]; saa += grids[a][i] * grids[a][i]; sbb += grids[b][i] * grids[b][i]; }
      out.corr[a * n + b] = saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : (a == b ? 1.0 : 0.0);
    }
  return out;
}

}  // namespace amm::live
