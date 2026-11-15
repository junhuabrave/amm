// Quoting mathematics. Pure functions, no state.
#pragma once
#include <algorithm>
#include <cmath>

#include "amm/core.hpp"

namespace amm {

// Guéant-Lehalle-Fernandez-Tapia asymptotic quotes with trade size D
// (in currency-effective units, i.e. units * multiplier):
//   half-spread = base + (D/2) * sigma * C
//   skew        = q * sigma * C                   (single asset)
//   base = ln(1 + gamma D / k) / (gamma D)
//   C    = sqrt( gamma / (2 A D k) * (1 + gamma D / k)^(1 + k / (gamma D)) )
// The multi-asset generalisation used by the engine replaces q * sigma^2 with
// the covariance between the instrument's price and the whole portfolio.
struct Glft {
  double base = 0.0, C = 0.0;
};
inline Glft glft(double gamma, double A, double k, double d_eff) {
  const double xi = gamma * d_eff, r = xi / k;
  Glft g;
  if (r < 1e-9) {
    g.base = 1.0 / k;
    g.C = std::sqrt(gamma / (2.0 * A * d_eff * k) * std::exp(1.0));
  } else {
    g.base = std::log1p(r) / xi;
    g.C = std::sqrt(gamma / (2.0 * A * d_eff * k) * std::exp((1.0 + k / xi) * std::log1p(r)));
  }
  return g;
}

// Margin d maximising expected profit (d - c) * P(win | d) for the logistic
// win curve P = 1 / (1 + exp(a + b d)); c is the cost of taking the trade
// (inventory and adverse selection). First-order condition
//   d - c = (1 + exp(-(a + b d))) / b
// has a unique root; Newton from d = c + 1/b converges monotonically.
inline double rfq_margin(double a, double b, double c) {
  double d = c + 1.0 / b;
  for (int i = 0; i < 40; ++i) {
    const double e = std::exp(std::clamp(-(a + b * d), -50.0, 50.0));
    const double step = (d - c - (1.0 + e) / b) / (1.0 + e);
    d -= step;
    if (std::fabs(step) < 1e-12) break;
  }
  return d;
}

struct Quote {
  bool bid_on = false, ask_on = false;
  double bid = kNaN, ask = kNaN, bid_sz = 0.0, ask_sz = 0.0;
  double half = 0.0, skew = 0.0;
};

}  // namespace amm
