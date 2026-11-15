// Microstructure signals: self-calibrating micro-price, fill-intensity
// curve estimation, post-trade markout (toxicity) tracking, RFQ win curve.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

#include "amm/core.hpp"

namespace amm {

// Fair-value estimate from top of book:
//   fair = mid + bx * (imbalance * half_spread) + bz * (normalised OFI * half_spread)
// bx, bz are learned online by ridge regression of the realised mid change
// over `horizon` on the two features, so the signal weights itself to zero
// on markets where book pressure carries no information.
class MicroPrice {
 public:
  double update(const TopOfBook& b) {
    const double mid = b.mid(), hs = 0.5 * b.spread();
    const double tot = b.bid_sz + b.ask_sz;
    const double imb = tot > 0 ? (b.bid_sz - b.ask_sz) / tot : 0.0;
    if (have_) {
      double e = 0.0;  // order-flow imbalance (Cont, Kukanov, Stoikov)
      e += (b.bid >= prev_.bid ? b.bid_sz : 0.0) - (b.bid <= prev_.bid ? prev_.bid_sz : 0.0);
      e -= (b.ask <= prev_.ask ? b.ask_sz : 0.0) - (b.ask >= prev_.ask ? prev_.ask_sz : 0.0);
      const double dt = std::max(0.0, secs(b.ts - prev_.ts));
      ofi_ = ofi_ * std::exp(-dt / ofi_tau_s) + e;
      depth_ = 0.98 * depth_ + 0.02 * 0.5 * tot;
    } else {
      depth_ = 0.5 * tot;
    }
    const double x = imb * hs;
    const double z = depth_ > 0 ? std::clamp(ofi_ / depth_, -3.0, 3.0) * hs : 0.0;
    learn(b.ts, mid, x, z);
    prev_ = b;
    have_ = true;
    fair_ = mid + bx_ * x + bz_ * z;
    return fair_;
  }
  double fair() const { return fair_; }
  double bx() const { return bx_; }
  double bz() const { return bz_; }
  bool ready() const { return have_; }

  double horizon_s = 0.5;
  double ofi_tau_s = 0.5;

 private:
  struct Sample { TimeNs ts; double mid, x, z; };
  void learn(TimeNs ts, double mid, double x, double z) {
    while (cnt_ > 0 && secs(ts - buf_[head_].ts) >= horizon_s) {
      const Sample& s = buf_[head_];
      const double y = mid - s.mid, lam = 0.999;
      sxx_ = lam * sxx_ + s.x * s.x; sxz_ = lam * sxz_ + s.x * s.z;
      szz_ = lam * szz_ + s.z * s.z; sxy_ = lam * sxy_ + s.x * y;
      szy_ = lam * szy_ + s.z * y;   syy_ = lam * syy_ + y * y;
      n_ = lam * n_ + 1.0;
      head_ = (head_ + 1) % buf_.size();
      --cnt_;
      if (n_ > 50) {
        // Per-feature ridge: shrinks each weight towards zero until the data
        // says otherwise.
        const double a = 1.1 * sxx_ + 1e-18, d = 1.1 * szz_ + 1e-18, det = a * d - sxz_ * sxz_;
        if (det > 0) {
          bx_ = std::clamp((d * sxy_ - sxz_ * szy_) / det, -0.5, 1.5);
          bz_ = std::clamp((a * szy_ - sxz_ * sxy_) / det, -1.0, 1.0);
        }
      }
    }
    if (cnt_ < buf_.size() && (cnt_ == 0 || secs(ts - last_push_) >= horizon_s / 16)) {
      buf_[(head_ + cnt_) % buf_.size()] = Sample{ts, mid, x, z};
      ++cnt_;
      last_push_ = ts;
    }
  }
  std::array<Sample, 64> buf_{};
  size_t head_ = 0, cnt_ = 0;
  TimeNs last_push_ = 0;
  TopOfBook prev_{};
  bool have_ = false;
  double ofi_ = 0, depth_ = 0, fair_ = kNaN;
  double bx_ = 0, bz_ = 0, sxx_ = 0, sxz_ = 0, szz_ = 0, sxy_ = 0, szy_ = 0, syy_ = 0, n_ = 0;
};

// Online estimate of the fill-intensity curve Lambda(d) = A * exp(-k d), where
// d is the distance of our quote from fair value. Uses our own fills: each
// distance bucket accumulates time-at-risk and fill counts (exponentially
// forgotten), a prior keeps empty buckets sane, and a weighted log-linear fit
// gives A and k.
class IntensityEstimator {
 public:
  void init(double A0, double k0) {
    A0_ = A_ = A0; k0_ = k_ = k0;
    width_ = 0.5 / k0;
  }
  void accrue(double d, double dt) {
    expo_[bucket(d)] += dt;
  }
  // Fills through fair value are adverse selection, not liquidity demand: the
  // toxicity tracker accounts for those, so they are excluded here.
  void fill(double d) { if (d > 0) cnt_[bucket(d)] += 1.0; }
  // elapsed_s: wall time since the previous fit, for exponential forgetting.
  void fit(double elapsed_s) {
    const double decay = std::exp(-std::max(0.0, elapsed_s) / tau_s);
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    int used = 0;
    for (size_t b = 0; b < kB; ++b) {
      expo_[b] *= decay; cnt_[b] *= decay;
      if (expo_[b] < 2.0) continue;
      const double x = (b + 0.5) * width_;
      const double rate = (cnt_[b] + prior_s * A0_ * std::exp(-k0_ * x)) / (expo_[b] + prior_s);
      const double y = std::log(std::max(rate, 1e-9)), w = expo_[b];
      sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
      ++used;
    }
    if (used == 0) return;
    // Slope is only weakly identified when our quotes sit in a narrow range of
    // distances, so it is ridge-shrunk to the prior; the level is then pinned
    // so the fitted curve passes through the observed rate at the mean distance.
    const double xb = sx / sw, yb = sy / sw;
    const double vxx = sxx - sw * xb * xb, vxy = sxy - sw * xb * yb;
    const double lam = slope_ridge * sw * width_ * width_;
    double k = (-vxy + lam * k0_) / (vxx + lam);
    double A = std::exp(yb + k * xb);
    k = std::clamp(k, 0.33 * k0_, 3.0 * k0_);
    A = std::clamp(A, 0.2 * A0_, 5.0 * A0_);
    k_ += 0.2 * (k - k_);
    A_ += 0.2 * (A - A_);
  }
  double A() const { return A_; }
  double k() const { return k_; }

  double tau_s = 300.0;   // forgetting time
  double prior_s = 20.0;  // prior weight, seconds of pseudo-exposure
  double slope_ridge = 4.0;

 private:
  static constexpr size_t kB = 12;
  size_t bucket(double d) const {
    if (d <= 0) return 0;
    return std::min(kB - 1, static_cast<size_t>(d / width_));
  }
  std::array<double, kB> expo_{}, cnt_{};
  double A0_ = 1, k0_ = 1, A_ = 1, k_ = 1, width_ = 1;
};

// Adverse-selection meter: average move of fair value against us over a fixed
// horizon after each fill, in price units. Fed back into the half-spread.
class ToxicityTracker {
 public:
  void record(TimeNs ts, Side side, double fair) {
    if (cnt_ == buf_.size()) { head_ = (head_ + 1) % buf_.size(); --cnt_; }
    buf_[(head_ + cnt_) % buf_.size()] = Pending{ts, sgn(side), fair};
    ++cnt_;
  }
  void settle(TimeNs now, double fair) {
    while (cnt_ > 0 && secs(now - buf_[head_].ts) >= horizon_s) {
      const Pending& p = buf_[head_];
      const double adverse = -p.s * (fair - p.fair);
      ewma_ += alpha * (adverse - ewma_);
      head_ = (head_ + 1) % buf_.size();
      --cnt_;
    }
  }
  double adverse() const { return std::max(0.0, ewma_); }
  double raw() const { return ewma_; }

  double horizon_s = 0.25;
  double alpha = 0.01;

 private:
  struct Pending { TimeNs ts; double s, fair; };
  std::array<Pending, 128> buf_{};
  size_t head_ = 0, cnt_ = 0;
  double ewma_ = 0.0;
};

// RFQ hit-ratio curve P(win | margin d) = 1 / (1 + exp(a + b d)), learned by
// online logistic regression on our own won/lost outcomes.
class WinCurve {
 public:
  void init(double a0, double b0) { a_ = a0; b_ = b0_ = b0; }
  double p(double d) const {
    return 1.0 / (1.0 + std::exp(std::clamp(a_ + b_ * d, -40.0, 40.0)));
  }
  void learn(double d, bool won) {
    const double g = (won ? 1.0 : 0.0) - p(d);  // dLoss/d(a + b d)
    a_ -= lr * g;
    b_ -= lr * g * d * b0_ * b0_;
    b_ = std::clamp(b_, 0.25 * b0_, 4.0 * b0_);
    a_ = std::clamp(a_, -8.0, 8.0);
  }
  double a() const { return a_; }
  double b() const { return b_; }
  double lr = 0.03;

 private:
  double a_ = -2, b_ = 50, b0_ = 50;
};

}  // namespace amm
