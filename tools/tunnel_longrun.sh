#!/bin/bash
# Tunnel Long Duration Rerun — 5 min sustained, 3 pairs.
# Fixes: full DERP reconnect + iperf3 restart before each relay.
# Run from the bench-client machine.
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY_INT="${RELAY:?Set RELAY env var (relay internal IP)}"
SSH_KEY="${SSH_KEY:?Set SSH_KEY env var (path to SSH key)}"
SSH_USER="${RELAY_USER:-worker}"
HS_KEY="${HS_KEY:?Set HS_KEY env var (headscale preauthkey)}"

TS_PORT=3340
HD_PORT=3341
DURATION=300

P1_RECV_TS="${P1_RECV_TS:?Set P1_RECV_TS env var}"
P2_SEND_INT="${P2_SEND_INT:?Set P2_SEND_INT env var}"
P2_RECV_TS="${P2_RECV_TS:?Set P2_RECV_TS env var}"
P2_RECV_INT="${P2_RECV_INT:?Set P2_RECV_INT env var}"
P3_SEND_INT="${P3_SEND_INT:?Set P3_SEND_INT env var}"
P3_RECV_TS="${P3_RECV_TS:?Set P3_RECV_TS env var}"
P3_RECV_INT="${P3_RECV_INT:?Set P3_RECV_INT env var}"

OUT=/tmp/tunnel_longrun
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
  timeout 600 ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o ServerAliveInterval=30 \
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

  remote_cmd "$P2_SEND_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-2 \
    --accept-dns=false 2>/dev/null"
  remote_cmd "$P2_RECV_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-3 \
    --accept-dns=false 2>/dev/null"
  remote_cmd "$P3_SEND_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-5 \
    --accept-dns=false 2>/dev/null"
  remote_cmd "$P3_RECV_INT" "sudo systemctl restart tailscaled; \
    sleep 2; sudo tailscale up \
    --login-server http://${RELAY_INT}:8080 \
    --authkey ${HS_KEY} --hostname tunnel-client-6 \
    --accept-dns=false 2>/dev/null"

  sleep 3
  fix_nft
  sleep 3

  # Verify all pairs via DERP.
  tailscale ping -c 1 "${P1_RECV_TS}" 2>&1 | head -1
  remote_cmd "$P2_SEND_INT" \
    "tailscale ping -c 1 ${P2_RECV_TS} 2>&1 | head -1"
  remote_cmd "$P3_SEND_INT" \
    "tailscale ping -c 1 ${P3_RECV_TS} 2>&1 | head -1"
}

start_iperf_servers() {
  log "  Starting iperf3 servers..."
  # Kill everything first.
  pkill -9 iperf3 2>/dev/null || true
  relay_cmd "pkill -9 iperf3 2>/dev/null"
  remote_cmd "$P2_SEND_INT" "pkill -9 iperf3 2>/dev/null"
  remote_cmd "$P2_RECV_INT" "pkill -9 iperf3 2>/dev/null"
  remote_cmd "$P3_SEND_INT" "pkill -9 iperf3 2>/dev/null"
  remote_cmd "$P3_RECV_INT" "pkill -9 iperf3 2>/dev/null"
  sleep 2

  # Start fresh servers.
  relay_cmd "setsid iperf3 -s -B ${P1_RECV_TS} -D 2>/dev/null"
  remote_cmd "$P2_RECV_INT" \
    "setsid iperf3 -s -D 2>/dev/null"
  remote_cmd "$P3_RECV_INT" \
    "setsid iperf3 -s -D 2>/dev/null"
  sleep 2

  # Verify servers are listening.
  log "  Verifying iperf3 servers..."
  relay_cmd "ss -tlnp | grep 5201 | head -1"
  remote_cmd "$P2_RECV_INT" "ss -tlnp | grep 5201 | head -1"
  remote_cmd "$P3_RECV_INT" "ss -tlnp | grep 5201 | head -1"
}

start_monitor() {
  local label=$1
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x hyper-derp || \
    pgrep -x derper); \
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

extract_result() {
  local file=$1
  python3 -c "
import json
try:
    d = json.load(open('${file}'))
    bps = d['end']['sum_received']['bits_per_second'] / 1e6
    retr = d['end']['sum_sent'].get('retransmits', 0)
    dur = d['end']['sum_received'].get('seconds', 0)
    print(f'  {bps:.0f} Mbps, {retr} retrans, {dur:.0f}s')
except Exception as e:
    print(f'  FAILED: {e}')
" 2>/dev/null
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  LONG DURATION RERUN — 300s SUSTAINED"
log "========================================="

# ---- HD (Hyper-DERP) ----------------------------------------

log ""
log "=== Hyper-DERP (8 workers) ==="

relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; \
  sudo pkill -9 derper 2>/dev/null; sleep 1"

HD_WORKERS="${HD_WORKERS:-8}"
relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
  --port ${HD_PORT} --workers ${HD_WORKERS} \
  --tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem \
  --debug-endpoints --metrics-port 9100 \
  </dev/null >/tmp/hd.log 2>&1 &"
sleep 3
log "  HD started with ${HD_WORKERS} workers"

switch_derp "$HD_PORT"
drop_caches
start_iperf_servers
start_monitor "longrun_hd"

# Quick sanity: 15s test first.
log "  Sanity check (15s)..."
iperf3 -c "${P1_RECV_TS}" -t 15 --json 2>/dev/null \
  > "$OUT/hd/sanity.json" || true
extract_result "$OUT/hd/sanity.json"

# Record relay process stats before.
log "  Relay stats before long run:"
relay_cmd "ps -C hyper-derp \
  -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null"

# Start 300s sustained on all 3 pairs.
log "  Starting 300s sustained load on 3 pairs..."
remote_out "$P2_SEND_INT" \
  "iperf3 -c ${P2_RECV_TS} -t ${DURATION} --json 2>/dev/null" \
  > "$OUT/hd/pair2.json" &
hd_p2=$!
remote_out "$P3_SEND_INT" \
  "iperf3 -c ${P3_RECV_TS} -t ${DURATION} --json 2>/dev/null" \
  > "$OUT/hd/pair3.json" &
hd_p3=$!
iperf3 -c "${P1_RECV_TS}" -t "$DURATION" --json 2>/dev/null \
  > "$OUT/hd/pair1.json" || true
wait $hd_p2 2>/dev/null || true
wait $hd_p3 2>/dev/null || true

log "  HD long duration results:"
log "    pair1:"; extract_result "$OUT/hd/pair1.json"
log "    pair2:"; extract_result "$OUT/hd/pair2.json"
log "    pair3:"; extract_result "$OUT/hd/pair3.json"

# Record relay process stats after.
log "  Relay stats after long run:"
relay_cmd "ps -C hyper-derp \
  -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null" \
  | tee "$OUT/hd/relay_ps_after.txt"

stop_monitor
download_cpu "longrun_hd" "$OUT/hd/cpu"

# Kill HD, clean up all iperf3.
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"
pkill -9 iperf3 2>/dev/null || true
remote_cmd "$P2_SEND_INT" "pkill -9 iperf3 2>/dev/null"
remote_cmd "$P2_RECV_INT" "pkill -9 iperf3 2>/dev/null"
remote_cmd "$P3_SEND_INT" "pkill -9 iperf3 2>/dev/null"
remote_cmd "$P3_RECV_INT" "pkill -9 iperf3 2>/dev/null"
sleep 3

# ---- TS (Go derper) ------------------------------------------

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
start_iperf_servers
start_monitor "longrun_ts"

# Quick sanity.
log "  Sanity check (15s)..."
iperf3 -c "${P1_RECV_TS}" -t 15 --json 2>/dev/null \
  > "$OUT/ts/sanity.json" || true
extract_result "$OUT/ts/sanity.json"

# Record relay process stats before.
log "  Relay stats before long run:"
relay_cmd "ps -C derper \
  -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null"

# Start 300s sustained on all 3 pairs.
log "  Starting 300s sustained load on 3 pairs..."
remote_out "$P2_SEND_INT" \
  "iperf3 -c ${P2_RECV_TS} -t ${DURATION} --json 2>/dev/null" \
  > "$OUT/ts/pair2.json" &
ts_p2=$!
remote_out "$P3_SEND_INT" \
  "iperf3 -c ${P3_RECV_TS} -t ${DURATION} --json 2>/dev/null" \
  > "$OUT/ts/pair3.json" &
ts_p3=$!
iperf3 -c "${P1_RECV_TS}" -t "$DURATION" --json 2>/dev/null \
  > "$OUT/ts/pair1.json" || true
wait $ts_p2 2>/dev/null || true
wait $ts_p3 2>/dev/null || true

log "  TS long duration results:"
log "    pair1:"; extract_result "$OUT/ts/pair1.json"
log "    pair2:"; extract_result "$OUT/ts/pair2.json"
log "    pair3:"; extract_result "$OUT/ts/pair3.json"

# Record relay process stats after.
log "  Relay stats after long run:"
relay_cmd "ps -C derper \
  -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null" \
  | tee "$OUT/ts/relay_ps_after.txt"

stop_monitor
download_cpu "longrun_ts" "$OUT/ts/cpu"
relay_cmd "sudo pkill -9 derper 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  LONG DURATION COMPLETE"
log "========================================="

python3 -c "
import json

def parse(f):
    try:
        d = json.load(open(f))
        bps = d['end']['sum_received']['bits_per_second'] / 1e6
        retr = d['end']['sum_sent'].get('retransmits', 0)
        dur = d['end']['sum_received'].get('seconds', 0)
        return bps, retr, dur
    except:
        return None, None, None

base = '$OUT'
print()
print(f'{'':>10} {'Pair1':>12} {'Pair2':>12} {'Pair3':>12} {'Agg':>12} {'Retrans':>12}')
print('-' * 72)
for relay in ['hd', 'ts']:
    rname = 'HD' if relay == 'hd' else 'TS'
    total_bps, total_retr = 0, 0
    parts = []
    for p in ['pair1', 'pair2', 'pair3']:
        bps, retr, dur = parse(f'{base}/{relay}/{p}.json')
        if bps:
            parts.append(f'{bps:>8.0f}')
            total_bps += bps
            total_retr += retr
        else:
            parts.append(f'{'FAIL':>8}')
    print(f'{rname:>10} {parts[0]:>12} {parts[1]:>12} {parts[2]:>12} {total_bps:>8.0f} Mbps {total_retr:>8} retr')

# Ratios
hd1 = parse(f'{base}/hd/pair1.json')
ts1 = parse(f'{base}/ts/pair1.json')
if hd1[0] and ts1[0]:
    print()
    print(f'P1 throughput ratio: {hd1[0]/ts1[0]:.2f}x')
    if ts1[1] and hd1[1]:
        print(f'P1 retransmit ratio: {ts1[1]/hd1[1]:.1f}x fewer')
        print(f'P1 retransmit rate:  HD {hd1[1]/300:.0f}/s vs TS {ts1[1]/300:.0f}/s')
" 2>/dev/null || true

touch "$OUT/LONGRUN_DONE"
