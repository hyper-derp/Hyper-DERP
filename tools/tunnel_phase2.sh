#!/bin/bash
# Tunnel Test Phase 2 — Two Pairs Simultaneously
# Measures cross-shard forwarding and resource sharing.
# Run from the bench-client VM (10.10.0.3).
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT=${RELAY_INT:?Set RELAY_INT env var}
SSH_KEY=${SSH_KEY:?Set SSH_KEY env var}
SSH_USER=${SSH_USER:-worker}
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

TS_PORT=3340
HD_PORT=3341
RUNS=10
DURATION=15

# Pair definitions: sender → receiver (Tailscale IPs).
# Pair 1: bench-client (100.64.0.2) → tunnel-relay (100.64.0.1)
# Pair 2: tunnel-client-2 (100.64.0.5) → tunnel-client-3 (100.64.0.6)
PAIR1_SENDER_INT=${PAIR1_SENDER_INT:?Set PAIR1_SENDER_INT env var}
PAIR1_RECV_TS=${PAIR1_RECV_TS:?Set PAIR1_RECV_TS env var}
PAIR1_RECV_INT=${PAIR1_RECV_INT:?Set PAIR1_RECV_INT env var}

PAIR2_SENDER_INT=${PAIR2_SENDER_INT:?Set PAIR2_SENDER_INT env var}
PAIR2_RECV_TS=${PAIR2_RECV_TS:?Set PAIR2_RECV_TS env var}
PAIR2_RECV_INT=${PAIR2_RECV_INT:?Set PAIR2_RECV_INT env var}

OUT=/tmp/tunnel_phase2
mkdir -p "$OUT"/{hd,ts}/{pair1,pair2}/{tcp1,tcp4,udp,ping,latload}
mkdir -p "$OUT"/{hd,ts}/cpu

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
  # Ensure tailscale0 traffic allowed in nftables on relay and
  # bench-client (nftables policy drop).
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

  # Reconnect all Tailscale clients.
  relay_cmd "sudo systemctl restart tailscaled; sleep 2; \
    sudo tailscale up --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-relay \
    --accept-dns=false 2>/dev/null"

  # Restart local tailscaled.
  sudo systemctl restart tailscaled
  sleep 2
  sudo tailscale up --login-server "http://${RELAY_INT}:8080" \
    --authkey "${HS_KEY}" --hostname tunnel-client \
    --accept-dns=false 2>/dev/null

  # Restart on pair 2 VMs.
  remote_cmd "$PAIR2_SENDER_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-2 \
    --accept-dns=false 2>/dev/null"
  remote_cmd "$PAIR2_RECV_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-3 \
    --accept-dns=false 2>/dev/null"

  sleep 3
  fix_nft
  sleep 3

  # Verify DERP connections.
  local ok
  ok=$(tailscale ping -c 1 "${PAIR1_RECV_TS}" 2>&1 \
    | grep -c "pong")
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: pair1 DERP ping failed, retrying..."
    sleep 5
    fix_nft
    sleep 3
  fi
  ok=$(remote_out "$PAIR2_SENDER_INT" \
    "tailscale ping -c 1 ${PAIR2_RECV_TS} 2>&1" \
    | grep -c "pong")
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: pair2 DERP ping failed, retrying..."
    sleep 5
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

# Start iperf3 servers on both receivers.
start_iperf_servers() {
  relay_cmd "pkill iperf3 2>/dev/null; sleep 1; \
    setsid iperf3 -s -B ${PAIR1_RECV_TS} -D 2>/dev/null"
  remote_cmd "$PAIR2_RECV_INT" "pkill iperf3 2>/dev/null; \
    sleep 1; setsid iperf3 -s -D 2>/dev/null"
  sleep 1
}

restart_iperf_servers() {
  # Kill all iperf3 on this VM and pair2 sender.
  pkill -9 iperf3 2>/dev/null || true
  remote_cmd "$PAIR2_SENDER_INT" \
    "pkill -9 iperf3 2>/dev/null"
  start_iperf_servers
}

# Run test on both pairs simultaneously.
# Args: test_type outdir_pair1 outdir_pair2
run_parallel_tcp1() {
  local out1=$1 out2=$2
  # Start pair 2 in background via SSH.
  remote_cmd "$PAIR2_SENDER_INT" \
    "iperf3 -c ${PAIR2_RECV_TS} -t ${DURATION} --json \
    2>/dev/null" > "$out2" &
  local p2_pid=$!
  # Run pair 1 locally.
  iperf3 -c "${PAIR1_RECV_TS}" -t "$DURATION" --json \
    2>/dev/null > "$out1" || true
  wait $p2_pid 2>/dev/null || true
}

run_parallel_tcp4() {
  local out1=$1 out2=$2
  remote_cmd "$PAIR2_SENDER_INT" \
    "iperf3 -c ${PAIR2_RECV_TS} -t ${DURATION} -P 4 --json \
    2>/dev/null" > "$out2" &
  local p2_pid=$!
  iperf3 -c "${PAIR1_RECV_TS}" -t "$DURATION" -P 4 --json \
    2>/dev/null > "$out1" || true
  wait $p2_pid 2>/dev/null || true
}

run_parallel_udp() {
  local out1=$1 out2=$2
  remote_cmd "$PAIR2_SENDER_INT" \
    "iperf3 -c ${PAIR2_RECV_TS} -u -b 0 -t ${DURATION} --json \
    2>/dev/null" > "$out2" &
  local p2_pid=$!
  iperf3 -c "${PAIR1_RECV_TS}" -u -b 0 -t "$DURATION" --json \
    2>/dev/null > "$out1" || true
  wait $p2_pid 2>/dev/null || true
}

run_parallel_ping() {
  local out1=$1 out2=$2
  remote_cmd "$PAIR2_SENDER_INT" \
    "ping -c 1000 -i 0.01 ${PAIR2_RECV_TS} 2>/dev/null" \
    > "$out2" &
  local p2_pid=$!
  ping -c 1000 -i 0.01 "${PAIR1_RECV_TS}" \
    2>/dev/null > "$out1" || true
  wait $p2_pid 2>/dev/null || true
}

run_parallel_latload() {
  local out1=$1 out2=$2
  # Background load on both pairs.
  iperf3 -c "${PAIR1_RECV_TS}" -t "$DURATION" \
    2>/dev/null > /dev/null &
  local load1_pid=$!
  remote_cmd "$PAIR2_SENDER_INT" \
    "iperf3 -c ${PAIR2_RECV_TS} -t ${DURATION} \
    2>/dev/null > /dev/null &
    sleep 2; \
    ping -c 500 -i 0.02 ${PAIR2_RECV_TS} 2>/dev/null" \
    > "$out2" &
  local p2_pid=$!
  sleep 2
  ping -c 500 -i 0.02 "${PAIR1_RECV_TS}" \
    2>/dev/null > "$out1" || true
  kill $load1_pid 2>/dev/null || true
  wait $load1_pid 2>/dev/null || true
  wait $p2_pid 2>/dev/null || true
  restart_iperf_servers
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
log "  TUNNEL PHASE 2 — TWO PAIRS"
log "========================================="

# ---- TS (Go derper) -----------------------------------------

log ""
log "Phase 2A: Go derper (TS) on port ${TS_PORT}"
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
start_monitor "ts_p2"
start_iperf_servers

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] TS 2-pair tests:"

  log "    tcp1..."
  run_parallel_tcp1 \
    "$OUT/ts/pair1/tcp1/r${padded}.json" \
    "$OUT/ts/pair2/tcp1/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/ts/pair1/tcp1/r${padded}.json" tcp
  echo -n "    P2:"; extract_result \
    "$OUT/ts/pair2/tcp1/r${padded}.json" tcp

  log "    tcp4..."
  run_parallel_tcp4 \
    "$OUT/ts/pair1/tcp4/r${padded}.json" \
    "$OUT/ts/pair2/tcp4/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/ts/pair1/tcp4/r${padded}.json" tcp
  echo -n "    P2:"; extract_result \
    "$OUT/ts/pair2/tcp4/r${padded}.json" tcp

  log "    udp..."
  run_parallel_udp \
    "$OUT/ts/pair1/udp/r${padded}.json" \
    "$OUT/ts/pair2/udp/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/ts/pair1/udp/r${padded}.json" udp
  echo -n "    P2:"; extract_result \
    "$OUT/ts/pair2/udp/r${padded}.json" udp

  log "    ping..."
  run_parallel_ping \
    "$OUT/ts/pair1/ping/r${padded}.txt" \
    "$OUT/ts/pair2/ping/r${padded}.txt"
  echo -n "    P1:"; extract_result \
    "$OUT/ts/pair1/ping/r${padded}.txt" ping
  echo -n "    P2:"; extract_result \
    "$OUT/ts/pair2/ping/r${padded}.txt" ping

  log "    latload..."
  run_parallel_latload \
    "$OUT/ts/pair1/latload/r${padded}.txt" \
    "$OUT/ts/pair2/latload/r${padded}.txt"
  echo -n "    P1:"; extract_result \
    "$OUT/ts/pair1/latload/r${padded}.txt" ping
  echo -n "    P2:"; extract_result \
    "$OUT/ts/pair2/latload/r${padded}.txt" ping

  sleep 2
done

stop_monitor
download_cpu "ts_p2" "$OUT/ts/cpu"
relay_cmd "pkill iperf3 2>/dev/null"
remote_cmd "$PAIR2_RECV_INT" "pkill iperf3 2>/dev/null"
relay_cmd "sudo pkill -9 derper 2>/dev/null"

# ---- HD (Hyper-DERP) ----------------------------------------

log ""
log "Phase 2B: Hyper-DERP (HD) on port ${HD_PORT}"

HD_WORKERS="${HD_WORKERS:-8}"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
  --port ${HD_PORT} --workers ${HD_WORKERS} \
  --tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem \
  --debug-endpoints --metrics-port 9100 \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 3
log "  HD started with ${HD_WORKERS} workers"

switch_derp "$HD_PORT"
drop_caches
start_monitor "hd_p2"
start_iperf_servers

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] HD 2-pair tests:"

  log "    tcp1..."
  run_parallel_tcp1 \
    "$OUT/hd/pair1/tcp1/r${padded}.json" \
    "$OUT/hd/pair2/tcp1/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/hd/pair1/tcp1/r${padded}.json" tcp
  echo -n "    P2:"; extract_result \
    "$OUT/hd/pair2/tcp1/r${padded}.json" tcp

  log "    tcp4..."
  run_parallel_tcp4 \
    "$OUT/hd/pair1/tcp4/r${padded}.json" \
    "$OUT/hd/pair2/tcp4/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/hd/pair1/tcp4/r${padded}.json" tcp
  echo -n "    P2:"; extract_result \
    "$OUT/hd/pair2/tcp4/r${padded}.json" tcp

  log "    udp..."
  run_parallel_udp \
    "$OUT/hd/pair1/udp/r${padded}.json" \
    "$OUT/hd/pair2/udp/r${padded}.json"
  echo -n "    P1:"; extract_result \
    "$OUT/hd/pair1/udp/r${padded}.json" udp
  echo -n "    P2:"; extract_result \
    "$OUT/hd/pair2/udp/r${padded}.json" udp

  log "    ping..."
  run_parallel_ping \
    "$OUT/hd/pair1/ping/r${padded}.txt" \
    "$OUT/hd/pair2/ping/r${padded}.txt"
  echo -n "    P1:"; extract_result \
    "$OUT/hd/pair1/ping/r${padded}.txt" ping
  echo -n "    P2:"; extract_result \
    "$OUT/hd/pair2/ping/r${padded}.txt" ping

  log "    latload..."
  run_parallel_latload \
    "$OUT/hd/pair1/latload/r${padded}.txt" \
    "$OUT/hd/pair2/latload/r${padded}.txt"
  echo -n "    P1:"; extract_result \
    "$OUT/hd/pair1/latload/r${padded}.txt" ping
  echo -n "    P2:"; extract_result \
    "$OUT/hd/pair2/latload/r${padded}.txt" ping

  sleep 2
done

stop_monitor
download_cpu "hd_p2" "$OUT/hd/cpu"
relay_cmd "pkill iperf3 2>/dev/null"
remote_cmd "$PAIR2_RECV_INT" "pkill iperf3 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  PHASE 2 COMPLETE"
log "========================================="

python3 -c "
import json, glob, statistics

def parse_iperf(files, mode='tcp'):
    tputs, retrans, losses = [], [], []
    for f in sorted(files):
        try:
            d = json.load(open(f))
            if mode == 'udp':
                t = d['end']['sum']['bits_per_second'] / 1e6
                if t > 100:
                    tputs.append(t)
                    losses.append(d['end']['sum']['lost_percent'])
            else:
                t = d['end']['sum_received']['bits_per_second'] / 1e6
                if t > 100:
                    tputs.append(t)
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
                    v = float(parts[1])
                    if v < 100:
                        avgs.append(v)
                    break
        except:
            pass
    return avgs

def show(label, values, unit='Mbps'):
    if not values:
        print(f'  {label:<24} NO DATA')
        return
    m = statistics.mean(values)
    sd = statistics.stdev(values) if len(values) > 1 else 0
    cv = sd / m * 100 if m > 0 else 0
    print(f'  {label:<24} {m:>8.0f} {min(values):>8.0f} {max(values):>8.0f} {sd:>8.0f} {cv:>5.1f}%  n={len(values)}')

base = '$OUT'
print()
print(f'{\"\":<24} {\"Mean\":>8} {\"Min\":>8} {\"Max\":>8} {\"StdDev\":>8} {\"CV%\":>6}')
print('=' * 68)

for relay in ['ts', 'hd']:
    rname = 'Go derper' if relay == 'ts' else 'Hyper-DERP'
    for pair in ['pair1', 'pair2']:
        print(f'--- {rname} / {pair} ---')
        t1, r1, _ = parse_iperf(
            glob.glob(f'{base}/{relay}/{pair}/tcp1/r*.json'))
        show('TCP 1-stream', t1)
        t4, _, _ = parse_iperf(
            glob.glob(f'{base}/{relay}/{pair}/tcp4/r*.json'))
        show('TCP 4-stream', t4)
        p = parse_ping(
            glob.glob(f'{base}/{relay}/{pair}/ping/r*.txt'))
        if p:
            m = statistics.mean(p)
            print(f'  {\"Ping avg (ms)\":<24} {m:>8.2f} {min(p):>8.2f} {max(p):>8.2f}')
        print()

# Aggregate comparison.
print('--- Aggregate (both pairs summed) ---')
for relay in ['ts', 'hd']:
    rname = 'TS' if relay == 'ts' else 'HD'
    t1_all = []
    for pair in ['pair1', 'pair2']:
        t, _, _ = parse_iperf(
            glob.glob(f'{base}/{relay}/{pair}/tcp1/r*.json'))
        t1_all.extend(t)
    if t1_all:
        print(f'  {rname} TCP1 all pairs: {statistics.mean(t1_all):.0f} Mbps  n={len(t1_all)}')
print()

# Fairness: ratio of pair1/pair2 throughput.
for relay in ['ts', 'hd']:
    rname = 'TS' if relay == 'ts' else 'HD'
    t1_p1, _, _ = parse_iperf(
        glob.glob(f'{base}/{relay}/pair1/tcp1/r*.json'))
    t1_p2, _, _ = parse_iperf(
        glob.glob(f'{base}/{relay}/pair2/tcp1/r*.json'))
    if t1_p1 and t1_p2:
        ratio = statistics.mean(t1_p1) / statistics.mean(t1_p2)
        print(f'  {rname} fairness (P1/P2): {ratio:.2f}')
" 2>/dev/null || true

touch "$OUT/PHASE2_DONE"
