#!/bin/bash
# Tunnel Test Phase 0 — Tailscale Familiarization
# Run from the bench host (not on the VMs).
# Answers all open questions from TUNNEL_TEST_DESIGN.md.
set -uo pipefail

# ---- Config --------------------------------------------------

RELAY=${RELAY:?Set RELAY env var}
RELAY_INT=${RELAY_INT:?Set RELAY_INT env var}
CLIENT=${CLIENT:?Set CLIENT env var}
CLIENT_INT=${CLIENT_INT:?Set CLIENT_INT env var}
SSH_KEY=${SSH_KEY:?Set SSH_KEY env var}
SSH_USER=${SSH_USER:-worker}
HD_PORT=3341
TS_PORT=3340

DERP_MAP_PORT=443
# Set this before running:
TS_AUTHKEY="${TS_AUTHKEY:?Set TS_AUTHKEY env var}"

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

ssh_relay() {
  ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${RELAY}" "$1" 2>/dev/null
}

ssh_client() {
  ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT}" "$1" 2>/dev/null
}

# ---- Step 0: Verify Tailscale installed ----------------------

log "Step 0: Verify Tailscale installed"
relay_ver=$(ssh_relay "tailscale version 2>/dev/null | head -1")
client_ver=$(ssh_client "tailscale version 2>/dev/null | head -1")
log "  Relay:  ${relay_ver:-NOT INSTALLED}"
log "  Client: ${client_ver:-NOT INSTALLED}"
if [ -z "$relay_ver" ] || [ -z "$client_ver" ]; then
  log "ERROR: Tailscale not installed on one or both VMs"
  exit 1
fi

# ---- Step 1: Start Go derper on relay with TLS ---------------

log ""
log "Step 1: Start Go derper on relay with TLS"
ssh_relay "sudo pkill -9 derper 2>/dev/null; sleep 1"

# Generate cert with IP SAN.
ssh_relay "
openssl req -x509 -newkey ec \
  -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /tmp/key.pem -out /tmp/cert.pem \
  -days 1 -nodes \
  -subj '/CN=${RELAY_INT}' \
  -addext 'subjectAltName=IP:${RELAY_INT},DNS:bench-relay' \
  2>/dev/null
CERTDIR=/tmp/derper-certs
mkdir -p \"\\\$CERTDIR\"
cp /tmp/cert.pem \"\\\$CERTDIR/${RELAY_INT}.crt\"
cp /tmp/key.pem \"\\\$CERTDIR/${RELAY_INT}.key\"
"

ssh_relay "sudo setsid derper -dev \
  -a :${DERP_MAP_PORT} \
  -certmode manual \
  -certdir /tmp/derper-certs \
  -hostname ${RELAY_INT} \
  </dev/null >/tmp/derper.log 2>&1 &"
sleep 4

ok=$(ssh_relay "ss -tlnp | grep -c :${DERP_MAP_PORT}" || echo 0)
if [ "${ok:-0}" -lt 1 ]; then
  log "ERROR: derper not listening on :${DERP_MAP_PORT}"
  ssh_relay "cat /tmp/derper.log" | tail -10
  exit 1
fi
log "  Go derper listening on :${DERP_MAP_PORT} (TLS)"

# ---- Step 2: Write custom DERP map --------------------------

log ""
log "Step 2: Write custom DERP map"

# Write DERP map to both VMs.
DERP_MAP='{
  "Regions": {
    "900": {
      "RegionID": 900,
      "RegionCode": "test",
      "RegionName": "Test Relay",
      "Nodes": [{
        "Name": "test-relay",
        "RegionID": 900,
        "HostName": "'"${RELAY_INT}"'",
        "DERPPort": '"${DERP_MAP_PORT}"',
        "InsecureForTests": true
      }]
    }
  },
  "OmitDefaultRegions": true
}'

ssh_relay "echo '${DERP_MAP}' | sudo tee /etc/tailscale/derp-map.json > /dev/null"
ssh_client "sudo mkdir -p /etc/tailscale && echo '${DERP_MAP}' | sudo tee /etc/tailscale/derp-map.json > /dev/null"
log "  DERP map written to both VMs"
log "  OmitDefaultRegions=true, InsecureForTests=true"

# ---- Step 3: Bring up Tailscale on both VMs ------------------

log ""
log "Step 3: Bring up Tailscale"

# First, down any existing sessions.
ssh_relay "sudo tailscale down 2>/dev/null" || true
ssh_client "sudo tailscale down 2>/dev/null" || true
sleep 1

# Bring up with auth key and custom DERP map.
log "  Bringing up relay VM..."
relay_up=$(ssh_relay "sudo tailscale up \
  --authkey='${TS_AUTHKEY}' \
  --hostname=tunnel-relay \
  --accept-routes=false \
  2>&1")
log "  Relay: ${relay_up:-ok}"

log "  Bringing up client VM..."
client_up=$(ssh_client "sudo tailscale up \
  --authkey='${TS_AUTHKEY}' \
  --hostname=tunnel-client \
  --accept-routes=false \
  2>&1")
log "  Client: ${client_up:-ok}"

sleep 3

# Get Tailscale IPs.
relay_ts_ip=$(ssh_relay "tailscale ip -4 2>/dev/null")
client_ts_ip=$(ssh_client "tailscale ip -4 2>/dev/null")
log "  Relay Tailscale IP:  ${relay_ts_ip:-NONE}"
log "  Client Tailscale IP: ${client_ts_ip:-NONE}"

if [ -z "$relay_ts_ip" ] || [ -z "$client_ts_ip" ]; then
  log "ERROR: Failed to get Tailscale IPs"
  ssh_relay "tailscale status 2>&1" | head -10
  ssh_client "tailscale status 2>&1" | head -10
  exit 1
fi

# ---- Step 4: Test Q1 — InsecureForTests ----------------------

log ""
log "Step 4: Test InsecureForTests behavior"
log "  (Does the client accept non-TLS DERP?)"
# We set InsecureForTests=true in the DERP map.
# Check if connections work.
relay_status=$(ssh_relay "tailscale status 2>/dev/null")
client_status=$(ssh_client "tailscale status 2>/dev/null")
log "  Relay status:"
echo "$relay_status" | head -5
log "  Client status:"
echo "$client_status" | head -5

# ---- Step 5: Test Q2 — netcheck, verify our DERP only -------

log ""
log "Step 5: netcheck — verify custom DERP map"
log "  (Does OmitDefaultRegions work?)"

# Apply DERP map via tailscale set.
ssh_relay "sudo tailscale set --custom-derp-map=/etc/tailscale/derp-map.json 2>/dev/null" || true
ssh_client "sudo tailscale set --custom-derp-map=/etc/tailscale/derp-map.json 2>/dev/null" || true
sleep 2

relay_netcheck=$(ssh_relay "tailscale netcheck 2>&1")
log "  Relay netcheck:"
echo "$relay_netcheck" | grep -E "DERP|region|latency" | head -10

client_netcheck=$(ssh_client "tailscale netcheck 2>&1")
log "  Client netcheck:"
echo "$client_netcheck" | grep -E "DERP|region|latency" | head -10

# ---- Step 6: Block direct UDP, force DERP relay path ---------

log ""
log "Step 6: Block direct UDP between VMs"

# Block direct WireGuard UDP between client VMs.
# The relay VM also runs Tailscale, so block UDP between
# the two Tailscale nodes to force DERP.
ssh_relay "
sudo iptables -D OUTPUT -d ${CLIENT_INT} -p udp -j DROP 2>/dev/null
sudo iptables -D INPUT -s ${CLIENT_INT} -p udp -j DROP 2>/dev/null
sudo iptables -A OUTPUT -d ${CLIENT_INT} -p udp -j DROP
sudo iptables -A INPUT -s ${CLIENT_INT} -p udp -j DROP
"
ssh_client "
sudo iptables -D OUTPUT -d ${RELAY_INT} -p udp -j DROP 2>/dev/null
sudo iptables -D INPUT -s ${RELAY_INT} -p udp -j DROP 2>/dev/null
sudo iptables -A OUTPUT -d ${RELAY_INT} -p udp -j DROP
sudo iptables -A INPUT -s ${RELAY_INT} -p udp -j DROP
"
log "  UDP blocked between ${RELAY_INT} and ${CLIENT_INT}"
sleep 5

# Check status — should show relay, not direct.
relay_status2=$(ssh_relay "tailscale status 2>/dev/null")
client_status2=$(ssh_client "tailscale status 2>/dev/null")
log "  Relay status (after UDP block):"
echo "$relay_status2" | head -5
log "  Client status (after UDP block):"
echo "$client_status2" | head -5

# ---- Step 7: Basic connectivity test -------------------------

log ""
log "Step 7: Basic connectivity through tunnel"

# Ping through Tailscale tunnel.
log "  Pinging client from relay (${client_ts_ip})..."
ping_result=$(ssh_relay "ping -c 5 -W 3 ${client_ts_ip} 2>&1")
echo "$ping_result" | tail -3

log "  Pinging relay from client (${relay_ts_ip})..."
ping_result2=$(ssh_client "ping -c 5 -W 3 ${relay_ts_ip} 2>&1")
echo "$ping_result2" | tail -3

# ---- Step 8: iperf3 throughput test --------------------------

log ""
log "Step 8: iperf3 throughput through tunnel"

# Start iperf3 server on relay.
ssh_relay "pkill iperf3 2>/dev/null; sleep 1"
ssh_relay "setsid iperf3 -s -D 2>/dev/null"
sleep 1

# TCP single stream, 15 seconds.
log "  TCP single stream (15s)..."
tcp1=$(ssh_client "iperf3 -c ${relay_ts_ip} -t 15 --json 2>/dev/null")
tcp1_bps=$(echo "$tcp1" | python3 -c "
import json, sys
try:
  d = json.load(sys.stdin)
  bps = d['end']['sum_received']['bits_per_second']
  print(f'{bps/1e6:.0f} Mbps')
except:
  print('FAILED')
" 2>/dev/null)
log "    Result: ${tcp1_bps}"

# TCP 4 parallel streams.
log "  TCP 4 streams (15s)..."
tcp4=$(ssh_client "iperf3 -c ${relay_ts_ip} -t 15 -P 4 --json 2>/dev/null")
tcp4_bps=$(echo "$tcp4" | python3 -c "
import json, sys
try:
  d = json.load(sys.stdin)
  bps = d['end']['sum_received']['bits_per_second']
  print(f'{bps/1e6:.0f} Mbps')
except:
  print('FAILED')
" 2>/dev/null)
log "    Result: ${tcp4_bps}"

# UDP unlimited.
log "  UDP unlimited (15s)..."
udp=$(ssh_client "iperf3 -c ${relay_ts_ip} -u -b 0 -t 15 --json 2>/dev/null")
udp_bps=$(echo "$udp" | python3 -c "
import json, sys
try:
  d = json.load(sys.stdin)
  bps = d['end']['sum']['bits_per_second']
  lost = d['end']['sum']['lost_percent']
  print(f'{bps/1e6:.0f} Mbps, {lost:.1f}% loss')
except:
  print('FAILED')
" 2>/dev/null)
log "    Result: ${udp_bps}"

ssh_relay "pkill iperf3 2>/dev/null"

# ---- Step 9: Latency test -----------------------------------

log ""
log "Step 9: Latency through tunnel"

ping_full=$(ssh_client "ping -c 100 -i 0.01 ${relay_ts_ip} 2>&1")
ping_summary=$(echo "$ping_full" | tail -2)
log "  100 pings @ 10ms interval:"
echo "  $ping_summary"

# ---- Step 10: Client resource usage --------------------------

log ""
log "Step 10: Client resource usage"

relay_ps=$(ssh_relay "ps -C tailscaled -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null")
client_ps=$(ssh_client "ps -C tailscaled -o pid,rss,vsz,%cpu,%mem --no-headers 2>/dev/null")
log "  Relay tailscaled: ${relay_ps}"
log "  Client tailscaled: ${client_ps}"

# ---- Step 11: Check derper logs for relay traffic ------------

log ""
log "Step 11: Verify traffic went through DERP relay"

derper_log=$(ssh_relay "tail -20 /tmp/derper.log 2>/dev/null")
log "  Derper log (last 20 lines):"
echo "$derper_log" | head -20

# ---- Cleanup -------------------------------------------------

log ""
log "Step 12: Cleanup"

# Remove iptables rules.
ssh_relay "
sudo iptables -D OUTPUT -d ${CLIENT_INT} -p udp -j DROP 2>/dev/null
sudo iptables -D INPUT -s ${CLIENT_INT} -p udp -j DROP 2>/dev/null
"
ssh_client "
sudo iptables -D OUTPUT -d ${RELAY_INT} -p udp -j DROP 2>/dev/null
sudo iptables -D INPUT -s ${RELAY_INT} -p udp -j DROP 2>/dev/null
"
log "  iptables rules removed"

# Leave Tailscale up for now (user can tailscale down manually).
# Kill derper.
ssh_relay "sudo pkill -9 derper 2>/dev/null"
log "  derper stopped"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  PHASE 0 SUMMARY"
log "========================================="
log ""
log "Q1: InsecureForTests    — check output above"
log "Q2: OmitDefaultRegions  — check netcheck output"
log "Q3: Auth key flow       — ${relay_up:-check output}"
log "Q4: Relay path forced   — check status output"
log "Q5: Basic connectivity  — ping/iperf results above"
log "Q6: Client overhead     — ${relay_ps}"
log ""
log "Tailscale IPs:"
log "  Relay:  ${relay_ts_ip}"
log "  Client: ${client_ts_ip}"
log ""
log "Throughput through tunnel:"
log "  TCP 1-stream: ${tcp1_bps}"
log "  TCP 4-stream: ${tcp4_bps}"
log "  UDP:          ${udp_bps}"
log ""
log "Phase 0 complete. Review output to answer open questions."
