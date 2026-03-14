#!/bin/bash
# Tunnel Test Phase 3 — Three Pairs (max with current quota)
# Measures scaling and cross-pair interference.
# Run from the bench-client VM (10.10.0.3).
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT=10.10.0.2
SSH_KEY=$HOME/.ssh/id_ed25519_targets
SSH_USER=worker
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

TS_PORT=3340
HD_PORT=3341
RUNS=10
DURATION=15

# Pair definitions (Tailscale IPs / internal IPs).
# Pair 1: bench-client → tunnel-relay
P1_SEND_INT=10.10.0.3
P1_RECV_TS=100.64.0.1
P1_RECV_INT=10.10.0.2

# Pair 2: tunnel-client-2 → tunnel-client-3
P2_SEND_INT=10.10.0.4
P2_RECV_TS=100.64.0.6
P2_RECV_INT=10.10.0.5

# Pair 3: tunnel-client-5 → tunnel-client-6
P3_SEND_INT=10.10.0.7
P3_RECV_TS=100.64.0.4
P3_RECV_INT=10.10.0.8

ALL_SENDERS=("$P1_SEND_INT" "$P2_SEND_INT" "$P3_SEND_INT")
ALL_RECVERS=("$P1_RECV_INT" "$P2_RECV_INT" "$P3_RECV_INT")
ALL_RECV_TS=("$P1_RECV_TS" "$P2_RECV_TS" "$P3_RECV_TS")
PAIR_NAMES=("pair1" "pair2" "pair3")
TS_HOSTNAMES=("tunnel-relay" "tunnel-client-3" "tunnel-client-6")
SEND_HOSTNAMES=("tunnel-client" "tunnel-client-2" "tunnel-client-5")

OUT=/tmp/tunnel_phase3
for relay in hd ts; do
  for pair in pair1 pair2 pair3; do
    mkdir -p "$OUT/${relay}/${pair}"/{tcp1,tcp4,udp,ping}
  done
  mkdir -p "$OUT/${relay}/cpu"
  mkdir -p "$OUT/${relay}/mixed"
done

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

  for i in 0 1 2; do
    local send_int="${ALL_SENDERS[$i]}"
    local recv_int="${ALL_RECVERS[$i]}"
    local send_hn="${SEND_HOSTNAMES[$i]}"
    local recv_hn="${TS_HOSTNAMES[$i]}"
    if [ "$send_int" != "$P1_SEND_INT" ]; then
      remote_cmd "$send_int" "sudo systemctl restart tailscaled; \
        sleep 2; sudo tailscale up \
        --login-server http://${RELAY_INT}:8080 \
        --authkey ${HS_KEY} --hostname ${send_hn} \
        --accept-dns=false 2>/dev/null"
    fi
    if [ "$recv_int" != "$RELAY_INT" ]; then
      remote_cmd "$recv_int" "sudo systemctl restart tailscaled; \
        sleep 2; sudo tailscale up \
        --login-server http://${RELAY_INT}:8080 \
        --authkey ${HS_KEY} --hostname ${recv_hn} \
        --accept-dns=false 2>/dev/null"
    fi
  done

  sleep 3
  fix_nft
  sleep 3

  # Verify all pairs.
  for i in 0 1 2; do
    local recv_ts="${ALL_RECV_TS[$i]}"
    local send_int="${ALL_SENDERS[$i]}"
    if [ "$send_int" = "$P1_SEND_INT" ]; then
      tailscale ping -c 1 "$recv_ts" 2>&1 | head -1
    else
      remote_out "$send_int" \
        "tailscale ping -c 1 ${recv_ts} 2>&1" | head -1
    fi
  done
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
  # Start iperf3 server on each receiver.
  relay_cmd "pkill iperf3 2>/dev/null; sleep 1; \
    setsid iperf3 -s -B ${P1_RECV_TS} -D 2>/dev/null"
  remote_cmd "$P2_RECV_INT" "pkill iperf3 2>/dev/null; \
    sleep 1; setsid iperf3 -s -D 2>/dev/null"
  remote_cmd "$P3_RECV_INT" "pkill iperf3 2>/dev/null; \
    sleep 1; setsid iperf3 -s -D 2>/dev/null"
  sleep 1
}

restart_iperf_servers() {
  pkill -9 iperf3 2>/dev/null || true
  remote_cmd "$P2_SEND_INT" "pkill -9 iperf3 2>/dev/null"
  remote_cmd "$P3_SEND_INT" "pkill -9 iperf3 2>/dev/null"
  start_iperf_servers
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

# ---- Test runners (3 pairs in parallel) ----------------------

run_3pair_test() {
  local test_type=$1 relay=$2 padded=$3
  local extra_args=""
  case $test_type in
    tcp1) extra_args="" ;;
    tcp4) extra_args="-P 4" ;;
    udp)  extra_args="-u -b 0" ;;
  esac

  # Start pairs 2 and 3 in background.
  remote_out "$P2_SEND_INT" \
    "iperf3 -c ${P2_RECV_TS} -t ${DURATION} ${extra_args} \
    --json 2>/dev/null" \
    > "$OUT/${relay}/pair2/${test_type}/r${padded}.json" &
  local p2=$!
  remote_out "$P3_SEND_INT" \
    "iperf3 -c ${P3_RECV_TS} -t ${DURATION} ${extra_args} \
    --json 2>/dev/null" \
    > "$OUT/${relay}/pair3/${test_type}/r${padded}.json" &
  local p3=$!

  # Pair 1 locally.
  iperf3 -c "${P1_RECV_TS}" -t "$DURATION" $extra_args --json \
    2>/dev/null \
    > "$OUT/${relay}/pair1/${test_type}/r${padded}.json" || true

  wait $p2 2>/dev/null || true
  wait $p3 2>/dev/null || true
}

run_3pair_ping() {
  local relay=$1 padded=$2
  remote_out "$P2_SEND_INT" \
    "ping -c 1000 -i 0.01 ${P2_RECV_TS} 2>/dev/null" \
    > "$OUT/${relay}/pair2/ping/r${padded}.txt" &
  local p2=$!
  remote_out "$P3_SEND_INT" \
    "ping -c 1000 -i 0.01 ${P3_RECV_TS} 2>/dev/null" \
    > "$OUT/${relay}/pair3/ping/r${padded}.txt" &
  local p3=$!
  ping -c 1000 -i 0.01 "${P1_RECV_TS}" 2>/dev/null \
    > "$OUT/${relay}/pair1/ping/r${padded}.txt" || true
  wait $p2 2>/dev/null || true
  wait $p3 2>/dev/null || true
}

# ---- Run test phase for one relay ----------------------------

run_phase() {
  local relay=$1 label=$2

  for run in $(seq 1 $RUNS); do
    padded=$(printf '%02d' "$run")
    log "  [$run/$RUNS] ${label} 3-pair tests:"

    log "    tcp1..."
    run_3pair_test tcp1 "$relay" "$padded"
    for p in pair1 pair2 pair3; do
      echo -n "    ${p}:"
      extract_result "$OUT/${relay}/${p}/tcp1/r${padded}.json" tcp
    done

    log "    tcp4..."
    run_3pair_test tcp4 "$relay" "$padded"
    for p in pair1 pair2 pair3; do
      echo -n "    ${p}:"
      extract_result "$OUT/${relay}/${p}/tcp4/r${padded}.json" tcp
    done

    log "    udp..."
    run_3pair_test udp "$relay" "$padded"
    for p in pair1 pair2 pair3; do
      echo -n "    ${p}:"
      extract_result "$OUT/${relay}/${p}/udp/r${padded}.json" udp
    done

    log "    ping..."
    run_3pair_ping "$relay" "$padded"
    for p in pair1 pair2 pair3; do
      echo -n "    ${p}:"
      extract_result "$OUT/${relay}/${p}/ping/r${padded}.txt" ping
    done

    restart_iperf_servers
    sleep 2
  done
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  TUNNEL PHASE 3 — THREE PAIRS"
log "========================================="

# ---- TS (Go derper) -----------------------------------------

log ""
log "Phase 3A: Go derper (TS) on port ${TS_PORT}"
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
start_monitor "ts_p3"
start_iperf_servers

run_phase ts "TS"

stop_monitor
download_cpu "ts_p3" "$OUT/ts/cpu"
relay_cmd "pkill iperf3 2>/dev/null"
for recv in "$P2_RECV_INT" "$P3_RECV_INT"; do
  remote_cmd "$recv" "pkill iperf3 2>/dev/null"
done
relay_cmd "sudo pkill -9 derper 2>/dev/null"

# ---- HD (Hyper-DERP) ----------------------------------------

log ""
log "Phase 3B: Hyper-DERP (HD) on port ${HD_PORT}"

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
start_monitor "hd_p3"
start_iperf_servers

run_phase hd "HD"

stop_monitor
download_cpu "hd_p3" "$OUT/hd/cpu"
relay_cmd "pkill iperf3 2>/dev/null"
for recv in "$P2_RECV_INT" "$P3_RECV_INT"; do
  remote_cmd "$recv" "pkill iperf3 2>/dev/null"
done

# ---- Mixed Workload Test (Phase 3 bonus) --------------------

log ""
log "Phase 3C: Mixed workload (2 bulk + 1 interactive)"
log "  Using HD relay"

# HD should still be running.
start_iperf_servers

for run in $(seq 1 5); do
  padded=$(printf '%02d' "$run")
  log "  [$run/5] Mixed workload:"

  # Pairs 1+2: bulk TCP, Pair 3: ping.
  remote_out "$P2_SEND_INT" \
    "iperf3 -c ${P2_RECV_TS} -t ${DURATION} 2>/dev/null" \
    > /dev/null &
  local bulk2=$!
  iperf3 -c "${P1_RECV_TS}" -t "$DURATION" 2>/dev/null \
    > /dev/null &
  local bulk1=$!
  sleep 2
  remote_out "$P3_SEND_INT" \
    "ping -c 500 -i 0.02 ${P3_RECV_TS} 2>/dev/null" \
    > "$OUT/hd/mixed/ping_under_bulk_r${padded}.txt"
  wait $bulk1 2>/dev/null || true
  wait $bulk2 2>/dev/null || true
  extract_result \
    "$OUT/hd/mixed/ping_under_bulk_r${padded}.txt" ping
  restart_iperf_servers
  sleep 2
done

relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  PHASE 3 COMPLETE"
log "========================================="

python3 -c "
import json, glob, statistics

def parse_iperf(files, mode='tcp'):
    tputs = []
    for f in sorted(files):
        try:
            d = json.load(open(f))
            if mode == 'udp':
                t = d['end']['sum']['bits_per_second'] / 1e6
            else:
                t = d['end']['sum_received']['bits_per_second'] / 1e6
            if t > 100:
                tputs.append(t)
        except:
            pass
    return tputs

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

base = '$OUT'
print()
print(f'{\"\":<28} {\"Mean\":>8} {\"Min\":>8} {\"Max\":>8}  N')
print('=' * 60)

for relay in ['ts', 'hd']:
    rname = 'Go derper' if relay == 'ts' else 'Hyper-DERP'
    print(f'--- {rname} ---')
    agg_tcp1 = []
    for pair in ['pair1', 'pair2', 'pair3']:
        t = parse_iperf(glob.glob(f'{base}/{relay}/{pair}/tcp1/r*.json'))
        agg_tcp1.extend(t)
        if t:
            m = statistics.mean(t)
            print(f'  {pair} TCP1: {m:>8.0f} {min(t):>8.0f} {max(t):>8.0f}  n={len(t)}')
    if agg_tcp1:
        print(f'  Aggregate TCP1: {sum(agg_tcp1)/len(agg_tcp1)*3:.0f} Mbps total')
    p_all = []
    for pair in ['pair1', 'pair2', 'pair3']:
        p = parse_ping(glob.glob(f'{base}/{relay}/{pair}/ping/r*.txt'))
        p_all.extend(p)
    if p_all:
        m = statistics.mean(p_all)
        print(f'  Ping avg: {m:.2f}ms  (n={len(p_all)})')
    print()

# Mixed workload results.
mixed = parse_ping(glob.glob(f'{base}/hd/mixed/ping_under_bulk_r*.txt'))
if mixed:
    m = statistics.mean(mixed)
    print(f'  HD Mixed (ping while 2 bulk): {m:.2f}ms  (n={len(mixed)})')
" 2>/dev/null || true

touch "$OUT/PHASE3_DONE"
