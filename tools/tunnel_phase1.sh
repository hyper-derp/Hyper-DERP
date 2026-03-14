#!/bin/bash
# Tunnel Test Phase 1 — Single Pair Baseline
# Statistical comparison: HD vs TS through real Tailscale tunnel.
# Run from the bench-client VM.
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT=10.10.0.2
RELAY_TS=100.64.0.1
SSH_KEY=$HOME/.ssh/id_ed25519_targets
SSH_USER=worker
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

TS_PORT=3340
HD_PORT=3341
RUNS=10
DURATION=15

OUT=/tmp/tunnel_phase1
mkdir -p "$OUT"/{hd,ts}/{tcp1,tcp4,udp,ping,latload,cpu}

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
  # Ensure tailscale0 traffic allowed in nftables.
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
  # Update DERP map to point to given port.
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

  # Reconnect both tailscale clients.
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

  # Verify DERP connection.
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
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x hyper-derp || pgrep -x derper); \
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
log "  TUNNEL PHASE 1 — SINGLE PAIR BASELINE"
log "========================================="

# ---- TS (Go derper) -----------------------------------------

log ""
log "Phase 1A: Go derper (TS) on port ${TS_PORT}"
relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
relay_cmd "sudo setsid derper -dev \
  -a :${TS_PORT} \
  -certmode manual \
  -certdir /tmp/derper-certs \
  -hostname ${RELAY_INT} \
  </dev/null >/tmp/derper.log 2>&1 &"
sleep 3

switch_derp "$TS_PORT"
drop_caches
start_monitor "ts_tunnel"

# Verify tunnel works.
tailscale ping -c 1 "${RELAY_TS}" 2>&1 | head -1
relay_cmd "pkill iperf3 2>/dev/null; setsid iperf3 -s -B ${RELAY_TS} -D 2>/dev/null"
sleep 1

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] TS tunnel tests:"

  log "    tcp1..."
  run_tcp1 "$OUT/ts/tcp1/r${padded}.json"
  extract_result "$OUT/ts/tcp1/r${padded}.json" tcp

  log "    tcp4..."
  run_tcp4 "$OUT/ts/tcp4/r${padded}.json"
  extract_result "$OUT/ts/tcp4/r${padded}.json" tcp

  log "    udp..."
  run_udp "$OUT/ts/udp/r${padded}.json"
  extract_result "$OUT/ts/udp/r${padded}.json" udp

  log "    ping..."
  run_ping "$OUT/ts/ping/r${padded}.txt"
  extract_result "$OUT/ts/ping/r${padded}.txt" ping

  log "    latload..."
  run_latload "$OUT/ts/latload/r${padded}.txt"
  extract_result "$OUT/ts/latload/r${padded}.txt" ping

  sleep 2
done

stop_monitor
download_cpu "ts_tunnel" "$OUT/ts/cpu"
relay_cmd "pkill iperf3 2>/dev/null"
relay_cmd "sudo pkill -9 derper 2>/dev/null"

# ---- HD (Hyper-DERP) ----------------------------------------

log ""
log "Phase 1B: Hyper-DERP (HD) on port ${HD_PORT}"

# Restart HD with 8 workers (P3 bitmask benefits from more workers).
HD_WORKERS="${HD_WORKERS:-8}"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
  --port ${HD_PORT} --workers ${HD_WORKERS} \
  --tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem \
  --debug-endpoints --metrics-port 9100 \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 3
log "  Restarted HD with ${HD_WORKERS} workers"

switch_derp "$HD_PORT"
drop_caches
start_monitor "hd_tunnel"

# Verify tunnel works.
tailscale ping -c 1 "${RELAY_TS}" 2>&1 | head -1
relay_cmd "pkill iperf3 2>/dev/null; setsid iperf3 -s -B ${RELAY_TS} -D 2>/dev/null"
sleep 1

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] HD tunnel tests:"

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
download_cpu "hd_tunnel" "$OUT/hd/cpu"
relay_cmd "pkill iperf3 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  PHASE 1 COMPLETE"
log "========================================="

python3 -c "
import json, glob, statistics, os

def parse_iperf(files, mode='tcp'):
    tputs, retrans, losses = [], [], []
    for f in sorted(files):
        try:
            d = json.load(open(f))
            if mode == 'udp':
                tputs.append(d['end']['sum']['bits_per_second'] / 1e6)
                losses.append(d['end']['sum']['lost_percent'])
            else:
                tputs.append(d['end']['sum_received']['bits_per_second'] / 1e6)
                retrans.append(d['end']['sum_sent'].get('retransmits', 0))
        except:
            pass
    return tputs, retrans, losses

def parse_ping(files):
    avgs = []
    for f in sorted(files):
        try:
            for line in open(f):
                if 'rtt' in line and 'avg' in line:
                    parts = line.split('=')[1].split('/')
                    avgs.append(float(parts[1]))
                    break
        except:
            pass
    return avgs

def show(label, values, unit='Mbps'):
    if not values:
        print(f'  {label:<20} NO DATA')
        return
    m = statistics.mean(values)
    sd = statistics.stdev(values) if len(values) > 1 else 0
    cv = sd / m * 100 if m > 0 else 0
    print(f'  {label:<20} {m:>8.0f} {min(values):>8.0f} {max(values):>8.0f} {sd:>8.0f} {cv:>5.1f}%')

base = '$OUT'
print()
print(f'{\"\":<20} {\"Mean\":>8} {\"Min\":>8} {\"Max\":>8} {\"StdDev\":>8} {\"CV%\":>6}')
print('=' * 56)

for relay in ['ts', 'hd']:
    print(f'--- {\"Go derper\" if relay==\"ts\" else \"Hyper-DERP\"} ---')
    t1, r1, _ = parse_iperf(glob.glob(f'{base}/{relay}/tcp1/r*.json'))
    show('TCP 1-stream', t1)
    if r1: show('  retransmits', r1, 'count')
    t4, r4, _ = parse_iperf(glob.glob(f'{base}/{relay}/tcp4/r*.json'))
    show('TCP 4-stream', t4)
    u, _, l = parse_iperf(glob.glob(f'{base}/{relay}/udp/r*.json'), 'udp')
    show('UDP throughput', u)
    p = parse_ping(glob.glob(f'{base}/{relay}/ping/r*.txt'))
    if p:
        m = statistics.mean(p)
        print(f'  {\"Ping avg (ms)\":<20} {m:>8.2f} {min(p):>8.2f} {max(p):>8.2f}')
    ll = parse_ping(glob.glob(f'{base}/{relay}/latload/r*.txt'))
    if ll:
        m = statistics.mean(ll)
        print(f'  {\"Lat-under-load (ms)\":<20} {m:>8.2f} {min(ll):>8.2f} {max(ll):>8.2f}')
    print()

# Ratios
t1_ts, _, _ = parse_iperf(glob.glob(f'{base}/ts/tcp1/r*.json'))
t1_hd, _, _ = parse_iperf(glob.glob(f'{base}/hd/tcp1/r*.json'))
if t1_ts and t1_hd:
    ratio = statistics.mean(t1_hd) / statistics.mean(t1_ts)
    print(f'HD/TS TCP 1-stream ratio: {ratio:.2f}x')
t4_ts, _, _ = parse_iperf(glob.glob(f'{base}/ts/tcp4/r*.json'))
t4_hd, _, _ = parse_iperf(glob.glob(f'{base}/hd/tcp4/r*.json'))
if t4_ts and t4_hd:
    ratio = statistics.mean(t4_hd) / statistics.mean(t4_ts)
    print(f'HD/TS TCP 4-stream ratio: {ratio:.2f}x')
" 2>/dev/null || true

touch "$OUT/PHASE1_DONE"
