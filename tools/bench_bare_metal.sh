#!/bin/bash
# Bare metal benchmark: DERP vs HD over 25GbE DAC.
#
# Raptor Lake (local) → ConnectX-4 25GbE → Haswell (relay)
#
# Tests:
#   1. DERP latency (ping/echo)
#   2. HD latency (ping/echo)
#   3. DERP throughput sweep
#   4. HD throughput sweep
#   5. Mixed DERP+HD concurrent

set -uo pipefail

KEY="$HOME/.ssh/id_ed25519_targets"
O="-o StrictHostKeyChecking=no -o BatchMode=yes"
HASWELL=worker@192.168.0.134
RIP=10.50.0.2
RPORT=3341
MPORT=9191
WORKERS=4
DURATION=10
MSG_SIZE=1400
RUNS=5

rcmd() { ssh -i "$KEY" $O "$HASWELL" "$@"; }

RELAY_KEY=$(head -c 32 /dev/urandom | xxd -p -c 64)
OUTDIR="/tmp/bench_bare_metal_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"

echo "Output: $OUTDIR"
echo "Relay key: $RELAY_KEY"
echo "Workers: $WORKERS"
echo "Message size: $MSG_SIZE"
echo

# ---- Start relay ----
rcmd "pkill -9 -f hyper-derp 2>/dev/null; sleep 1"
rcmd "nohup ./hyper-derp --port $RPORT --workers $WORKERS \
  --metrics-port $MPORT --debug-endpoints \
  --hd-relay-key $RELAY_KEY --hd-enroll-mode auto \
  > /tmp/relay.log 2>&1 &"
sleep 3

H=$(rcmd "curl -sf http://localhost:$MPORT/health" 2>&1)
if ! echo "$H" | grep -q '"ok"'; then
  echo "FATAL: relay not running"
  exit 1
fi
echo "Relay running (${WORKERS}w)"
echo

# ---- 1. DERP latency ----
echo "=== 1. DERP Latency (ping/echo, 1000 pings) ==="
for run in $(seq 1 $RUNS); do
  # Start echo peer.
  ./build/tools/bench/derp-test-client \
    --host $RIP --port $RPORT \
    --mode echo --count 1200 --timeout 30000 \
    > /dev/null 2>&1 &
  EPID=$!
  sleep 1

  # Read echo peer's pubkey - use ready-fd or just wait.
  # Simpler: start ping with stdin key exchange.
  ./build/tools/bench/derp-test-client \
    --host $RIP --port $RPORT \
    --mode ping --count 1000 --size $MSG_SIZE \
    --warmup 100 --timeout 15000 \
    --json --output "$OUTDIR/derp_ping_r${run}.json" \
    --label "derp_ping" --workers $WORKERS \
    2>&1 | grep -E "Ping:|RTT"

  kill $EPID 2>/dev/null; wait $EPID 2>/dev/null
done
echo

# ---- 2. HD latency ----
echo "=== 2. HD Latency (ping/echo, 1000 pings) ==="
for run in $(seq 1 $RUNS); do
  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RELAY_KEY \
    --mode echo --count 1200 --timeout 30000 \
    > /dev/null 2>&1 &
  EPID=$!
  sleep 2

  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RELAY_KEY \
    --mode ping --count 1000 --size $MSG_SIZE \
    --warmup 100 --timeout 15000 \
    --json --output "$OUTDIR/hd_ping_r${run}.json" \
    --label "hd_ping" --workers $WORKERS \
    2>&1 | grep -E "Ping:|RTT"

  kill $EPID 2>/dev/null; wait $EPID 2>/dev/null
done
echo

# ---- 3. DERP throughput ----
echo "=== 3. DERP Throughput (send/recv, ${DURATION}s) ==="
for run in $(seq 1 $RUNS); do
  ./build/tools/bench/derp-test-client \
    --host $RIP --port $RPORT \
    --mode sink --count 0 --size $MSG_SIZE \
    --duration $DURATION --timeout 20000 \
    --json --output "$OUTDIR/derp_sink_r${run}.json" \
    --label "derp_throughput" --workers $WORKERS \
    2>&1 &
  SPID=$!
  sleep 1

  ./build/tools/bench/derp-test-client \
    --host $RIP --port $RPORT \
    --mode flood --count 0 --size $MSG_SIZE \
    --duration $DURATION \
    --json --output "$OUTDIR/derp_flood_r${run}.json" \
    --label "derp_throughput" --workers $WORKERS \
    2>&1 | grep -E "Flood:|PPS|Mbps"

  wait $SPID 2>/dev/null
  echo "  recv: $(python3 -c "
import json
d=json.load(open('$OUTDIR/derp_sink_r${run}.json'))
secs=d.get('elapsed_ns',1)/1e9
mb=d.get('bytes_total',0)*8/1e6
print(f'{mb/secs:.0f} Mbps, {d.get(\"packets_completed\",0)} pkts')
" 2>/dev/null || echo 'no data')"
  sleep 2
done
echo

# ---- 4. HD throughput ----
echo "=== 4. HD Throughput (send/recv, ${DURATION}s) ==="
for run in $(seq 1 $RUNS); do
  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RELAY_KEY \
    --mode recv --count 999999 --size $MSG_SIZE \
    --timeout $((DURATION * 1000 + 5000)) \
    2>"$OUTDIR/hd_recv_r${run}.log" &
  RPID=$!
  sleep 2

  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RELAY_KEY \
    --mode send --count 999999 --size $MSG_SIZE \
    --timeout $((DURATION * 1000)) \
    2>"$OUTDIR/hd_send_r${run}.log" &
  SPID=$!

  # Let it run for duration.
  sleep $((DURATION + 2))
  kill $RPID $SPID 2>/dev/null
  wait $RPID $SPID 2>/dev/null

  # Parse results.
  SENT=$(grep "Sent" "$OUTDIR/hd_send_r${run}.log" 2>/dev/null | head -1)
  RECV=$(grep "Received" "$OUTDIR/hd_recv_r${run}.log" 2>/dev/null | head -1)
  echo "  run $run: $SENT / $RECV"
  sleep 2
done
echo

# ---- 5. Metrics summary ----
echo "=== 5. Final metrics ==="
rcmd "curl -sf http://localhost:$MPORT/metrics" \
  | grep -v "^#" \
  | grep -E "recv_bytes|send_bytes|peers|frame_pool"
echo

echo "=== Workers ==="
rcmd "curl -sf http://localhost:$MPORT/debug/workers" \
  | python3 -c "
import sys,json; d=json.load(sys.stdin)
for w in d['workers']:
  print(f'  w{w[\"id\"]}: {w[\"recv_bytes\"]/1e6:.1f}MB recv '\
        f'{w[\"send_bytes\"]/1e6:.1f}MB send '\
        f'{w[\"peers\"]}p')
" 2>&1

# ---- Cleanup ----
echo
echo "=== Cleanup ==="
rcmd "pkill -f hyper-derp" 2>/dev/null
pkill -f test-client 2>/dev/null

echo
echo "Results saved to $OUTDIR"
ls -la "$OUTDIR"/*.json 2>/dev/null | wc -l
echo " JSON files"
