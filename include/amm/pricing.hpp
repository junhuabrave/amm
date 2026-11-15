// Pricers map factor levels to a model price and factor sensitivities.
// Adding an asset class means adding one Pricer; nothing else changes.
#pragma once
#include <algorithm>
#include <cmath>

#include "amm/core.hpp"

namespace amm {

struct PriceOut {
  double px = kNaN;
  Exposures ex;
};

class Pricer {
 public:
  virtual ~Pricer() = default;
  // levels: factor levels indexed by FactorId. t: years elapsed since start.
  virtual PriceOut price(const double* levels, double t) const = 0;
};

// px = a + b * (level - l0). Spot equity/crypto (a=0,b=1,l0=0), perps,
// futures and index products quoted linearly in their driver.
class LinearPricer final : public Pricer {
 public:
  LinearPricer(FactorId f, double a = 0.0, double b = 1.0, double l0 = 0.0)
      : f_(f), a_(a), b_(b), l0_(l0) {}
  PriceOut price(const double* lv, double) const override {
    PriceOut o;
    o.px = a_ + b_ * (lv[f_] - l0_);
    o.ex.add(f_, b_);
    return o;
  }

 private:
  FactorId f_;
  double a_, b_, l0_;
};

inline double norm_cdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }
inline double norm_pdf(double x) { return 0.3989422804014327 * std::exp(-0.5 * x * x); }

// European vanilla under Black-Scholes with zero carry. Vol is the ATM vol
// factor plus a linear smile in log-moneyness. `ratio` turns the same pricer
// into a covered warrant (N warrants per share).
class OptionPricer final : public Pricer {
 public:
  OptionPricer(FactorId spot, FactorId vol, double strike, double expiry_y,
               bool call, double ratio = 1.0, double smile_slope = 0.0)
      : fs_(spot), fv_(vol), k_(strike), t_(expiry_y), call_(call),
        ratio_(ratio), slope_(smile_slope) {}
  PriceOut price(const double* lv, double t) const override {
    const double s = std::max(lv[fs_], 1e-9);
    const double tau = std::max(t_ - t, 1e-6);
    const double v = std::max(0.01, lv[fv_] + slope_ * std::log(k_ / s));
    const double sq = v * std::sqrt(tau);
    const double d1 = (std::log(s / k_) + 0.5 * v * v * tau) / sq;
    const double d2 = d1 - sq;
    const double c = s * norm_cdf(d1) - k_ * norm_cdf(d2);
    PriceOut o;
    o.px = ratio_ * (call_ ? c : c - s + k_);
    o.ex.add(fs_, ratio_ * (call_ ? norm_cdf(d1) : norm_cdf(d1) - 1.0));
    o.ex.add(fv_, ratio_ * s * norm_pdf(d1) * std::sqrt(tau));
    return o;
  }

 private:
  FactorId fs_, fv_;
  double k_, t_;
  bool call_;
  double ratio_, slope_;
};

// Fixed-coupon bullet bond, clean price per 100 face, semi-annual coupons.
// Yield (bp) = rate factor + beta * credit factor + z-spread.
class BondPricer final : public Pricer {
 public:
  BondPricer(FactorId rate, FactorId credit, double coupon_pct, double maturity_y,
             double beta = 1.0, double z_bp = 0.0)
      : fr_(rate), fc_(credit), c_(coupon_pct), t_(maturity_y), beta_(beta), z_(z_bp) {}
  PriceOut price(const double* lv, double t) const override {
    const double y = (lv[fr_] + beta_ * lv[fc_] + z_) * 1e-4;
    const double tau = std::max(t_ - t, 1e-6);
    const int n = std::max(1, static_cast<int>(std::ceil(2.0 * tau - 1e-9)));
    const double disc = 1.0 / (1.0 + 0.5 * y);
    double df = 1.0, p = 0.0, dp = 0.0;
    for (int i = 1; i <= n; ++i) {
      df *= disc;
      const double cf = 0.5 * c_ + (i == n ? 100.0 : 0.0);
      p += cf * df;
      dp -= 0.5 * i * cf * df * disc;  // d/dy
    }
    PriceOut o;
    o.px = p;
    o.ex.add(fr_, dp * 1e-4);
    o.ex.add(fc_, beta_ * dp * 1e-4);
    return o;
  }

 private:
  FactorId fr_, fc_;
  double c_, t_, beta_, z_;
};

}  // namespace amm
