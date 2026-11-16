#!/bin/sh
# Run the engine against Binance USD-M futures through the order gateway, with
# the drop copy and the reconciler alongside.
#
#   scripts/live_gateway.sh dry      live market data, orders acknowledged locally (no keys, nothing sent)
#   scripts/live_gateway.sh testnet  orders to testnet.binancefuture.com (needs BINANCE_API_KEY/SECRET)
#
# Optional: CONFIG (default config/binance_perp.ini), SYMBOLS (default "BTCUSDT ETHUSDT"),
# DURATION seconds for the market-data feed (default: run until Ctrl-C), OUT directory (default data/live).
# Market data: dry mode uses the mainnet public streams; testnet mode uses the
# testnet's own public streams, so quotes are priced off the book they rest in
# (the testnet market is thin: expect wide spreads and few fills). Mainnet
# trading is deliberately not offered by this script.
set -eu
MODE=${1:-dry}
CONFIG=${CONFIG:-config/binance_perp.ini}
SYMBOLS=${SYMBOLS:-"BTCUSDT ETHUSDT"}
OUT=${OUT:-data/live}
cd "$(dirname "$0")/.."
mkdir -p "$OUT"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
ORDERS="$OUT/orders.fifo"
STATE="$OUT/state_$STAMP.jsonl"
JOURNAL="$OUT/dropcopy_$STAMP.jsonl"
EVENTS="$OUT/events_$STAMP.csv"
KILL="$OUT/KILL"
rm -f "$ORDERS" "$KILL"
mkfifo "$ORDERS"
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -f "$ORDERS"; }
trap cleanup EXIT INT TERM

case "$MODE" in
  dry)
    GW_ARGS="--dry-run --journal $JOURNAL"
    REC_ARGS="--no-rest"
    MD_ARGS=""
    ;;
  testnet)
    : "${BINANCE_API_KEY:?set BINANCE_API_KEY (testnet)}"
    : "${BINANCE_API_SECRET:?set BINANCE_API_SECRET (testnet)}"
    GW_ARGS=""
    REC_ARGS=""
    MD_ARGS="--perp-book-url wss://fstream.binancefuture.com/stream --perp-trade-url wss://fstream.binancefuture.com/stream"
    python3 feeds/binance_dropcopy.py --journal "$JOURNAL" --perp $SYMBOLS 2>>"$OUT/dropcopy.log" &
    PIDS="$PIDS $!"
    ;;
  *) echo "usage: $0 dry|testnet" >&2; exit 2 ;;
esac

python3 feeds/reconcile.py --config "$CONFIG" --state "$STATE" --journal "$JOURNAL" --kill-file "$KILL" \
  $REC_ARGS 2>>"$OUT/reconcile.log" &
PIDS="$PIDS $!"

FEED_ARGS=""
[ -n "${DURATION:-}" ] && FEED_ARGS="--duration $DURATION"
echo "mode $MODE; logs, state and journals in $OUT; touch $KILL to stop trading" >&2
python3 feeds/binance_ws.py --perp $SYMBOLS --stats 60 $MD_ARGS $FEED_ARGS 2>>"$OUT/feed.log" \
  | python3 feeds/binance_gateway.py --orders "$ORDERS" --perp $SYMBOLS $GW_ARGS 2>>"$OUT/gateway.log" \
  | ./build/amm_run --config "$CONFIG" --orders "$ORDERS" --kill-file "$KILL" --state "$STATE" \
      --record "$EVENTS" --status 30
