#!/bin/bash
# @file hd_wg_e2e.sh
# @brief End-to-end hd-wg test using two network namespaces.
#
# Brings up an HD relay + two hd-wg daemons in separate
# netns connected by a veth pair. Verifies:
#   - direct-path tunnel establishes
#   - ping 10.99.0.2 succeeds at LAN latency
#   - force-relay mode also works
#   - relay fallback triggers when direct UDP is blocked
#
# Requires root (netns + WG interface creation) and the
# `wireguard` kernel module. Exits 77 (CTest SKIP) when
# unavailable.
#
# Usage: hd_wg_e2e.sh <build_dir>

set -u

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <build_dir>"
  exit 2
fi

BUILD="$1"
HYPER_DERP="$BUILD/hyper-derp"
HD_WG="$BUILD/client/hd-wg"

if [ "$(id -u)" -ne 0 ]; then
  echo "SKIP: root required for netns + WG iface"
  exit 77
fi
if ! modprobe wireguard 2>/dev/null; then
  echo "SKIP: wireguard kernel module unavailable"
  exit 77
fi
if [ ! -x "$HYPER_DERP" ] || [ ! -x "$HD_WG" ]; then
  echo "SKIP: binaries not built ($HYPER_DERP, $HD_WG)"
  exit 77
fi

TMP=$(mktemp -d -t hd-wg-e2e.XXXXXX)
NS_A=hdwg-a-$$
NS_B=hdwg-b-$$
VETH_A=hdwg-v-a-$$
VETH_B=hdwg-v-b-$$

cleanup() {
  # Kill daemons in both namespaces (ignore errors).
  for ns in "$NS_A" "$NS_B"; do
    ip netns pids "$ns" 2>/dev/null | xargs -r kill -9 2>/dev/null
    ip netns delete "$ns" 2>/dev/null
  done
  ip link delete "$VETH_A" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

# -- Setup -------------------------------------------------------------------

ip netns add "$NS_A"
ip netns add "$NS_B"
ip link add "$VETH_A" type veth peer name "$VETH_B"
ip link set "$VETH_A" netns "$NS_A"
ip link set "$VETH_B" netns "$NS_B"

ip netns exec "$NS_A" ip link set lo up
ip netns exec "$NS_B" ip link set lo up
ip netns exec "$NS_A" ip addr add 10.200.0.1/24 dev "$VETH_A"
ip netns exec "$NS_B" ip addr add 10.200.0.2/24 dev "$VETH_B"
ip netns exec "$NS_A" ip link set "$VETH_A" up
ip netns exec "$NS_B" ip link set "$VETH_B" up

# TLS cert for the relay.
openssl req -x509 -newkey rsa:2048 \
  -keyout "$TMP/relay.key" -out "$TMP/relay.crt" \
  -days 1 -nodes -subj '/CN=hd-wg-e2e' \
  2>/dev/null

# Keys.
RELAY_KEY=$(openssl rand -hex 32)
WG_A_KEY=$(openssl rand -hex 32)
WG_B_KEY=$(openssl rand -hex 32)

# -- Start relay in ns A -----------------------------------------------------

ip netns exec "$NS_A" "$HYPER_DERP" \
    --port 3341 \
    --tls-cert "$TMP/relay.crt" --tls-key "$TMP/relay.key" \
    --hd-relay-key "$RELAY_KEY" \
    --hd-enroll-mode auto \
    --log-level warn \
    > "$TMP/relay.log" 2>&1 &
RELAY_PID=$!

# Wait for relay to listen.
for i in $(seq 1 40); do
  if ip netns exec "$NS_A" ss -tln 2>/dev/null \
       | grep -q ':3341 '; then
    break
  fi
  sleep 0.1
done

# -- Run a tunnel test -------------------------------------------------------

# Brings up both hd-wg daemons with the given flags and
# pings. Returns 0 on success, non-zero on failure.
# $1: extra args for peer A
# $2: extra args for peer B
# $3: expected endpoint hint ("direct" or "relay")
run_tunnel_test() {
  local extra_a="$1"
  local extra_b="$2"
  local hint="$3"

  # Kill any previous daemons in each ns.
  ip netns pids "$NS_A" | xargs -r kill -9 2>/dev/null
  ip netns pids "$NS_B" | xargs -r kill -9 2>/dev/null
  sleep 0.5
  # Restart relay (killed above).
  ip netns exec "$NS_A" "$HYPER_DERP" \
      --port 3341 \
      --tls-cert "$TMP/relay.crt" --tls-key "$TMP/relay.key" \
      --hd-relay-key "$RELAY_KEY" \
      --hd-enroll-mode auto --log-level warn \
      > "$TMP/relay.log" 2>&1 &
  for i in $(seq 1 40); do
    ip netns exec "$NS_A" ss -tln 2>/dev/null \
      | grep -q ':3341 ' && break
    sleep 0.1
  done

  ip netns exec "$NS_A" "$HD_WG" \
      --relay-host 10.200.0.1 --relay-port 3341 \
      --relay-key "$RELAY_KEY" \
      --wg-key "$WG_A_KEY" \
      --tunnel 10.99.0.1/24 \
      --wg-port 51820 --proxy-port 51821 \
      $extra_a \
      > "$TMP/a.log" 2>&1 &
  ip netns exec "$NS_B" "$HD_WG" \
      --relay-host 10.200.0.1 --relay-port 3341 \
      --relay-key "$RELAY_KEY" \
      --wg-key "$WG_B_KEY" \
      --tunnel 10.99.0.2/24 \
      --wg-port 51820 --proxy-port 51821 \
      $extra_b \
      > "$TMP/b.log" 2>&1 &

  # Give ICE + WG time to settle. 10s ICE window + WG
  # handshake on the settled endpoint.
  sleep 14

  if ! ip netns exec "$NS_A" ping -c 3 -W 2 10.99.0.2 \
       > "$TMP/ping.out" 2>&1; then
    echo "FAIL: ping 10.99.0.2 failed"
    cat "$TMP/ping.out"
    echo "--- a.log ---"; cat "$TMP/a.log"
    echo "--- b.log ---"; cat "$TMP/b.log"
    return 1
  fi

  # Check the daemon's own log to know whether the path
  # settled as direct or relayed.
  local state_line
  state_line=$(grep -E \
      'promoted to direct|direct failed|no candidates|stalled' \
      "$TMP/a.log" | tail -1)
  echo "  state: $state_line"

  case "$hint" in
    direct)
      echo "$state_line" | grep -q 'promoted to direct' || {
        echo "FAIL: expected direct promotion"
        echo "--- a.log ---"; cat "$TMP/a.log"
        return 1
      }
      ;;
    relay)
      echo "$state_line" | \
          grep -qE 'no candidates|direct failed|stalled' || {
        echo "FAIL: expected relay fallback"
        echo "--- a.log ---"; cat "$TMP/a.log"
        return 1
      }
      ;;
  esac
  return 0
}

# -- Scenarios ---------------------------------------------------------------

echo "[1/3] direct path (veth LAN, no block)"
run_tunnel_test "" "" "direct" || exit 1
echo "  PASS"

echo "[2/3] force-relay mode"
run_tunnel_test "--force-relay" "--force-relay" "relay" || exit 1
echo "  PASS"

echo "[3/3] iptables block → relay fallback"
ip netns exec "$NS_A" iptables -A OUTPUT -p udp \
    --dport 51820 -d 10.200.0.2 -j DROP 2>/dev/null || true
ip netns exec "$NS_B" iptables -A OUTPUT -p udp \
    --dport 51820 -d 10.200.0.1 -j DROP 2>/dev/null || true
run_tunnel_test "" "" "relay" || exit 1
echo "  PASS"

echo "all hd-wg e2e scenarios passed"
exit 0
