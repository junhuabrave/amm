// Paper-trade the engine against a stream of normalised market-data events.
//
//   live:    python3 feeds/binance.py BTCUSDT ETHUSDT --perp BTCUSDT | amm_run --config config/crypto.ini --record data/session.csv
//   replay:  amm_run --config config/crypto.ini --replay data/session.csv
//   gateway: amm_run --config ... --orders FIFO [--kill-file PATH] [--state FILE]  (see scripts/live_gateway.sh)
//
// Without --orders, orders are matched locally (see live/paper.hpp) and nothing
// is sent to any venue. With --orders, order lines are written to that file
// (normally a named pipe read by a gateway process such as
// feeds/binance_gateway.py) and execution reports are read from the event stream.
#include <cstring>
#include <fstream>
#include <iostream>

#include "amm/live/runner.hpp"

int main(int argc, char** argv) {
  std::string config, replay, record, orders, kill_file, state;
  double status = 10.0;
  bool fills = false;
  for (int i = 1; i < argc; ++i) {
    const auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    if (arg("--config")) config = argv[++i];
    else if (arg("--replay")) replay = argv[++i];
    else if (arg("--record")) record = argv[++i];
    else if (arg("--status")) status = std::atof(argv[++i]);
    else if (arg("--orders")) orders = argv[++i];
    else if (arg("--kill-file")) kill_file = argv[++i];
    else if (arg("--state")) state = argv[++i];
    else if (!std::strcmp(argv[i], "--fills")) fills = true;
    else config.clear(), i = argc;
  }
  if (config.empty()) {
    std::cerr << "usage: amm_run --config FILE [--replay FILE] [--record FILE] [--status SECONDS] [--fills]\n"
                 "               [--orders FILE [--kill-file PATH] [--state FILE]]\n"
                 "       reads normalised events from --replay FILE, or from standard input when omitted;\n"
                 "       --orders sends orders to a gateway process instead of the in-process paper venue\n";
    return 2;
  }
  try {
    const amm::live::LiveConfig cfg = amm::live::load_config(config);
    std::ofstream rec;
    if (!record.empty()) {
      rec.open(record, std::ios::app);
      if (!rec) throw std::runtime_error("cannot open record file: " + record);
    }
    std::ofstream ord, st;
    amm::live::RunnerOptions opt;
    if (!orders.empty()) {
      ord.open(orders, std::ios::app);
      if (!ord) throw std::runtime_error("cannot open orders file: " + orders);
      opt.orders = &ord;
      opt.kill_file = kill_file;
    }
    if (!state.empty()) {
      st.open(state, std::ios::app);
      if (!st) throw std::runtime_error("cannot open state file: " + state);
      opt.state = &st;
    }
    opt.status_every_s = status;
    opt.log = &std::cout;
    opt.log_fills = fills;
    opt.record = record.empty() ? nullptr : &rec;
    amm::live::Runner runner(cfg, opt);
    if (orders.empty())
      std::cout << "paper trading " << cfg.instruments.size() << " instruments on " << cfg.factors.size()
                << " factors; no orders leave this process\n";
    else
      std::cout << "trading " << cfg.instruments.size() << " instruments through the gateway at " << orders
                << "; waiting for it to report ready\n";
    if (replay.empty()) {
      std::ios::sync_with_stdio(false);
      runner.run(std::cin);
    } else {
      std::ifstream in(replay);
      if (!in) throw std::runtime_error("cannot open replay file: " + replay);
      runner.run(in);
    }
    runner.summary(std::cout);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "amm_run: " << e.what() << '\n';
    return 1;
  }
}
