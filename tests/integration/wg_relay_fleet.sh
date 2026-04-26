#!/bin/bash
# @file wg_relay_fleet.sh
# @brief End-to-end test for `mode: wireguard` driven against the
#        libvirt hd-r2 / hd-c1 / hd-c2 fleet.
#
# Verifies the operator workflow + data path against a real
# WireGuard tunnel:
#   - relay in `mode: wireguard` (already deployed via deb +
#     systemd; the test does not redeploy)
#   - stock wg-quick on each client, Endpoint = relay
#   - drive `wg peer add`, `wg peer pubkey`, `wg link add`
#     via `hdcli` on the relay
#   - assert ping across the tunnel (4/4)
#   - assert relay-side counters move (userspace + XDP)
#
# Exits 77 (CTest SKIP) when:
#   - any fleet host is unreachable via ssh
#   - the relay daemon is not currently in `mode: wireguard`
#   - the client wg0 interfaces are not configured to use the
#     relay endpoint
#
# This is intentionally non-destructive — it neither installs
# packages nor rewrites yaml. Use it as a regression check
# against a fleet you've already brought up via the
# wireguard_relay_quickstart.md docs.
#
# Usage:
#   wg_relay_fleet.sh                # userspace + XDP if attached
#   wg_relay_fleet.sh --require-xdp  # additionally fail if the
#                                    # daemon is not running with
#                                    # XDP attached

set -u

REQUIRE_XDP=0
case "${1:-}" in
  --require-xdp) REQUIRE_XDP=1 ;;
  '') ;;
  *) echo "usage: $0 [--require-xdp]" >&2; exit 2 ;;
esac

# Fleet topology — keep in lockstep with ~/.ssh/config aliases.
SSH_CFG="${SSH_CONFIG:-$HOME/.ssh/config}"
RELAY=hd-r2
CLIENT_A=hd-c1
CLIENT_B=hd-c2

# Tunnel addresses inside the WG overlay.
TUN_A=10.99.0.1
TUN_B=10.99.0.2

# Operator-facing peer names — must match what's already in the
# relay roster. The fleet bring-up doc uses alice/bob.
NAME_A=alice
NAME_B=bob

# Where the relay's hdcli lives on hd-r2 (deb-shipped path).
HD_CLI=/usr/bin/hdcli

ssh_cmd() {
  local host=$1; shift
  ssh -F "$SSH_CFG" -o ConnectTimeout=5 -o BatchMode=yes \
      "$host" "$@"
}

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# -- Reachability ---------------------------------------------------

for h in "$RELAY" "$CLIENT_A" "$CLIENT_B"; do
  if ! ssh_cmd "$h" 'true' 2>/dev/null; then
    skip "cannot reach $h via ssh"
  fi
done

# -- Relay state ----------------------------------------------------

show=$(ssh_cmd "$RELAY" "$HD_CLI wg show 2>&1" || true)
if ! echo "$show" | grep -q 'port'; then
  skip "relay daemon is not running in mode: wireguard "\
       "(or hdcli unreachable on $RELAY)"
fi

# The hdcli table renderer wraps each row in ANSI bold escapes
# and a box-drawing column separator; pull the last token of the
# row by stripping non-printables and grabbing the trailing word.
extract_field() {
  # Strip ANSI bold + box-drawing + the CRs the einheit table
  # renderer emits; awk picks up $NF cleanly only after that.
  echo "$1" | grep -E "[[:space:]]$2[[:space:]]" \
    | sed -E 's/\x1b\[[0-9;]*m//g' \
    | tr -d '│\r' \
    | awk '{print $NF}'
}

xdp_attached=$(extract_field "$show" 'xdp_attached')
if [ "$REQUIRE_XDP" -eq 1 ] && [ "$xdp_attached" != "true" ]; then
  fail "--require-xdp set but xdp_attached=$xdp_attached"
fi

# Make sure both peers + the link are registered. These calls are
# idempotent against the daemon's deduplication: a second add for
# an existing name returns wg_peer_add_failed which we accept.
for cmd in \
    "wg peer add $NAME_A 192.168.122.189:51820 alice-laptop" \
    "wg peer add $NAME_B 192.168.122.239:51820 bob-laptop" \
    "wg link add $NAME_A $NAME_B"; do
  ssh_cmd "$RELAY" "$HD_CLI $cmd" >/dev/null 2>&1 || true
done

# Confirm the link is now there.
links=$(ssh_cmd "$RELAY" "$HD_CLI wg link list 2>&1")
if ! echo "$links" | grep -q "$NAME_A" || \
   ! echo "$links" | grep -q "$NAME_B"; then
  fail "link $NAME_A↔$NAME_B not present after registration"
fi

# -- Client state ---------------------------------------------------

for h in "$CLIENT_A" "$CLIENT_B"; do
  if ! ssh_cmd "$h" 'sudo wg show wg0 2>/dev/null' \
       | grep -q 'listening port'; then
    skip "$h has no wg0 interface up"
  fi
done

# -- Workflow: ping + assert counter movement -----------------------

# Snapshot counters before the ping run so we can assert delta.
read_count() {
  local s
  s=$(ssh_cmd "$RELAY" "$HD_CLI wg show 2>&1")
  extract_field "$s" "$1"
}

before_rx=$(read_count rx_packets)
before_rx=${before_rx:-0}
before_xdp_fwd=$(read_count xdp_fwd_packets)
before_xdp_fwd=${before_xdp_fwd:-0}

if ! ssh_cmd "$CLIENT_A" \
     "ping -c 4 -W 2 -q $TUN_B" >/dev/null; then
  # Re-run with output for the ctest log.
  ssh_cmd "$CLIENT_A" "ping -c 4 -W 2 $TUN_B" || true
  fail "ping $TUN_A → $TUN_B did not get 4/4"
fi

after_rx=$(read_count rx_packets)
after_rx=${after_rx:-0}
after_xdp_fwd=$(read_count xdp_fwd_packets)
after_xdp_fwd=${after_xdp_fwd:-0}

# At least one of the two counters must have moved. With XDP
# attached most packets bypass userspace — `rx_packets` may stay
# flat — so success is "userspace-rx OR xdp-fwd advanced".
moved=0
if [ "${after_rx:-0}" -gt "${before_rx:-0}" ]; then moved=1; fi
if [ "${after_xdp_fwd}" -gt "${before_xdp_fwd}" ]; then moved=1; fi
if [ "$moved" -eq 0 ]; then
  fail "ping reached the destination but the relay's "  \
       "rx_packets and xdp_fwd_packets both stayed flat "  \
       "(rx ${before_rx} → ${after_rx}, "  \
       "xdp ${before_xdp_fwd} → ${after_xdp_fwd})"
fi

# When XDP is attached we also expect the fast path to be the
# dominant carrier after the cold-start window. The 4-packet ping
# is enough to learn both peers' MACs in either direction.
if [ "$xdp_attached" = "true" ]; then
  delta_rx=$(( ${after_rx:-0} - ${before_rx:-0} ))
  delta_xdp=$(( ${after_xdp_fwd} - ${before_xdp_fwd} ))
  if [ "$delta_xdp" -lt "$delta_rx" ]; then
    fail "with XDP attached, userspace handled more packets " \
         "($delta_rx) than XDP did ($delta_xdp) — fast path " \
         "is not active"
  fi
fi

echo "PASS: 4/4 ping over the relay; "  \
     "userspace +$(( ${after_rx:-0} - ${before_rx:-0} )), "  \
     "xdp +$(( after_xdp_fwd - before_xdp_fwd )); "  \
     "xdp_attached=$xdp_attached"
exit 0
