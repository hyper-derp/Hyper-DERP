#!/bin/bash
# HD Protocol VM test — Level 0 DERP + Level 1 HD.
#
# bench-relay (10.101.1.1) — hyper-derp relay
# bench-client (10.101.1.2) — test clients
#
# Usage: ./tools/test_hd_vm.sh

set -uo pipefail

KEY="$HOME/.ssh/id_ed25519_targets"
O="-o StrictHostKeyChecking=no -o BatchMode=yes"
R=worker@10.101.1.1
C=worker@10.101.1.2
RIP=10.101.1.1
RPORT=3341
MPORT=9191
PASS=0
FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
rcmd() { ssh -i "$KEY" $O "$R" "$@"; }
ccmd() { ssh -i "$KEY" $O "$C" "$@"; }

RELAY_KEY=$(head -c 32 /dev/urandom | xxd -p -c 64)

rcmd "pkill -9 -f hyper-derp || true" 2>/dev/null
ccmd "pkill -9 -f 'test-client' || true" 2>/dev/null
sleep 1

# ---- 1. Start relay ----
echo "=== 1. Start relay ==="
rcmd "nohup ./hyper-derp \
  --port $RPORT --workers 2 \
  --metrics-port $MPORT --debug-endpoints \
  --hd-relay-key $RELAY_KEY --hd-enroll-mode auto \
  > /tmp/relay.log 2>&1 &"
sleep 2

H=$(rcmd "curl -sf http://localhost:$MPORT/health")
if echo "$H" | grep -q '"ok"'; then
  ok "relay started"
else
  fail "relay started"
  echo "$H"
  exit 1
fi

RS=$(rcmd "curl -sf http://localhost:$MPORT/api/v1/relay")
if echo "$RS" | grep -q '"hd_enabled":true'; then
  ok "HD protocol enabled"
else
  fail "HD protocol enabled"
fi

# ---- 2. DERP send/recv (Level 0) ----
echo
echo "=== 2. DERP send/recv (Level 0) ==="
ccmd "cat > /tmp/derp_test.sh << 'SCRIPT'
#!/bin/bash
./derp-test-client \
  --host $RIP --port $RPORT \
  --mode recv --count 5 --size 1400 \
  --timeout 15000 > /tmp/recv.out 2>&1 &
RPID=\$!
sleep 2
RKEY=\$(grep '^pubkey:' /tmp/recv.out | awk '{print \$2}')
./derp-test-client \
  --host $RIP --port $RPORT \
  --mode send --count 5 --size 1400 \
  --dst-key \$RKEY > /tmp/send.out 2>&1
wait \$RPID 2>/dev/null
cat /tmp/recv.out
SCRIPT
chmod +x /tmp/derp_test.sh" 2>/dev/null

DERP_OUT=$(ccmd "timeout 30 /tmp/derp_test.sh" 2>&1)
if echo "$DERP_OUT" | grep -q "Received 5 packets"; then
  ok "DERP send/recv (5 x 1400B)"
else
  fail "DERP send/recv"
  echo "$DERP_OUT" | tail -5
fi

# ---- 3. HD send/recv (Level 1) ----
echo
echo "=== 3. HD send/recv (Level 1) ==="
ccmd "cat > /tmp/hd_test.sh << 'SCRIPT'
#!/bin/bash
./hd-test-client \
  --host $RIP --port $RPORT \
  --relay-key $RELAY_KEY \
  --mode recv --count 5 --size 1400 \
  --timeout 15000 > /tmp/hd_recv.out 2>&1 &
RPID=\$!
sleep 2
./hd-test-client \
  --host $RIP --port $RPORT \
  --relay-key $RELAY_KEY \
  --mode send --count 5 --size 1400 \
  > /tmp/hd_send.out 2>&1
wait \$RPID 2>/dev/null
echo 'RECV:'
cat /tmp/hd_recv.out
echo 'SEND:'
cat /tmp/hd_send.out
SCRIPT
chmod +x /tmp/hd_test.sh" 2>/dev/null

HD_OUT=$(ccmd "timeout 30 /tmp/hd_test.sh" 2>&1)
echo "$HD_OUT" | tail -10

if echo "$HD_OUT" | grep -q "Received 5 packets"; then
  ok "HD send/recv (5 x 1400B)"
elif echo "$HD_OUT" | grep -q "Sent 5 packets"; then
  # Send worked, recv may have timed out.
  fail "HD recv (send ok, recv timeout)"
else
  fail "HD send/recv"
fi

# ---- 4. HD echo/ping (Level 1) ----
echo
echo "=== 4. HD echo + ping ==="
ccmd "cat > /tmp/hd_ping.sh << 'SCRIPT'
#!/bin/bash
./hd-test-client \
  --host $RIP --port $RPORT \
  --relay-key $RELAY_KEY \
  --mode echo --count 100 \
  > /tmp/hd_echo.out 2>&1 &
EPID=\$!
sleep 2
./hd-test-client \
  --host $RIP --port $RPORT \
  --relay-key $RELAY_KEY \
  --mode ping --count 10 --size 64 \
  --timeout 5000 \
  > /tmp/hd_ping.out 2>&1
kill \$EPID 2>/dev/null
wait \$EPID 2>/dev/null
echo 'PING:'
cat /tmp/hd_ping.out
echo 'ECHO:'
cat /tmp/hd_echo.out
SCRIPT
chmod +x /tmp/hd_ping.sh" 2>/dev/null

PING_OUT=$(ccmd "timeout 30 /tmp/hd_ping.sh" 2>&1)
echo "$PING_OUT" | grep -E "Ping:|RTT|Echoed" | head -5

if echo "$PING_OUT" | grep -q "completed"; then
  ok "HD ping/echo"
else
  fail "HD ping/echo"
fi

# ---- 5. Metrics after all traffic ----
echo
echo "=== 5. Metrics ==="
M=$(rcmd "curl -sf http://localhost:$MPORT/metrics" \
  | grep -v "^#" \
  | grep -E "recv_bytes|send_bytes|peers_active")
echo "$M"

RECV=$(echo "$M" | grep recv_bytes | awk '{print $2}')
if [ "${RECV:-0}" -gt 0 ]; then
  ok "metrics show traffic (${RECV}B)"
else
  fail "metrics show no traffic"
fi

# ---- 6. HD peer list ----
echo
echo "=== 6. REST API — HD peers ==="
PEERS=$(rcmd "curl -sf http://localhost:$MPORT/api/v1/peers")
echo "$PEERS" | python3 -m json.tool 2>/dev/null || echo "$PEERS"

# ---- 7. Worker stats ----
echo
echo "=== 7. Workers ==="
rcmd "curl -sf http://localhost:$MPORT/debug/workers" \
  | python3 -c "
import sys,json; d=json.load(sys.stdin)
for w in d['workers']:
  print(f'  w{w[\"id\"]}: {w[\"peers\"]}p '\
        f'{w[\"recv_bytes\"]}B recv '\
        f'{w[\"send_bytes\"]}B send')
" 2>&1 || echo "  (parse failed)"

# ---- Cleanup ----
echo
echo "=== Cleanup ==="
rcmd "pkill -f hyper-derp || true" 2>/dev/null
ccmd "pkill -f 'test-client' || true" 2>/dev/null

echo
echo "=== Relay log (last 15 lines) ==="
rcmd "tail -15 /tmp/relay.log"

echo
echo "==============================="
echo "Results: $PASS passed, $FAIL failed"
echo "==============================="
exit $FAIL
