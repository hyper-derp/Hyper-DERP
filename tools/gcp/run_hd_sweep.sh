#!/bin/bash
# HD Protocol kTLS benchmark sweep on GCP.
#
# Usage: ./tools/gcp/run_hd_sweep.sh [relay_vcpu] [runs_per_point]
#
# Runs the full rate sweep for both DERP and HD at each rate
# point, collecting JSON results from 4 client VMs in parallel.
#
# Each client runs:
#   20 peers, 10 active pairs, 1400B messages, 15s duration
# Total per rate point: 80 peers, 40 pairs across 4 clients.

set -uo pipefail

PROJECT="hyper-derp-bench"
ZONE="europe-west4-a"
RELAY_VCPU="${1:-16}"
RUNS="${2:-20}"
RELAY_KEY="aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888"
RELAY_PORT=3340
METRICS_PORT=9191
DURATION=15
MSG_SIZE=1400
PEERS_PER_CLIENT=20
PAIRS_PER_CLIENT=10
RESULTS_DIR="results/gcp-hd-$(date +%Y%m%d)"

SSH="gcloud compute ssh --project=$PROJECT --zone=$ZONE"
SCP="gcloud compute scp --project=$PROJECT --zone=$ZONE"

# Get relay internal IP.
RELAY_IP=$(gcloud compute instances describe hd-relay \
  --project="$PROJECT" --zone="$ZONE" \
  --format="get(networkInterfaces[0].networkIP)")
echo "Relay IP: $RELAY_IP"

mkdir -p "$RESULTS_DIR"

# Determine worker count and rate points from vCPU count.
case $RELAY_VCPU in
  2)  WORKERS=1
      RATES="500 1000 2000 3000 5000" ;;
  4)  WORKERS=2
      RATES="500 1000 2000 3000 5000 7500" ;;
  8)  WORKERS=4
      RATES="500 1000 2000 3000 5000 7500 10000" ;;
  16) WORKERS=8
      RATES="500 1000 2000 3000 5000 7500 10000 15000 20000" ;;
  *)  echo "Unsupported vCPU count: $RELAY_VCPU"
      exit 1 ;;
esac

echo "Config: ${RELAY_VCPU}vCPU, ${WORKERS}w, ${RUNS} runs"
echo "Rates: $RATES"
echo "Clients: 4 x ${PEERS_PER_CLIENT}p/${PAIRS_PER_CLIENT}pairs"
echo

start_relay() {
  local mode=$1
  $SSH hd-relay --command "
    pkill -9 hyper-derp 2>/dev/null; sleep 1
    if [ '$mode' = 'hd' ]; then
      nohup ./hyper-derp --port $RELAY_PORT --workers $WORKERS \
        --tls-cert /etc/hd-cert.pem --tls-key /etc/hd-key.pem \
        --hd-relay-key $RELAY_KEY --hd-enroll-mode auto \
        --metrics-port $METRICS_PORT \
        > /tmp/relay.log 2>&1 &
    else
      nohup ./hyper-derp --port $RELAY_PORT --workers $WORKERS \
        --tls-cert /etc/hd-cert.pem --tls-key /etc/hd-key.pem \
        --metrics-port $METRICS_PORT \
        > /tmp/relay.log 2>&1 &
    fi
    sleep 3
    pgrep -c hyper-derp
  " 2>/dev/null
}

stop_relay() {
  $SSH hd-relay --command "pkill -9 hyper-derp" 2>/dev/null
  sleep 1
}

# Verify relay is healthy via metrics endpoint.
check_relay() {
  local health
  health=$($SSH hd-relay --command \
    "curl -sf http://localhost:$METRICS_PORT/health" 2>/dev/null)
  echo "$health" | grep -q '"ok"'
}

# Run one test across all 4 clients in parallel.
run_test() {
  local mode=$1
  local rate=$2
  local run=$3
  local outdir="$RESULTS_DIR/${RELAY_VCPU}vcpu_${WORKERS}w"
  local prefix
  mkdir -p "$outdir"

  if [ "$mode" = "hd" ]; then
    prefix="hd"
  else
    prefix="ts"
  fi

  for i in 0 1 2 3; do
    if [ "$mode" = "hd" ]; then
      $SSH "hdclient-${i}" --command "
        ./hd-scale-test --host $RELAY_IP --port $RELAY_PORT \
          --relay-key $RELAY_KEY \
          --metrics-host $RELAY_IP \
          --metrics-port $METRICS_PORT \
          --peers $PEERS_PER_CLIENT \
          --active-pairs $PAIRS_PER_CLIENT \
          --msg-size $MSG_SIZE --duration $DURATION \
          --rate-mbps $rate --tls \
          --json
      " > "$outdir/${prefix}_${rate}_r$(printf '%02d' "$run")_c${i}.json" \
        2>/dev/null &
    else
      $SSH "hdclient-${i}" --command "
        ./derp-scale-test --host $RELAY_IP --port $RELAY_PORT \
          --peers $PEERS_PER_CLIENT \
          --active-pairs $PAIRS_PER_CLIENT \
          --msg-size $MSG_SIZE --duration $DURATION \
          --rate-mbps $rate --tls \
          --json
      " > "$outdir/${prefix}_${rate}_r$(printf '%02d' "$run")_c${i}.json" \
        2>/dev/null &
    fi
  done
  wait
}

# Collect relay-side metrics after each run.
collect_relay_metrics() {
  local mode=$1
  local rate=$2
  local run=$3
  local outdir="$RESULTS_DIR/${RELAY_VCPU}vcpu_${WORKERS}w"
  local prefix
  if [ "$mode" = "hd" ]; then prefix="hd"; else prefix="ts"; fi

  $SSH hd-relay --command \
    "curl -sf http://localhost:$METRICS_PORT/metrics" \
    > "$outdir/${prefix}_${rate}_r$(printf '%02d' "$run")_relay.txt" \
    2>/dev/null || true
}

# Main sweep loop.
TOTAL_POINTS=0
for RATE in $RATES; do
  for _ in $(seq 1 "$RUNS"); do
    TOTAL_POINTS=$((TOTAL_POINTS + 2))
  done
done
echo "Total test points: $TOTAL_POINTS"
echo

DONE=0
for RATE in $RATES; do
  echo "=== ${RELAY_VCPU}vCPU @ ${RATE} Mbps ==="

  for RUN in $(seq 1 "$RUNS"); do
    # DERP (Tailscale compatible, plain kTLS).
    DONE=$((DONE + 1))
    echo "  [$DONE/$TOTAL_POINTS] DERP run $RUN/$RUNS @ ${RATE}M..."
    start_relay "derp"
    if ! check_relay; then
      echo "    WARN: relay health check failed, retrying..."
      stop_relay
      start_relay "derp"
    fi
    run_test "derp" "$RATE" "$RUN"
    collect_relay_metrics "derp" "$RATE" "$RUN"
    stop_relay

    # HD Protocol over kTLS.
    DONE=$((DONE + 1))
    echo "  [$DONE/$TOTAL_POINTS] HD run $RUN/$RUNS @ ${RATE}M..."
    start_relay "hd"
    if ! check_relay; then
      echo "    WARN: relay health check failed, retrying..."
      stop_relay
      start_relay "hd"
    fi
    run_test "hd" "$RATE" "$RUN"
    collect_relay_metrics "hd" "$RATE" "$RUN"
    stop_relay
  done
done

echo
echo "Sweep complete."
echo "Results in $RESULTS_DIR"
find "$RESULTS_DIR" -name "*.json" | wc -l
echo " JSON files"
