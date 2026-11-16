// Normalised market-data events: the single seam between venue adapters and
// the engine. Adapters (any language, any venue) write these as CSV lines;
// the runner reads them from a pipe for live use or from a file for replay.
//
//   <ts_ns>,B,<symbol>,<bid>,<bid_size>,<ask>,<ask_size>     top of book
//   <ts_ns>,T,<symbol>,<price>,<qty>,<B|S>                   trade, aggressor side
//   <ts_ns>,C,<symbol>,<price>                               composite / indicative price
//
// When orders go to a real venue (live/gateway.hpp), the order gateway writes
// its execution reports into the same stream:
//
//   <ts_ns>,A,<order id>                                     accepted by the venue
//   <ts_ns>,F,<order id>,<price>,<qty>                       filled (partly or fully)
//   <ts_ns>,D,<order id>,<reason>                            done: can no longer fill
//   <ts_ns>,P,<symbol>,<qty>                                 venue's net position (startup, reconnect)
//   <ts_ns>,G,<status>                                       gateway status: ready | down
//
// Lines starting with '#' and blank lines are ignored.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

#include "amm/core.hpp"

namespace amm::live {

struct MdEvent {
  TimeNs ts = 0;
  char type = 0;       // 'B', 'T', 'C'; execution reports 'A', 'F', 'D', 'P', 'G'
  std::string sym;     // symbol; for A, F, D the order id; for G the status
  std::string text;    // D: reason
  double a = 0, b = 0, c = 0, d = 0;  // B: bid,bid_sz,ask,ask_sz  T: px,qty,(+1 buy / -1 sell)  C: px
};

inline bool parse_event(const std::string& line, MdEvent& ev) {
  if (line.empty() || line[0] == '#') return false;
  std::string f[7];
  size_t n = 0, start = 0;
  for (size_t i = 0; i <= line.size() && n < 7; ++i)
    if (i == line.size() || line[i] == ',') {
      f[n++] = line.substr(start, i - start);
      start = i + 1;
    }
  while (n > 0 && !f[n - 1].empty() && (f[n - 1].back() == '\r' || f[n - 1].back() == '\n')) f[n - 1].pop_back();
  if (n < 3 || f[1].size() != 1 || f[2].empty()) return false;
  char* end = nullptr;
  ev.ts = std::strtoll(f[0].c_str(), &end, 10);
  if (end == f[0].c_str()) return false;
  ev.type = f[1][0];
  ev.sym = f[2];
  auto num = [&](size_t i, double& out) {
    char* e = nullptr;
    out = std::strtod(f[i].c_str(), &e);
    return e != f[i].c_str();
  };
  switch (ev.type) {
    case 'B': return n == 7 && num(3, ev.a) && num(4, ev.b) && num(5, ev.c) && num(6, ev.d);
    case 'T':
      if (n != 6 || !num(3, ev.a) || !num(4, ev.b) || f[5].empty()) return false;
      ev.c = (f[5][0] == 'B' || f[5][0] == 'b') ? 1.0 : -1.0;
      return true;
    case 'C': return n == 4 && num(3, ev.a);
    case 'A': case 'G': return n == 3;
    case 'F': return n == 5 && num(3, ev.a) && num(4, ev.b);
    case 'D': ev.text = n >= 4 ? f[3] : ""; return n >= 3;
    case 'P': return n == 4 && num(3, ev.a);
    default: return false;
  }
}

inline std::string format_event(const MdEvent& ev) {
  char buf[256];
  switch (ev.type) {
    case 'B': std::snprintf(buf, sizeof buf, "%lld,B,%s,%.17g,%.17g,%.17g,%.17g", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.a, ev.b, ev.c, ev.d); break;
    case 'T': std::snprintf(buf, sizeof buf, "%lld,T,%s,%.17g,%.17g,%c", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.a, ev.b, ev.c > 0 ? 'B' : 'S'); break;
    case 'A': case 'G': std::snprintf(buf, sizeof buf, "%lld,%c,%s", static_cast<long long>(ev.ts), ev.type, ev.sym.c_str()); break;
    case 'F': std::snprintf(buf, sizeof buf, "%lld,F,%s,%.17g,%.17g", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.a, ev.b); break;
    case 'D': std::snprintf(buf, sizeof buf, "%lld,D,%s,%s", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.text.c_str()); break;
    case 'P': std::snprintf(buf, sizeof buf, "%lld,P,%s,%.17g", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.a); break;
    default:  std::snprintf(buf, sizeof buf, "%lld,C,%s,%.17g", static_cast<long long>(ev.ts), ev.sym.c_str(), ev.a); break;
  }
  return buf;
}

}  // namespace amm::live
