#!/bin/bash
# HD vs DERP throughput comparison on bare metal 25GbE.
#
# Raptor Lake (client) → ConnectX-4 25GbE → Haswell (relay)
#
# Runs DERP scale test, then HD with forwarding rules via
# REST API.

set -uo pipefail

KEY="$HOME/.ssh/id_ed25519_targets"
O="-o StrictHostKeyChecking=no -o BatchMode=yes"
HASWELL=worker@192.168.0.134
RIP=10.50.0.2
RPORT=3341
MPORT=9191
WORKERS=4
MSG=1400
DUR=15
RK="aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888"

rcmd() { ssh -i "$KEY" $O "$HASWELL" "$@"; }

# ---- Start relay ----
rcmd "pkill -9 -f hyper-derp 2>/dev/null; sleep 1"
rcmd "cat > /tmp/start.sh << 'EOF'
#!/bin/bash
./hyper-derp --port 3341 --workers 4 \
  --metrics-port 9191 --debug-endpoints \
  --hd-relay-key aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888 \
  --hd-enroll-mode auto > /tmp/relay.log 2>&1 &
disown
echo \$!
EOF
chmod +x /tmp/start.sh
bash /tmp/start.sh"
sleep 3
rcmd "pgrep -c hyper-derp" || { echo "relay failed"; exit 1; }
echo "Relay running (${WORKERS}w, plain TCP, HD enabled)"
echo

# ============================================================
# DERP BENCHMARK
# ============================================================
echo "###################################################"
echo "# DERP Throughput (plain TCP, ${WORKERS}w)"
echo "###################################################"
echo

for RATE in 3000 5000 7500 10000; do
  echo "--- DERP @ ${RATE} Mbps ---"
  ./build/tools/bench/derp-scale-test \
    --host $RIP --port $RPORT \
    --peers 20 --active-pairs 10 \
    --msg-size $MSG --duration $DUR \
    --rate-mbps $RATE 2>&1 \
    | grep -E "Throughput|loss|Messages"
  sleep 2
done

echo
echo "=== DERP worker stats ==="
rcmd "curl -sf http://localhost:$MPORT/debug/workers" 2>/dev/null \
  | python3 -c "
import sys,json; d=json.load(sys.stdin)
for w in d['workers']:
  print(f'  w{w[\"id\"]}: {w[\"recv_bytes\"]/1e9:.2f}GB recv '\
        f'{w[\"send_bytes\"]/1e9:.2f}GB send')
" 2>/dev/null || echo "  (metrics unavailable)"

# Reset relay for HD test.
rcmd "pkill -9 -f hyper-derp 2>/dev/null; sleep 1; bash /tmp/start.sh" 2>/dev/null
sleep 3

echo
echo "###################################################"
echo "# HD Throughput (plain TCP, ${WORKERS}w)"
echo "###################################################"
echo

# HD test: N sender/receiver pairs.
# 1. Connect all peers (they auto-approve).
# 2. Set forwarding rules via REST API.
# 3. Run traffic.

PAIRS=10
TOTAL=$((PAIRS * 2))

echo "Connecting $TOTAL HD peers..."

# Start receivers first.
RPIDS=()
RKEYS=()
for i in $(seq 0 $((PAIRS-1))); do
  OUT="/tmp/hd_recv_${i}"
  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RK \
    --mode recv --count 999999 --size $MSG \
    --timeout $((DUR * 1000 + 10000)) \
    > "${OUT}.out" 2>"${OUT}.err" &
  RPIDS+=($!)
done
sleep 3

# Collect receiver pubkeys.
for i in $(seq 0 $((PAIRS-1))); do
  RKEYS+=($(grep "^pubkey:" "/tmp/hd_recv_${i}.err" \
    | awk '{print $2}'))
done

# Start senders.
SPIDS=()
SKEYS=()
for i in $(seq 0 $((PAIRS-1))); do
  OUT="/tmp/hd_send_${i}"
  ./build/tools/bench/hd-test-client \
    --host $RIP --port $RPORT \
    --relay-key $RK \
    --mode send --count 999999 --size $MSG \
    --timeout $((DUR * 1000)) \
    > "${OUT}.out" 2>"${OUT}.err" &
  SPIDS+=($!)
done
sleep 2

# Collect sender pubkeys.
for i in $(seq 0 $((PAIRS-1))); do
  SKEYS+=($(grep "^pubkey:" "/tmp/hd_send_${i}.err" \
    | awk '{print $2}'))
done

# Set forwarding rules: sender[i] → receiver[i].
echo "Setting $PAIRS forwarding rules..."
for i in $(seq 0 $((PAIRS-1))); do
  SK="${SKEYS[$i]:-}"
  RKY="${RKEYS[$i]:-}"
  if [ -n "$SK" ] && [ -n "$RKY" ]; then
    curl -sf -X POST \
      "http://${RIP}:${MPORT}/api/v1/peers/${SK}/rules" \
      -H "Content-Type: application/json" \
      -d "{\"dst_key\":\"${RKY}\"}" > /dev/null 2>&1
  fi
done

echo "Rules set. Waiting ${DUR}s for traffic..."
sleep $((DUR + 5))

# Kill senders (they may still be sending).
for pid in "${SPIDS[@]}"; do
  kill $pid 2>/dev/null
done
sleep 2
# Kill receivers.
for pid in "${RPIDS[@]}"; do
  kill $pid 2>/dev/null
done
wait 2>/dev/null

# Collect results.
echo
echo "--- HD Results ---"
TOTAL_SENT=0
TOTAL_RECV=0
for i in $(seq 0 $((PAIRS-1))); do
  SENT=$(grep "Sent" "/tmp/hd_send_${i}.err" 2>/dev/null \
    | grep -oP '\d+ packets' | grep -oP '\d+' || echo 0)
  RECV=$(grep "Received" "/tmp/hd_recv_${i}.err" 2>/dev/null \
    | grep -oP '\d+ packets' | grep -oP '\d+' || echo 0)
  TOTAL_SENT=$((TOTAL_SENT + ${SENT:-0}))
  TOTAL_RECV=$((TOTAL_RECV + ${RECV:-0}))
done

LOSS=0
if [ $TOTAL_SENT -gt 0 ]; then
  LOSS=$(python3 -c "print(f'{100*(1-$TOTAL_RECV/$TOTAL_SENT):.2f}')")
fi
MBPS=$(python3 -c "print(f'{$TOTAL_RECV*$MSG*8/1e6/$DUR:.1f}')")

echo "Messages sent: $TOTAL_SENT"
echo "Messages recv: $TOTAL_RECV"
echo "Message loss:  ${LOSS}%"
echo "Throughput:    ${MBPS} Mbps (recv)"

echo
echo "=== HD worker stats ==="
rcmd "curl -sf http://localhost:$MPORT/debug/workers" 2>/dev/null \
  | python3 -c "
import sys,json; d=json.load(sys.stdin)
for w in d['workers']:
  print(f'  w{w[\"id\"]}: {w[\"recv_bytes\"]/1e9:.2f}GB recv '\
        f'{w[\"send_bytes\"]/1e9:.2f}GB send {w[\"peers\"]}p')
" 2>/dev/null || echo "  (metrics unavailable)"

echo
echo "=== HD peers ==="
rcmd "curl -sf http://localhost:$MPORT/api/v1/peers" 2>/dev/null \
  | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  {d[\"count\"]} peers')
for p in d['peers'][:3]:
  print(f'  {p[\"key\"][:16]}... state={p[\"state\"]} rules={p[\"rule_count\"]}')
if d['count']>3: print(f'  ... and {d[\"count\"]-3} more')
" 2>/dev/null || echo "  (unavailable)"

# Cleanup.
rcmd "pkill -f hyper-derp" 2>/dev/null
rm -f /tmp/hd_recv_* /tmp/hd_send_*

echo
echo "Done."
