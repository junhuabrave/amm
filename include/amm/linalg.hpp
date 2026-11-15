// Small dense symmetric linear algebra for factor-space computations.
// Matrices are row-major n x n in std::vector<double>.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace amm {

// Lower Cholesky factor of a symmetric positive semi-definite matrix.
// Non-positive pivots are clamped so slightly indefinite inputs still work.
inline std::vector<double> cholesky(const std::vector<double>& a, size_t n) {
  std::vector<double> l(n * n, 0.0);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j <= i; ++j) {
      double s = a[i * n + j];
      for (size_t k = 0; k < j; ++k) s -= l[i * n + k] * l[j * n + k];
      if (i == j) l[i * n + j] = std::sqrt(std::max(s, 1e-300));
      else l[i * n + j] = s / l[j * n + j];
    }
  return l;
}

// Cyclic Jacobi eigen-decomposition of a symmetric matrix: a = V diag(w) V'.
// On return `a` is destroyed, `v` holds eigenvectors in columns.
inline void jacobi_eigen(std::vector<double>& a, size_t n, std::vector<double>& w,
                         std::vector<double>& v) {
  v.assign(n * n, 0.0);
  for (size_t i = 0; i < n; ++i) v[i * n + i] = 1.0;
  for (int sweep = 0; sweep < 60; ++sweep) {
    double off = 0.0, diag = 0.0;
    for (size_t i = 0; i < n; ++i) {
      diag += a[i * n + i] * a[i * n + i];
      for (size_t j = i + 1; j < n; ++j) off += a[i * n + j] * a[i * n + j];
    }
    if (off <= 1e-28 * (diag + 1e-300)) break;
    for (size_t p = 0; p < n; ++p)
      for (size_t q = p + 1; q < n; ++q) {
        const double apq = a[p * n + q];
        if (std::fabs(apq) < 1e-300) continue;
        const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (size_t k = 0; k < n; ++k) {  // rotate columns p, q
          const double akp = a[k * n + p], akq = a[k * n + q];
          a[k * n + p] = c * akp - s * akq;
          a[k * n + q] = s * akp + c * akq;
        }
        for (size_t k = 0; k < n; ++k) {  // rotate rows p, q
          const double apk = a[p * n + k], aqk = a[q * n + k];
          a[p * n + k] = c * apk - s * aqk;
          a[q * n + k] = s * apk + c * aqk;
        }
        for (size_t k = 0; k < n; ++k) {
          const double vkp = v[k * n + p], vkq = v[k * n + q];
          v[k * n + p] = c * vkp - s * vkq;
          v[k * n + q] = s * vkp + c * vkq;
        }
      }
  }
  w.resize(n);
  for (size_t i = 0; i < n; ++i) w[i] = a[i * n + i];
}

// Inverse square root of a symmetric positive semi-definite matrix, with
// eigenvalues floored at rel_floor * largest eigenvalue.
inline std::vector<double> inv_sqrt_sym(std::vector<double> a, size_t n, double rel_floor = 1e-10) {
  std::vector<double> w, v;
  jacobi_eigen(a, n, w, v);
  double wmax = 0.0;
  for (double x : w) wmax = std::max(wmax, x);
  std::vector<double> r(n * n, 0.0);
  if (wmax <= 0.0) return r;
  for (size_t k = 0; k < n; ++k) {
    const double s = 1.0 / std::sqrt(std::max(w[k], rel_floor * wmax));
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < n; ++j) r[i * n + j] += s * v[i * n + k] * v[j * n + k];
  }
  return r;
}

}  // namespace amm
