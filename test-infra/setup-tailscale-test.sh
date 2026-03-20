#!/bin/bash
# Sets up a Tailscale integration test environment:
#   - Headscale (coordination server) on the host
#   - Two Debian VMs with Tailscale clients
#   - Hyper-DERP relay on the host
#
# Network: virbr-targets 10.101.0.0/20
#   Host:  10.101.0.1 (headscale :8080, hyper-derp :3340)
#   VM ts-client-1: 10.101.1.10
#   VM ts-client-2: 10.101.1.11
#
# Usage: sudo ./setup-tailscale-test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
# Resolve the invoking user's home (sudo changes $HOME).
REAL_HOME="${REAL_HOME:-$(getent passwd "${SUDO_USER:-$USER}" \
  | cut -d: -f6)}"
SSH_KEY="${SSH_KEY:-${REAL_HOME}/.ssh/id_ed25519}"
BRIDGE="virbr-targets"
HOST_IP="10.101.0.1"
BASE_IMG="/var/lib/libvirt/images/deb-01.qcow2"
IMG_DIR="/var/lib/libvirt/images"

VM1_NAME="ts-client-1"
VM1_IP="10.101.1.10"
VM1_MAC="52:54:00:cc:01:10"

VM2_NAME="ts-client-2"
VM2_IP="10.101.1.11"
VM2_MAC="52:54:00:cc:01:11"

# -- Helpers -----------------------------------------------------------

log() { echo "=== $* ===" >&2; }

ssh_vm() {
  local ip="$1"; shift
  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=5 -i "$SSH_KEY" "worker@${ip}" "$@"
}

wait_ssh() {
  local ip="$1"
  log "Waiting for SSH on $ip"
  for i in $(seq 1 60); do
    if ssh_vm "$ip" "true" 2>/dev/null; then
      return 0
    fi
    sleep 2
  done
  echo "ERROR: SSH timeout for $ip" >&2
  return 1
}

# -- Step 1: Configure headscale ---------------------------------------

log "Configuring headscale"

cp "$SCRIPT_DIR/headscale-config.yaml" /etc/headscale/config.yaml
cp "$SCRIPT_DIR/derp-map.yaml" /etc/headscale/derp-map.yaml

# Clean state for reproducibility.
systemctl stop headscale 2>/dev/null || true
rm -f /var/lib/headscale/db.sqlite
rm -f /var/lib/headscale/noise_private.key

systemctl start headscale
sleep 2

if ! systemctl is-active --quiet headscale; then
  echo "ERROR: headscale failed to start" >&2
  journalctl -u headscale --no-pager -n 20
  exit 1
fi

# Create user and auth keys.
# v0.28+ uses numeric user IDs for --user flag.
headscale users create testuser 2>/dev/null || true
USER_ID=$(headscale users list -o json \
  | python3 -c "import sys,json; print([u['id'] for u in json.load(sys.stdin) if u['name']=='testuser'][0])")
AUTHKEY1=$(headscale preauthkeys create --user "$USER_ID" -o json \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['key'])")
AUTHKEY2=$(headscale preauthkeys create --user "$USER_ID" -o json \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['key'])")

log "Auth keys created"
echo "  VM1: ${AUTHKEY1:0:16}..."
echo "  VM2: ${AUTHKEY2:0:16}..."

# -- Step 2: Create VMs from template ---------------------------------

create_vm() {
  local name="$1" mac="$2" ip="$3"

  if virsh dominfo "$name" &>/dev/null; then
    log "Destroying existing VM: $name"
    virsh destroy "$name" 2>/dev/null || true
    virsh undefine "$name" --remove-all-storage 2>/dev/null || true
    sleep 1
  fi

  log "Creating VM: $name ($ip)"

  # Clone disk.
  local disk="$IMG_DIR/${name}.qcow2"
  qemu-img create -f qcow2 -b "$BASE_IMG" -F qcow2 "$disk" 10G

  # Cloud-init ISO for network config + SSH key.
  local ci_dir
  ci_dir=$(mktemp -d)

  cat > "$ci_dir/meta-data" <<EOMETA
instance-id: ${name}
local-hostname: ${name}
EOMETA

  cat > "$ci_dir/user-data" <<EOUSER
#cloud-config
hostname: ${name}
manage_etc_hosts: true
users:
  - name: worker
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat "${SSH_KEY}.pub")
EOUSER

  cat > "$ci_dir/network-config" <<EONET
version: 2
ethernets:
  enp1s0:
    addresses:
      - ${ip}/20
    routes:
      - to: default
        via: ${HOST_IP}
    nameservers:
      addresses:
        - 1.1.1.1
        - 8.8.8.8
EONET

  local ci_iso="$IMG_DIR/${name}-cidata.iso"
  genisoimage -output "$ci_iso" -volid cidata -joliet -rock \
    "$ci_dir/meta-data" "$ci_dir/user-data" "$ci_dir/network-config" \
    2>/dev/null
  rm -rf "$ci_dir"

  virt-install \
    --name "$name" \
    --memory 2048 \
    --vcpus 2 \
    --disk "path=$disk,format=qcow2" \
    --disk "path=$ci_iso,device=cdrom" \
    --network "network=targets,model=virtio,mac=$mac" \
    --os-variant debian12 \
    --graphics none \
    --noautoconsole \
    --import \
    2>/dev/null
}

create_vm "$VM1_NAME" "$VM1_MAC" "$VM1_IP"
create_vm "$VM2_NAME" "$VM2_MAC" "$VM2_IP"

# -- Step 3: Wait for VMs and install Tailscale -----------------------

wait_ssh "$VM1_IP"
wait_ssh "$VM2_IP"

install_tailscale() {
  local ip="$1" authkey="$2" name="$3"

  log "Installing Tailscale on $name ($ip)"

  ssh_vm "$ip" "sudo bash" <<'EOTS'
# Install tailscale.
curl -fsSL https://tailscale.com/install.sh | sh

# Block direct WireGuard UDP between the two VMs so
# Tailscale is forced to relay through DERP.
# Port 41641 is Tailscale's default WireGuard port.
iptables -A OUTPUT -p udp --dport 41641 -d 10.101.0.0/20 -j DROP
iptables -A INPUT -p udp --sport 41641 -s 10.101.0.0/20 -j DROP
EOTS

  # Start tailscale pointed at our headscale.
  ssh_vm "$ip" "sudo tailscale up \
    --login-server http://${HOST_IP}:8080 \
    --authkey $authkey \
    --accept-routes \
    --hostname $name"
}

# -- Step 3.5: Start the relay BEFORE Tailscale clients ------------------
# Clients will try to connect to DERP immediately on `tailscale up`.

log "Starting Hyper-DERP relay with TLS"

# Kill any existing relay.
pkill -f hyper-derp 2>/dev/null || true
sleep 0.3

RELAY_BIN="$PROJECT_DIR/build/hyper-derp"
CERT="$PROJECT_DIR/certs/cert.pem"
KEY="$PROJECT_DIR/certs/key.pem"

if [ ! -f "$RELAY_BIN" ]; then
  echo "ERROR: build the relay first: cmake --build build -j" >&2
  exit 1
fi

modprobe tls 2>/dev/null || true
"$RELAY_BIN" --port 3340 --workers 2 \
  --tls-cert "$CERT" --tls-key "$KEY" &
RELAY_PID=$!
sleep 1

if ! kill -0 $RELAY_PID 2>/dev/null; then
  echo "ERROR: relay failed to start" >&2
  exit 1
fi
echo "  Relay PID: $RELAY_PID"

# -- Step 4: Register Tailscale clients ---------------------------------

install_tailscale "$VM1_IP" "$AUTHKEY1" "$VM1_NAME"
install_tailscale "$VM2_IP" "$AUTHKEY2" "$VM2_NAME"

# -- Step 5: Verify ----------------------------------------------------

log "Waiting for Tailscale to settle"
sleep 5

log "Headscale nodes"
headscale nodes list

log "Tailscale status on $VM1_NAME"
ssh_vm "$VM1_IP" "tailscale status" || true

log "Tailscale status on $VM2_NAME"
ssh_vm "$VM2_IP" "tailscale status" || true

# Get the tailscale IPs.
TS_IP1=$(ssh_vm "$VM1_IP" "tailscale ip -4" 2>/dev/null || echo "unknown")
TS_IP2=$(ssh_vm "$VM2_IP" "tailscale ip -4" 2>/dev/null || echo "unknown")

log "Tailscale IPs"
echo "  $VM1_NAME: $TS_IP1"
echo "  $VM2_NAME: $TS_IP2"

cat <<EOF

======================================================
  Tailscale Test Environment Ready
======================================================
  Headscale:    http://${HOST_IP}:8080
  DERP relay:   (start manually on port 3340)
  $VM1_NAME:    $VM1_IP (TS: $TS_IP1)
  $VM2_NAME:    $VM2_IP (TS: $TS_IP2)

  Relay PID:    $RELAY_PID

  Test commands:
    # Tailscale ping (tests DERP relay path):
    ssh worker@$VM1_IP "tailscale ping $TS_IP2"

    # ICMP ping over Tailscale:
    ssh worker@$VM1_IP "ping -c 5 $TS_IP2"

    # Check DERP/netcheck:
    ssh worker@$VM1_IP "tailscale netcheck"

    # Check connection type (should show DERP):
    ssh worker@$VM1_IP "tailscale status"

    # Iperf throughput over DERP relay:
    ssh worker@$VM2_IP "iperf3 -s -D"
    ssh worker@$VM1_IP "iperf3 -c $TS_IP2 -t 10"

    # Stop:
    kill $RELAY_PID
    virsh destroy ts-client-1; virsh destroy ts-client-2
======================================================
EOF
