#!/bin/bash
# Diagnostic CPU profiling run.
# 4w HD kTLS at 5G/10G/15G with mpstat on both machines.
# 3 runs per rate, 15s each.
set -uo pipefail

RELAY=10.50.0.1
RELAY_SSH=hd-test01
RELAY_USER=worker
HD_PORT=3341
SIZE=1400
PEERS=20
PAIRS=10
DURATION=15
WORKERS=4
RATES="5000 10000 15000"
RUNS=3

OUT=/tmp/bench_diag_cpu
mkdir -p "$OUT"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

relay_cmd() {
  timeout 30 ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}" "$1" 2>/dev/null || true
}

relay_out() {
  timeout 30 ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}" "$1" 2>/dev/null
}

wait_port() {
  local host=$1 port=$2 deadline=$((SECONDS + 10))
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    ((SECONDS >= deadline)) && return 1
    sleep 0.5
  done
}

# Generate certs.
log "Generating certs..."
relay_cmd "mkdir -p /tmp/bench_certs"
relay_cmd "openssl req -x509 \
  -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /tmp/bench_certs/key.pem \
  -out /tmp/bench_certs/cert.pem \
  -days 1 -nodes \
  -subj '/CN=derp.tailscale.com' \
  -addext 'subjectAltName=DNS:derp.tailscale.com,IP:${RELAY}' \
  2>/dev/null"
relay_cmd "cp /tmp/bench_certs/cert.pem \
  /tmp/bench_certs/derp.tailscale.com.crt"
relay_cmd "cp /tmp/bench_certs/key.pem \
  /tmp/bench_certs/derp.tailscale.com.key"

# Start HD.
log "Starting HD (${WORKERS}w)..."
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
relay_cmd "sudo modprobe tls"
relay_cmd "sudo setsid hyper-derp --port ${HD_PORT} \
  --workers $WORKERS \
  --tls-cert /tmp/bench_certs/cert.pem \
  --tls-key /tmp/bench_certs/key.pem \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 4
wait_port "$RELAY" "$HD_PORT" || {
  log "ERROR: HD not listening"; exit 1; }
log "HD started"

for rate in $RATES; do
  log "--- ${rate} Mbps (${RUNS} runs) ---"
  for r in $(seq -w 1 "$RUNS"); do
    log "  run ${r}/${RUNS}"

    # Start mpstat on relay.
    relay_cmd "nohup mpstat -P ALL 1 \
      > /tmp/mpstat_relay_${rate}_r${r}.txt 2>&1 &"

    # Start mpstat on client.
    mpstat -P ALL 1 > "${OUT}/mpstat_client_${rate}_r${r}.txt" \
      2>&1 &
    local_mpstat=$!

    sleep 1

    # Run benchmark.
    derp-scale-test \
      --host "$RELAY" --port "$HD_PORT" \
      --peers "$PEERS" --active-pairs "$PAIRS" \
      --duration "$DURATION" --msg-size "$SIZE" \
      --rate-mbps "$rate" \
      --tls --insecure \
      --json 2>/dev/null \
      | sed -n '/{/,/}/p' \
      > "${OUT}/rate_${rate}_r${r}.json"

    sleep 1

    # Stop mpstat.
    relay_cmd "pkill -f mpstat 2>/dev/null"
    kill $local_mpstat 2>/dev/null
    wait $local_mpstat 2>/dev/null || true

    # Download relay mpstat.
    scp -o StrictHostKeyChecking=no -o LogLevel=ERROR \
      "${RELAY_USER}@${RELAY_SSH}:/tmp/mpstat_relay_${rate}_r${r}.txt" \
      "${OUT}/" 2>/dev/null

    # Quick summary.
    python3 -c "
import json
try:
  d = json.load(open('${OUT}/rate_${rate}_r${r}.json'))
  print(f'    {d[\"throughput_mbps\"]:.0f} Mbps, '
        f'loss {d[\"message_loss_pct\"]:.1f}%')
except: print('    (parse error)')
" 2>/dev/null

    sleep 2
  done
done

# Stop HD.
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# Summarize CPU usage.
log ""
log "=== CPU Summary ==="
for rate in $RATES; do
  log "--- ${rate} Mbps ---"
  for r in $(seq -w 1 "$RUNS"); do
    log "  Relay (run ${r}):"
    python3 -c "
import re
lines = open('${OUT}/mpstat_relay_${rate}_r${r}.txt').readlines()
# Find 'all' lines, skip header rows.
cpus = {}
for line in lines:
  parts = line.split()
  if len(parts) < 12:
    continue
  if parts[1] == 'CPU':
    continue
  cpu = parts[1]
  try:
    idle = float(parts[-1])
  except:
    continue
  cpus.setdefault(cpu, []).append(100 - idle)
for cpu in sorted(cpus, key=lambda x: (x != 'all', x)):
  vals = cpus[cpu]
  # Skip first and last (partial).
  if len(vals) > 3:
    vals = vals[1:-1]
  avg = sum(vals)/len(vals)
  if cpu == 'all' or avg > 10:
    print(f'    {cpu:>3s}: {avg:5.1f}% avg util')
" 2>/dev/null
    log "  Client (run ${r}):"
    python3 -c "
import re
lines = open('${OUT}/mpstat_client_${rate}_r${r}.txt').readlines()
cpus = {}
for line in lines:
  parts = line.split()
  if len(parts) < 12:
    continue
  if parts[1] == 'CPU':
    continue
  cpu = parts[1]
  try:
    idle = float(parts[-1])
  except:
    continue
  cpus.setdefault(cpu, []).append(100 - idle)
for cpu in sorted(cpus, key=lambda x: (x != 'all', x)):
  vals = cpus[cpu]
  if len(vals) > 3:
    vals = vals[1:-1]
  avg = sum(vals)/len(vals)
  if cpu == 'all' or avg > 10:
    print(f'    {cpu:>3s}: {avg:5.1f}% avg util')
" 2>/dev/null
  done
done

log ""
log "Done. Results in ${OUT}/"
