// Core vocabulary types shared by every layer of the engine.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace amm {

using InstrumentId = uint32_t;
using FactorId = uint32_t;
using OrderId = uint64_t;
using TimeNs = int64_t;

constexpr FactorId kNoFactor = ~0u;
constexpr InstrumentId kNoInstrument = ~0u;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr OrderId kHedgeTag = 1ull << 62;  // set on order ids of hedge orders

inline double secs(TimeNs t) { return static_cast<double>(t) * 1e-9; }
inline TimeNs ns(double s) { return static_cast<TimeNs>(std::llround(s * 1e9)); }

enum class Side : uint8_t { Buy = 0, Sell = 1 };
inline double sgn(Side s) { return s == Side::Buy ? 1.0 : -1.0; }
inline Side opp(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class AssetClass : uint8_t {
  Equity, Option, Warrant, CryptoSpot, CryptoPerp, Future, CreditIndex, Bond
};
enum class Protocol : uint8_t { Clob, Rfq };

// Sensitivity of an instrument's price (per unit, before multiplier) to one
// risk factor. Instruments carry at most four of these; that is what makes
// the engine asset agnostic: everything downstream sees only (factor, dP/dF).
struct Exposure {
  FactorId f = kNoFactor;
  double d = 0.0;
};
struct Exposures {
  std::array<Exposure, 4> e{};
  uint8_t n = 0;
  void add(FactorId f, double d) {
    if (n < e.size()) e[n++] = Exposure{f, d};
  }
  double to(FactorId f) const {
    for (uint8_t i = 0; i < n; ++i)
      if (e[i].f == f) return e[i].d;
    return 0.0;
  }
};

struct TopOfBook {
  double bid = kNaN, ask = kNaN;
  double bid_sz = 0.0, ask_sz = 0.0;
  TimeNs ts = 0;
  bool valid() const { return bid == bid && ask == ask && ask > bid; }
  double mid() const { return 0.5 * (bid + ask); }
  double spread() const { return ask - bid; }
};

// Static reference data plus per-instrument quoting parameters.
struct Instrument {
  InstrumentId id = kNoInstrument;
  std::string symbol;
  AssetClass cls = AssetClass::Equity;
  Protocol proto = Protocol::Clob;
  double tick = 0.01;       // minimum price increment
  double lot = 1.0;         // minimum quantity increment (fractional for crypto)
  double mult = 1.0;        // currency value of 1.0 price move on 1 unit
  double quote_size = 1.0;  // units shown per side
  double max_pos = 0.0;     // hard position limit, units
  double max_spread = 0.0;  // quoting obligation (0 = none)
  double fee = 0.0;         // per-unit fee in price terms, added to half-spread
  double idio_vol = 0.0;    // price vol / sqrt(s) not explained by factors
  double A0 = 1.0;          // prior fill intensity at zero distance, 1/s per side
  double k0 = 1.0;          // prior intensity decay, 1/price
  double rfq_a0 = -2.0;     // prior RFQ win curve: P(win|d) = 1/(1+exp(a+b d))
  double rfq_b0 = 50.0;
  FactorId ref_for_factor = kNoFactor;  // this market is the observation of a factor
  double ref_gain = 1.0;                // Newton damping for that observation
  FactorId hedges_factor = kNoFactor;   // this instrument is the hedge for a factor
  int pricer = -1;
};

struct Fill {
  InstrumentId inst = kNoInstrument;
  Side side = Side::Buy;  // our side
  double px = 0.0, qty = 0.0;
  TimeNs ts = 0;
  OrderId oid = 0;
  bool hedge = false;
};

// Outbound order interface. The engine is written against this only, so the
// same engine binary drives the simulator or a real gateway.
class Venue {
 public:
  virtual ~Venue() = default;
  virtual OrderId submit(InstrumentId inst, Side side, double px, double qty,
                         bool ioc, bool hedge) = 0;
  virtual void cancel(InstrumentId inst, OrderId id) = 0;
};

inline double floor_to(double x, double step) {
  return std::floor(x / step + 1e-9) * step;
}
inline double ceil_to(double x, double step) {
  return std::ceil(x / step - 1e-9) * step;
}

}  // namespace amm
