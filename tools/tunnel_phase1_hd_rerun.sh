#!/bin/bash
# Tunnel Phase 1 HD Re-run — 8 workers, iperf3 bug fixed.
# Run from the bench-client VM.
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT=10.10.0.2
RELAY_TS=100.64.0.1
SSH_KEY=$HOME/.ssh/id_ed25519_targets
SSH_USER=worker
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

HD_PORT=3341
HD_WORKERS="${HD_WORKERS:-8}"
RUNS=10
DURATION=15

OUT=/tmp/tunnel_phase1
# Back up old HD results.
if [ -d "$OUT/hd" ]; then
  mv "$OUT/hd" "$OUT/hd_4w_backup"
fi
mkdir -p "$OUT"/hd/{tcp1,tcp4,udp,ping,latload,cpu}

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

relay_cmd() {
  timeout 30 ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${RELAY_INT}" "$1" 2>/dev/null || true
}

relay_out() {
  timeout 30 ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${RELAY_INT}" "$1" 2>/dev/null
}

fix_nft() {
  relay_cmd 'sudo nft insert rule inet filter output position 13 oif "tailscale0" accept 2>/dev/null; sudo nft insert rule inet filter output position 13 ip daddr 100.64.0.0/10 accept 2>/dev/null; sudo nft insert rule inet filter input position 8 iif "tailscale0" accept 2>/dev/null; sudo nft insert rule inet filter input position 8 ip saddr 100.64.0.0/10 accept 2>/dev/null'
  sudo nft insert rule inet filter output position 13 oif "tailscale0" accept 2>/dev/null || true
  sudo nft insert rule inet filter output position 13 ip daddr 100.64.0.0/10 accept 2>/dev/null || true
  sudo nft insert rule inet filter input position 8 iif "tailscale0" accept 2>/dev/null || true
  sudo nft insert rule inet filter input position 8 ip saddr 100.64.0.0/10 accept 2>/dev/null || true
}

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

switch_derp() {
  local port=$1
  relay_cmd "sudo tee /etc/headscale/derp-map.yaml > /dev/null << EOF
regions:
  900:
    regionid: 900
    regioncode: test
    regionname: \"Test Relay\"
    nodes:
      - name: test-relay
        regionid: 900
        hostname: \"${RELAY_INT}\"
        ipv4: \"${RELAY_INT}\"
        derpport: ${port}
        insecurefortests: true
EOF"
  relay_cmd "sudo systemctl restart headscale; sleep 2"
  relay_cmd "sudo systemctl restart tailscaled; sleep 2; \
    sudo tailscale up --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-relay \
    --accept-dns=false 2>/dev/null"
  sudo systemctl restart tailscaled
  sleep 2
  sudo tailscale up --login-server "http://${RELAY_INT}:8080" \
    --authkey "${HS_KEY}" --hostname tunnel-client \
    --accept-dns=false 2>/dev/null
  sleep 3
  fix_nft
  sleep 3
  local ok
  ok=$(tailscale ping -c 1 "${RELAY_TS}" 2>&1 | grep -c "pong")
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: DERP ping failed, retrying..."
    sleep 5
    fix_nft
    sleep 3
  fi
}

start_monitor() {
  local label=$1
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x hyper-derp); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${label}_pidstat.txt 2>&1' &"
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${label}_mpstat.txt 2>&1 &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local label=$1 dest=$2
  for suffix in pidstat mpstat; do
    scp -i "$SSH_KEY" \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null \
      -o LogLevel=ERROR \
      "${SSH_USER}@${RELAY_INT}:/tmp/cpu/${label}_${suffix}.txt" \
      "$dest/" 2>/dev/null || true
  done
}

# ---- Test Functions ------------------------------------------

run_tcp1() {
  local outfile=$1
  iperf3 -c "${RELAY_TS}" -t "$DURATION" --json \
    2>/dev/null > "$outfile" || true
}

run_tcp4() {
  local outfile=$1
  iperf3 -c "${RELAY_TS}" -t "$DURATION" -P 4 --json \
    2>/dev/null > "$outfile" || true
}

run_udp() {
  local outfile=$1
  iperf3 -c "${RELAY_TS}" -u -b 0 -t "$DURATION" --json \
    2>/dev/null > "$outfile" || true
}

run_ping() {
  local outfile=$1
  ping -c 1000 -i 0.01 "${RELAY_TS}" \
    2>/dev/null > "$outfile" || true
}

run_latload() {
  local outfile=$1
  # Start background load.
  iperf3 -c "${RELAY_TS}" -t "$DURATION" \
    2>/dev/null > /dev/null &
  local load_pid=$!
  sleep 2
  # Measure latency under load.
  ping -c 500 -i 0.02 "${RELAY_TS}" \
    2>/dev/null > "$outfile" || true
  kill $load_pid 2>/dev/null || true
  wait $load_pid 2>/dev/null || true
  # Restart iperf3 server to avoid stale connections.
  pkill -9 iperf3 2>/dev/null || true
  sleep 1
  relay_cmd "pkill iperf3 2>/dev/null; sleep 1; \
    setsid iperf3 -s -B ${RELAY_TS} -D 2>/dev/null"
  sleep 1
}

extract_result() {
  local file=$1 type=$2
  python3 -c "
import json, sys
try:
  if '${type}' == 'ping':
    lines = open('${file}').readlines()
    for l in lines:
      if 'rtt' in l and 'avg' in l:
        parts = l.split('=')[1].split('/')
        print(f'  avg={parts[1]}ms min={parts[0]}ms max={parts[2]}ms')
        break
  else:
    d = json.load(open('${file}'))
    if '${type}' == 'udp':
      bps = d['end']['sum']['bits_per_second']
      lost = d['end']['sum']['lost_percent']
      print(f'  {bps/1e6:.0f} Mbps, {lost:.1f}% loss')
    else:
      bps = d['end']['sum_received']['bits_per_second']
      retr = d['end']['sum_sent'].get('retransmits', 0)
      print(f'  {bps/1e6:.0f} Mbps, {retr} retrans')
except Exception as e:
  print(f'  FAILED: {e}')
" 2>/dev/null
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  PHASE 1 HD RE-RUN — 8 WORKERS"
log "========================================="

# Restart HD with 8 workers.
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
  --port ${HD_PORT} --workers ${HD_WORKERS} \
  --tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem \
  --debug-endpoints --metrics-port 9100 \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 3
log "  HD restarted with ${HD_WORKERS} workers"

switch_derp "$HD_PORT"
drop_caches
start_monitor "hd_tunnel_8w"

# Verify tunnel works.
tailscale ping -c 1 "${RELAY_TS}" 2>&1 | head -1
relay_cmd "pkill iperf3 2>/dev/null; \
  setsid iperf3 -s -B ${RELAY_TS} -D 2>/dev/null"
sleep 1

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] HD 8w tunnel tests:"

  log "    tcp1..."
  run_tcp1 "$OUT/hd/tcp1/r${padded}.json"
  extract_result "$OUT/hd/tcp1/r${padded}.json" tcp

  log "    tcp4..."
  run_tcp4 "$OUT/hd/tcp4/r${padded}.json"
  extract_result "$OUT/hd/tcp4/r${padded}.json" tcp

  log "    udp..."
  run_udp "$OUT/hd/udp/r${padded}.json"
  extract_result "$OUT/hd/udp/r${padded}.json" udp

  log "    ping..."
  run_ping "$OUT/hd/ping/r${padded}.txt"
  extract_result "$OUT/hd/ping/r${padded}.txt" ping

  log "    latload..."
  run_latload "$OUT/hd/latload/r${padded}.txt"
  extract_result "$OUT/hd/latload/r${padded}.txt" ping

  sleep 2
done

stop_monitor
download_cpu "hd_tunnel_8w" "$OUT/hd/cpu"
relay_cmd "pkill iperf3 2>/dev/null"

log ""
log "========================================="
log "  HD 8W RE-RUN COMPLETE"
log "========================================="

touch "$OUT/HD_8W_RERUN_DONE"
