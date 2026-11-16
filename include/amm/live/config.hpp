// Universe and engine configuration from an INI-style file, so instruments,
// factors and venues are added without recompiling.
//
//   [engine]                      gamma, hedge_band, risk_limit, max_loss, ...
//   [paper]                       latency_ms
//   [factor NAME]                 level, vol (per sqrt second)
//   [corr]                        A B = rho
//   [instrument SYMBOL]           feed, class, protocol, tick, lot, mult, quote_size, max_pos,
//                                 max_spread, fee, maker_fee, taker_fee, max_order_qty, quote, lead, lead_beta,
//                                 lead_tau_ms, lead_gain, idio_vol, A0, k0, rfq_a0, rfq_b0,
//                                 ref_for, ref_gain, hedges, pricer
//   pricer = linear FACTOR [a] [b] [l0]
//          | option SPOT VOL strike expiry_years call|put [ratio] [smile_slope]
//          | bond RATE CREDIT coupon_pct maturity_years [beta] [z_bp]
#pragma once
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "amm/engine.hpp"
#include "amm/live/risk_gate.hpp"

namespace amm::live {

struct FactorCfg { std::string name; double level = 0.0, vol = 0.0; };
struct CorrCfg { std::string a, b; double rho = 0.0; };
struct InstrumentCfg {
  Instrument in;
  std::string feed;                      // symbol as the venue adapter emits it
  std::string ref_for, hedges, lead;     // factor names
  std::vector<std::string> pricer;       // tokens of the pricer spec
  double max_order_qty = 0.0;            // pre-trade cap per order; 0 = max_pos
  double maker_fee = 0.0, taker_fee = 0.0;  // fraction of notional (0.0002 = 2 bp); negative = rebate
};
struct LiveConfig {
  EngineConfig engine;
  double paper_latency_s = 0.005;
  bool paper_post_only = true;  // quotes that would take liquidity on arrival are rejected, as live
  RiskLimits risk;
  bool fees_in_quotes = true;  // widen each quote by the maker fee (in price terms, at the factor level)
  std::vector<FactorCfg> factors;
  std::vector<CorrCfg> corr;
  std::vector<InstrumentCfg> instruments;
};

namespace detail {
inline std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline std::vector<std::string> split_ws(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  for (std::string t; is >> t;) out.push_back(t);
  return out;
}
inline double to_num(const std::string& s, const std::string& where) {
  char* e = nullptr;
  const double v = std::strtod(s.c_str(), &e);
  if (e == s.c_str() || *e != '\0') throw std::runtime_error(where + ": not a number: '" + s + "'");
  return v;
}
inline bool to_bool(const std::string& s, const std::string& where) {
  if (s == "true" || s == "1" || s == "yes") return true;
  if (s == "false" || s == "0" || s == "no") return false;
  throw std::runtime_error(where + ": expected true/false, got '" + s + "'");
}
}  // namespace detail

inline LiveConfig parse_config(std::istream& is, const std::string& name = "config") {
  using namespace detail;
  LiveConfig cfg;
  std::string line, kind, arg;
  int ln = 0;
  auto where = [&] { return name + ":" + std::to_string(ln); };
  while (std::getline(is, line)) {
    ++ln;
    const size_t hash = line.find_first_of(";#");
    if (hash != std::string::npos) line.erase(hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') throw std::runtime_error(where() + ": unterminated section header");
      const std::string h = trim(line.substr(1, line.size() - 2));
      const size_t sp = h.find_first_of(" \t");
      kind = h.substr(0, sp);
      arg = sp == std::string::npos ? "" : trim(h.substr(sp));
      if (kind == "factor") {
        if (arg.empty()) throw std::runtime_error(where() + ": [factor] needs a name");
        cfg.factors.push_back(FactorCfg{arg, 0.0, 0.0});
      } else if (kind == "instrument") {
        if (arg.empty()) throw std::runtime_error(where() + ": [instrument] needs a symbol");
        InstrumentCfg ic;
        ic.in.symbol = arg;
        ic.feed = arg;
        cfg.instruments.push_back(ic);
      } else if (kind != "engine" && kind != "paper" && kind != "corr" && kind != "risk") {
        throw std::runtime_error(where() + ": unknown section [" + kind + "]");
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) throw std::runtime_error(where() + ": expected key = value");
    const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
    auto num = [&] { return to_num(val, where()); };
    if (kind == "engine") {
      EngineConfig& e = cfg.engine;
      if (key == "gamma") e.gamma = num();
      else if (key == "glft") e.glft = to_bool(val, where());
      else if (key == "cross_asset_fair") e.cross_asset_fair = to_bool(val, where());
      else if (key == "portfolio_skew") e.portfolio_skew = to_bool(val, where());
      else if (key == "adapt_tox") e.adapt_tox = to_bool(val, where());
      else if (key == "adapt_intensity") e.adapt_intensity = to_bool(val, where());
      else if (key == "basis_tau_s") e.basis_tau_s = num();
      else if (key == "skew_cap") e.skew_cap = num();
      else if (key == "hedge_band") e.hedge_band = num();
      else if (key == "hedge_target_frac") e.hedge_target_frac = num();
      else if (key == "hedge_cooldown_s") e.hedge_cooldown_s = num();
      else if (key == "hedge_slip_ticks") e.hedge_slip_ticks = static_cast<int>(num());
      else if (key == "risk_limit") e.risk_limit = num();
      else if (key == "max_loss") e.max_loss = num();
      else if (key == "requote_ticks") e.requote_ticks = static_cast<int>(num());
      else if (key == "min_quote_life_ms") e.min_quote_life_s = num() * 1e-3;
      else if (key == "wait_cancel_ack") e.wait_cancel_ack = to_bool(val, where());
      else if (key == "fees_in_quotes") cfg.fees_in_quotes = to_bool(val, where());
      else if (key == "secs_per_year") e.secs_per_year = num();
      else throw std::runtime_error(where() + ": unknown [engine] key '" + key + "'");
    } else if (kind == "paper") {
      if (key == "latency_ms") cfg.paper_latency_s = num() * 1e-3;
      else if (key == "post_only") cfg.paper_post_only = to_bool(val, where());
      else throw std::runtime_error(where() + ": unknown [paper] key '" + key + "'");
    } else if (kind == "risk") {
      RiskLimits& r = cfg.risk;
      if (key == "collar_bps") r.collar_bps = num();
      else if (key == "max_order_notional") r.max_order_notional = num();
      else if (key == "max_orders_per_s") r.max_orders_per_s = num();
      else if (key == "burst") r.burst = num();
      else if (key == "max_open") r.max_open = static_cast<int>(num());
      else throw std::runtime_error(where() + ": unknown [risk] key '" + key + "'");
    } else if (kind == "factor") {
      FactorCfg& f = cfg.factors.back();
      if (key == "level") f.level = num();
      else if (key == "vol") f.vol = num();
      else throw std::runtime_error(where() + ": unknown [factor] key '" + key + "'");
    } else if (kind == "corr") {
      const auto names = split_ws(key);
      if (names.size() != 2) throw std::runtime_error(where() + ": expected 'A B = rho'");
      cfg.corr.push_back(CorrCfg{names[0], names[1], num()});
    } else if (kind == "instrument") {
      InstrumentCfg& ic = cfg.instruments.back();
      Instrument& in = ic.in;
      if (key == "feed") ic.feed = val;
      else if (key == "class") {
        static const std::map<std::string, AssetClass> m = {
            {"equity", AssetClass::Equity}, {"option", AssetClass::Option}, {"warrant", AssetClass::Warrant},
            {"crypto_spot", AssetClass::CryptoSpot}, {"crypto_perp", AssetClass::CryptoPerp},
            {"future", AssetClass::Future}, {"credit_index", AssetClass::CreditIndex}, {"bond", AssetClass::Bond}};
        const auto it = m.find(val);
        if (it == m.end()) throw std::runtime_error(where() + ": unknown class '" + val + "'");
        in.cls = it->second;
      } else if (key == "protocol") {
        if (val == "clob") in.proto = Protocol::Clob;
        else if (val == "rfq") in.proto = Protocol::Rfq;
        else throw std::runtime_error(where() + ": protocol must be clob or rfq");
      }
      else if (key == "tick") in.tick = num();
      else if (key == "lot") in.lot = num();
      else if (key == "mult") in.mult = num();
      else if (key == "quote_size") in.quote_size = num();
      else if (key == "max_pos") in.max_pos = num();
      else if (key == "max_spread") in.max_spread = num();
      else if (key == "fee") in.fee = num();
      else if (key == "idio_vol") in.idio_vol = num();
      else if (key == "A0") in.A0 = num();
      else if (key == "k0") in.k0 = num();
      else if (key == "rfq_a0") in.rfq_a0 = num();
      else if (key == "rfq_b0") in.rfq_b0 = num();
      else if (key == "ref_gain") in.ref_gain = num();
      else if (key == "max_order_qty") ic.max_order_qty = num();
      else if (key == "quote") in.quote = to_bool(val, where());
      else if (key == "lead") ic.lead = val;
      else if (key == "lead_beta") in.lead_beta = num();
      else if (key == "lead_tau_ms") in.lead_tau_s = num() * 1e-3;
      else if (key == "lead_gain") in.lead_gain = num();
      else if (key == "maker_fee") ic.maker_fee = num();
      else if (key == "taker_fee") ic.taker_fee = num();
      else if (key == "ref_for") ic.ref_for = val;
      else if (key == "hedges") ic.hedges = val;
      else if (key == "pricer") ic.pricer = split_ws(val);
      else throw std::runtime_error(where() + ": unknown [instrument] key '" + key + "'");
    } else {
      throw std::runtime_error(where() + ": key outside any section");
    }
  }
  if (cfg.factors.empty()) throw std::runtime_error(name + ": no [factor] sections");
  if (cfg.instruments.empty()) throw std::runtime_error(name + ": no [instrument] sections");
  return cfg;
}

// Returns `text` with `key = value` set in every section matching `section`:
// "engine", "paper", "factor BTC", "instrument ETH-USDT-PERP", or a kind with
// "*" ("instrument *") for all of that kind. An existing key is replaced in
// place, otherwise the line is added after the section header. Throws when no
// section matches, so a typo cannot silently sweep nothing.
inline std::string set_ini_value(const std::string& text, std::string section, const std::string& key,
                                 const std::string& value) {
  using detail::trim;
  for (char& ch : section) if (ch == ':') ch = ' ';
  section = trim(section);
  const size_t sp = section.find(' ');
  const std::string want_kind = section.substr(0, sp), want_arg = sp == std::string::npos ? "" : trim(section.substr(sp));
  std::istringstream is(text);
  std::vector<std::string> lines;
  for (std::string l; std::getline(is, l);) lines.push_back(l);
  const std::string kv = key + " = " + value;
  int matched = 0;
  std::vector<std::string> out;
  bool in_match = false, done = false;
  size_t header_at = 0;
  for (const std::string& raw : lines) {
    std::string l = raw;
    const size_t hash = l.find_first_of(";#");
    if (hash != std::string::npos) l.erase(hash);
    l = trim(l);
    if (!l.empty() && l.front() == '[' && l.back() == ']') {
      if (in_match && !done) out.insert(out.begin() + static_cast<long>(header_at) + 1, kv);
      in_match = false;
      const std::string h = trim(l.substr(1, l.size() - 2));
      const size_t hs = h.find_first_of(" \t");
      const std::string kind = h.substr(0, hs), arg = hs == std::string::npos ? "" : trim(h.substr(hs));
      out.push_back(raw);
      if (kind == want_kind && (want_arg == "*" || arg == want_arg)) {
        in_match = true; done = false; header_at = out.size() - 1; ++matched;
      }
      continue;
    }
    const size_t eq = l.find('=');
    if (in_match && eq != std::string::npos && trim(l.substr(0, eq)) == key) {
      out.push_back(kv); done = true;
      continue;
    }
    out.push_back(raw);
  }
  if (in_match && !done) out.insert(out.begin() + static_cast<long>(header_at) + 1, kv);
  if (!matched) throw std::runtime_error("no [" + section + "] section to set '" + key + "' in");
  std::string r;
  for (const auto& l : out) r += l + '\n';
  return r;
}

inline LiveConfig load_config(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open config file: " + path);
  return parse_config(f, path);
}

// Populate an engine from a parsed configuration. Throws on unknown factor
// names, malformed pricer specs or non-positive sizes, so a bad file fails
// at start-up and never mid-session.
inline void build_engine(const LiveConfig& cfg, Engine& eng) {
  using namespace detail;
  std::map<std::string, FactorId> fid;
  for (const auto& f : cfg.factors) {
    if (fid.count(f.name)) throw std::runtime_error("duplicate factor " + f.name);
    if (!(f.vol > 0)) throw std::runtime_error("factor " + f.name + ": vol must be > 0");
    fid[f.name] = eng.factors().add(f.name, f.level, f.vol);
  }
  auto factor = [&](const std::string& n, const std::string& ctx) {
    const auto it = fid.find(n);
    if (it == fid.end()) throw std::runtime_error(ctx + ": unknown factor '" + n + "'");
    return it->second;
  };
  for (const auto& c : cfg.corr) {
    if (!(c.rho >= -1.0 && c.rho <= 1.0)) throw std::runtime_error("corr " + c.a + " " + c.b + ": outside [-1, 1]");
    eng.factors().set_corr(factor(c.a, "[corr]"), factor(c.b, "[corr]"), c.rho);
  }
  for (const auto& ic : cfg.instruments) {
    Instrument in = ic.in;
    const std::string ctx = "instrument " + in.symbol;
    if (cfg.fees_in_quotes && ic.maker_fee > 0.0 && !ic.ref_for.empty()) {
      // the engine's fee is per unit in price terms; value the rate at the factor level.
      // A rebate (negative fee) is not used to tighten quotes: it would pull them through the touch.
      const double level = eng.factors().level(factor(ic.ref_for, ctx));
      in.fee += ic.maker_fee * level;
    }
    if (!(in.tick > 0 && in.lot > 0 && in.mult > 0))
      throw std::runtime_error(ctx + ": tick, lot and mult must all be > 0");
    if (in.quote && !(in.quote_size > 0 && in.max_pos > 0))
      throw std::runtime_error(ctx + ": quote_size and max_pos must be > 0 (or quote = false for a reference market)");
    if (!(in.A0 > 0 && in.k0 > 0)) throw std::runtime_error(ctx + ": A0 and k0 must be > 0");
    const auto& p = ic.pricer;
    auto opt = [&](size_t i, double dflt) { return p.size() > i ? to_num(p[i], ctx + " pricer") : dflt; };
    std::unique_ptr<Pricer> pr;
    if (p.size() >= 2 && p[0] == "linear") {
      pr = std::make_unique<LinearPricer>(factor(p[1], ctx), opt(2, 0.0), opt(3, 1.0), opt(4, 0.0));
    } else if (p.size() >= 6 && p[0] == "option") {
      if (p[5] != "call" && p[5] != "put") throw std::runtime_error(ctx + ": option must be call or put");
      pr = std::make_unique<OptionPricer>(factor(p[1], ctx), factor(p[2], ctx), to_num(p[3], ctx), to_num(p[4], ctx),
                                          p[5] == "call", opt(6, 1.0), opt(7, 0.0));
    } else if (p.size() >= 5 && p[0] == "bond") {
      pr = std::make_unique<BondPricer>(factor(p[1], ctx), factor(p[2], ctx), to_num(p[3], ctx), to_num(p[4], ctx),
                                        opt(5, 1.0), opt(6, 0.0));
    } else {
      throw std::runtime_error(ctx + ": missing or malformed pricer");
    }
    if (!ic.ref_for.empty()) in.ref_for_factor = factor(ic.ref_for, ctx);
    if (!ic.hedges.empty()) in.hedges_factor = factor(ic.hedges, ctx);
    if (!ic.lead.empty()) {
      in.lead_factor = factor(ic.lead, ctx);
      if (!(in.lead_tau_s > 0)) throw std::runtime_error(ctx + ": lead_tau_ms must be > 0");
    }
    in.pricer = eng.add_pricer(std::move(pr));
    eng.add_instrument(in);
  }
}

}  // namespace amm::live
