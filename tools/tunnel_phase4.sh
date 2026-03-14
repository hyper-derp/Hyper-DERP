#!/bin/bash
# Tunnel Test Phase 4 — Chaos / Realistic Scenarios
# Client churn, asymmetric load, long duration.
# Run from the bench-client VM (10.10.0.3).
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT=10.10.0.2
SSH_KEY=$HOME/.ssh/id_ed25519_targets
SSH_USER=worker
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

HD_PORT=3341
TS_PORT=3340
DURATION_LONG=300  # 5 minutes

# Same pair definitions as Phase 3.
P1_SEND_INT=10.10.0.3
P1_RECV_TS=100.64.0.1
P1_RECV_INT=10.10.0.2

P2_SEND_INT=10.10.0.4
P2_RECV_TS=100.64.0.6
P2_RECV_INT=10.10.0.5

P3_SEND_INT=10.10.0.7
P3_RECV_TS=100.64.0.4
P3_RECV_INT=10.10.0.8

OUT=/tmp/tunnel_phase4
mkdir -p "$OUT"/{hd,ts}/{churn,asymmetric,longrun,cpu}

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

remote_cmd() {
  local host=$1 cmd=$2
  timeout 30 ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${host}" "$cmd" 2>/dev/null || true
}

remote_out() {
  local host=$1 cmd=$2
  timeout 30 ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${host}" "$cmd" 2>/dev/null
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

  # Restart all tailscale clients.
  relay_cmd "sudo systemctl restart tailscaled; sleep 2; \
    sudo tailscale up --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-relay \
    --accept-dns=false 2>/dev/null"

  sudo systemctl restart tailscaled
  sleep 2
  sudo tailscale up --login-server "http://${RELAY_INT}:8080" \
    --authkey "${HS_KEY}" --hostname tunnel-client \
    --accept-dns=false 2>/dev/null

  for send_int in "$P2_SEND_INT" "$P3_SEND_INT"; do
    local hn
    case $send_int in
      "$P2_SEND_INT") hn="tunnel-client-2" ;;
      "$P3_SEND_INT") hn="tunnel-client-5" ;;
    esac
    remote_cmd "$send_int" "sudo systemctl restart tailscaled; \
      sleep 2; sudo tailscale up \
      --login-server http://${RELAY_INT}:8080 \
      --authkey ${HS_KEY} --hostname ${hn} \
      --accept-dns=false 2>/dev/null"
  done
  for recv_int in "$P2_RECV_INT" "$P3_RECV_INT"; do
    local hn
    case $recv_int in
      "$P2_RECV_INT") hn="tunnel-client-3" ;;
      "$P3_RECV_INT") hn="tunnel-client-6" ;;
    esac
    remote_cmd "$recv_int" "sudo systemctl restart tailscaled; \
      sleep 2; sudo tailscale up \
      --login-server http://${RELAY_INT}:8080 \
      --authkey ${HS_KEY} --hostname ${hn} \
      --accept-dns=false 2>/dev/null"
  done

  sleep 3
  fix_nft
  sleep 3
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

start_iperf_servers() {
  relay_cmd "pkill iperf3 2>/dev/null; sleep 1; \
    setsid iperf3 -s -B ${P1_RECV_TS} -D 2>/dev/null"
  remote_cmd "$P2_RECV_INT" "pkill iperf3 2>/dev/null; \
    sleep 1; setsid iperf3 -s -D 2>/dev/null"
  remote_cmd "$P3_RECV_INT" "pkill iperf3 2>/dev/null; \
    sleep 1; setsid iperf3 -s -D 2>/dev/null"
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
# Test 1: Client Churn — connect/disconnect during load
# ==============================================================

run_churn_test() {
  local relay=$1 label=$2
  log ""
  log "  Test 1: Client churn (${label})"

  start_iperf_servers

  # Pair 1: sustained bulk transfer (60s).
  iperf3 -c "${P1_RECV_TS}" -t 60 --json 2>/dev/null \
    > "$OUT/${relay}/churn/bulk_during_churn.json" &
  local bulk_pid=$!

  # Pair 3: continuous ping for latency during churn.
  remote_out "$P3_SEND_INT" \
    "ping -c 3000 -i 0.02 ${P3_RECV_TS} 2>/dev/null" \
    > "$OUT/${relay}/churn/ping_during_churn.txt" &
  local ping_pid=$!

  # Pair 2: churn — disconnect/reconnect every 10-15s.
  for cycle in $(seq 1 5); do
    log "    churn cycle $cycle: disconnect pair 2"
    remote_cmd "$P2_SEND_INT" "sudo tailscale down 2>/dev/null"
    sleep $((10 + RANDOM % 6))
    log "    churn cycle $cycle: reconnect pair 2"
    remote_cmd "$P2_SEND_INT" \
      "sudo tailscale up --login-server http://${RELAY_INT}:8080 \
      --authkey ${HS_KEY} --hostname tunnel-client-2 \
      --accept-dns=false 2>/dev/null"
    sleep 5
  done

  wait $bulk_pid 2>/dev/null || true
  wait $ping_pid 2>/dev/null || true

  log "    Bulk throughput during churn:"
  extract_result "$OUT/${relay}/churn/bulk_during_churn.json" tcp
  log "    Latency during churn:"
  extract_result "$OUT/${relay}/churn/ping_during_churn.txt" ping
}

# ==============================================================
# Test 2: Asymmetric Load — 1 pair full blast, others trickle
# ==============================================================

run_asymmetric_test() {
  local relay=$1 label=$2
  log ""
  log "  Test 2: Asymmetric load (${label})"

  start_iperf_servers

  # Pair 1: full blast TCP.
  iperf3 -c "${P1_RECV_TS}" -t 30 --json 2>/dev/null \
    > "$OUT/${relay}/asymmetric/blast.json" &
  local blast_pid=$!

  # Pair 2: trickle (100 Kbps).
  remote_out "$P2_SEND_INT" \
    "iperf3 -c ${P2_RECV_TS} -t 30 -b 100K --json \
    2>/dev/null" \
    > "$OUT/${relay}/asymmetric/trickle.json" &
  local trickle_pid=$!

  # Pair 3: ping (interactive).
  remote_out "$P3_SEND_INT" \
    "ping -c 1500 -i 0.02 ${P3_RECV_TS} 2>/dev/null" \
    > "$OUT/${relay}/asymmetric/ping.txt" &
  local ping_pid=$!

  wait $blast_pid 2>/dev/null || true
  wait $trickle_pid 2>/dev/null || true
  wait $ping_pid 2>/dev/null || true

  log "    Blast throughput:"
  extract_result "$OUT/${relay}/asymmetric/blast.json" tcp
  log "    Trickle throughput:"
  extract_result "$OUT/${relay}/asymmetric/trickle.json" tcp
  log "    Interactive latency:"
  extract_result "$OUT/${relay}/asymmetric/ping.txt" ping
}

# ==============================================================
# Test 3: Long Duration — 5 minutes sustained
# ==============================================================

run_longrun_test() {
  local relay=$1 label=$2
  log ""
  log "  Test 3: Long duration (${DURATION_LONG}s, ${label})"

  start_iperf_servers

  # All 3 pairs: sustained TCP for 5 minutes.
  remote_out "$P2_SEND_INT" \
    "iperf3 -c ${P2_RECV_TS} -t ${DURATION_LONG} --json \
    2>/dev/null" \
    > "$OUT/${relay}/longrun/pair2.json" &
  local p2=$!
  remote_out "$P3_SEND_INT" \
    "iperf3 -c ${P3_RECV_TS} -t ${DURATION_LONG} --json \
    2>/dev/null" \
    > "$OUT/${relay}/longrun/pair3.json" &
  local p3=$!
  iperf3 -c "${P1_RECV_TS}" -t "$DURATION_LONG" --json \
    2>/dev/null \
    > "$OUT/${relay}/longrun/pair1.json" || true
  wait $p2 2>/dev/null || true
  wait $p3 2>/dev/null || true

  for p in pair1 pair2 pair3; do
    log "    ${p}:"
    extract_result "$OUT/${relay}/longrun/${p}.json" tcp
  done

  # Collect relay memory stats after long run.
  relay_cmd "ps -C hyper-derp -C derper \
    -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null" \
    | tee "$OUT/${relay}/longrun/relay_ps.txt"
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  TUNNEL PHASE 4 — CHAOS / REALISTIC"
log "========================================="

# ---- HD tests ------------------------------------------------

log ""
log "=== Hyper-DERP ==="

HD_WORKERS="${HD_WORKERS:-8}"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; \
  sudo pkill -9 derper 2>/dev/null; sleep 1"
relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
  --port ${HD_PORT} --workers ${HD_WORKERS} \
  --tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem \
  --debug-endpoints --metrics-port 9100 \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 3

switch_derp "$HD_PORT"
drop_caches
start_monitor "hd_p4"

run_churn_test hd "HD"
run_asymmetric_test hd "HD"
run_longrun_test hd "HD"

stop_monitor
download_cpu "hd_p4" "$OUT/hd/cpu"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# ---- TS tests ------------------------------------------------

log ""
log "=== Go derper ==="

relay_cmd "sudo setsid derper -dev \
  -a :${TS_PORT} \
  -certmode manual \
  -certdir /tmp/derper-certs \
  -hostname ${RELAY_INT} \
  </dev/null >/tmp/derper.log 2>&1 &"
sleep 3

switch_derp "$TS_PORT"
drop_caches
start_monitor "ts_p4"

run_churn_test ts "TS"
run_asymmetric_test ts "TS"
run_longrun_test ts "TS"

stop_monitor
download_cpu "ts_p4" "$OUT/ts/cpu"
relay_cmd "sudo pkill -9 derper 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  PHASE 4 COMPLETE"
log "========================================="

python3 -c "
import json, statistics

base = '$OUT'

def parse_tcp(f):
    try:
        d = json.load(open(f))
        return d['end']['sum_received']['bits_per_second'] / 1e6
    except:
        return None

def parse_ping(f):
    try:
        for line in open(f):
            if 'rtt' in line and 'avg' in line:
                return float(line.split('=')[1].split('/')[1])
    except:
        return None

print()
print('=== Churn Test ===')
for relay in ['hd', 'ts']:
    rname = 'HD' if relay == 'hd' else 'TS'
    bulk = parse_tcp(f'{base}/{relay}/churn/bulk_during_churn.json')
    ping = parse_ping(f'{base}/{relay}/churn/ping_during_churn.txt')
    print(f'  {rname}: bulk={bulk:.0f} Mbps, ping={ping:.2f}ms' if bulk and ping else f'  {rname}: FAILED')

print()
print('=== Asymmetric Test ===')
for relay in ['hd', 'ts']:
    rname = 'HD' if relay == 'hd' else 'TS'
    blast = parse_tcp(f'{base}/{relay}/asymmetric/blast.json')
    trickle = parse_tcp(f'{base}/{relay}/asymmetric/trickle.json')
    ping = parse_ping(f'{base}/{relay}/asymmetric/ping.txt')
    if blast and trickle and ping:
        print(f'  {rname}: blast={blast:.0f} Mbps, trickle={trickle:.0f} Mbps, ping={ping:.2f}ms')
    else:
        print(f'  {rname}: PARTIAL DATA')

print()
print('=== Long Duration (300s) ===')
for relay in ['hd', 'ts']:
    rname = 'HD' if relay == 'hd' else 'TS'
    for p in ['pair1', 'pair2', 'pair3']:
        t = parse_tcp(f'{base}/{relay}/longrun/{p}.json')
        print(f'  {rname} {p}: {t:.0f} Mbps' if t else f'  {rname} {p}: FAILED')
" 2>/dev/null || true

touch "$OUT/PHASE4_DONE"
