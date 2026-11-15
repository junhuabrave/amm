// Risk-factor model: levels, volatilities and correlations of the common
// drivers every instrument is decomposed onto.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "amm/core.hpp"

namespace amm {

class FactorModel {
 public:
  // vol is in factor units per sqrt(second).
  FactorId add(std::string name, double level, double vol) {
    const size_t n = names_.size() + 1;
    std::vector<double> c(n * n, 0.0);
    for (size_t i = 0; i + 1 < n; ++i)
      for (size_t j = 0; j + 1 < n; ++j) c[i * n + j] = corr_[i * (n - 1) + j];
    c[(n - 1) * n + (n - 1)] = 1.0;
    corr_.swap(c);
    names_.push_back(std::move(name));
    level_.push_back(level);
    vol_.push_back(vol);
    vol0_.push_back(vol);
    var_.push_back(vol * vol);
    last_ts_.push_back(-1);
    return static_cast<FactorId>(n - 1);
  }
  void set_corr(FactorId a, FactorId b, double rho) {
    corr_[a * size() + b] = corr_[b * size() + a] = rho;
  }
  size_t size() const { return names_.size(); }
  const std::string& name(FactorId f) const { return names_[f]; }
  double level(FactorId f) const { return level_[f]; }
  const double* levels() const { return level_.data(); }
  double vol(FactorId f) const { return vol_[f]; }
  double corr(FactorId a, FactorId b) const { return corr_[a * size() + b]; }
  double cov(FactorId a, FactorId b) const {
    return corr_[a * size() + b] * vol_[a] * vol_[b];
  }
  void set_level(FactorId f, double v) { level_[f] = v; }

  // New observation of a factor level. Also feeds the online volatility
  // estimate (time-weighted EWMA of realised variance), bounded to
  // [0.5x, 4x] of the configured prior so one bad print cannot blow it up.
  void observe(FactorId f, double v, TimeNs ts) {
    if (adapt_vol && last_ts_[f] >= 0) {
      const double dt = secs(ts - last_ts_[f]);
      if (dt > 1e-7) {
        const double d = v - level_[f];
        const double w = 1.0 - std::exp(-dt / vol_tau_s);
        var_[f] = (1.0 - w) * var_[f] + w * (d * d / dt);
        const double lo = 0.25 * vol0_[f] * vol0_[f], hi = 16.0 * vol0_[f] * vol0_[f];
        var_[f] = std::clamp(var_[f], lo, hi);
        vol_[f] = std::sqrt(var_[f]);
      }
    }
    if (ts > last_ts_[f]) last_ts_[f] = ts;
    level_[f] = v;
  }

  // y = Sigma * x
  void mul(const double* x, double* y) const {
    const size_t n = size();
    for (size_t i = 0; i < n; ++i) {
      double a = 0.0;
      for (size_t j = 0; j < n; ++j) a += corr_[i * n + j] * vol_[j] * x[j];
      y[i] = a * vol_[i];
    }
  }
  // Variance per second of a price with the given exposures.
  double variance(const Exposures& ex) const {
    double v = 0.0;
    for (uint8_t i = 0; i < ex.n; ++i)
      for (uint8_t j = 0; j < ex.n; ++j)
        v += ex.e[i].d * ex.e[j].d * cov(ex.e[i].f, ex.e[j].f);
    return v;
  }

  bool adapt_vol = true;
  double vol_tau_s = 30.0;

 private:
  std::vector<std::string> names_;
  std::vector<double> level_, vol_, vol0_, var_, corr_;
  std::vector<TimeNs> last_ts_;
};

}  // namespace amm
