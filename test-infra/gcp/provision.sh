#!/bin/bash
# provision.sh — Install build tools, Go derper, and
# hyper-derp on both benchmark VMs.
#
# Run BEFORE lockdown.sh (needs internet for apt/go).
set -euo pipefail

PROJECT="hyper-derp"
ZONE="us-central1-a"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Verify account safety.
ACCOUNT=$(gcloud config get-value account 2>/dev/null)
if [[ "$ACCOUNT" == *"optris"* ]]; then
  echo "ABORT: work account active" >&2
  exit 1
fi

# nftables rules: only allow internal + IAP SSH.
NFTABLES_RULES='#!/usr/sbin/nft -f
flush ruleset
table inet filter {
  chain input {
    type filter hook input priority 0; policy drop;
    ct state established,related accept
    iif lo accept
    ip saddr 10.10.0.0/24 accept
    ip saddr 35.235.240.0/20 tcp dport 22 accept
    drop
  }
  chain forward {
    type filter hook forward priority 0; policy drop;
  }
  chain output {
    type filter hook output priority 0; policy drop;
    ct state established,related accept
    oif lo accept
    ip daddr 10.10.0.0/24 accept
    # Allow DNS + apt during provisioning (removed by lockdown).
    ip daddr 169.254.169.254 accept
    udp dport 53 accept
    tcp dport {80, 443} accept
    drop
  }
}'

# Locked-down nftables (installed by lockdown.sh).
NFTABLES_LOCKED='#!/usr/sbin/nft -f
flush ruleset
table inet filter {
  chain input {
    type filter hook input priority 0; policy drop;
    ct state established,related accept
    iif lo accept
    ip saddr 10.10.0.0/24 accept
    ip saddr 35.235.240.0/20 tcp dport 22 accept
    drop
  }
  chain forward {
    type filter hook forward priority 0; policy drop;
  }
  chain output {
    type filter hook output priority 0; policy drop;
    ct state established,related accept
    oif lo accept
    ip daddr 10.10.0.0/24 accept
    ip daddr 169.254.169.254 accept
    drop
  }
}'

# Save locked rules for lockdown.sh to use.
echo "$NFTABLES_LOCKED" > "$SCRIPT_DIR/nftables-locked.conf"

# Common provisioning script run on each VM.
PROVISION_SCRIPT='#!/bin/bash
set -euxo pipefail

export DEBIAN_FRONTEND=noninteractive

# Install build dependencies.
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake clang lld \
  liburing-dev libspdlog-dev libsodium-dev \
  libgtest-dev libgmock-dev \
  nftables ethtool numactl perf-tools-unstable \
  golang-go git ca-certificates curl

# Install Go derper (Tailscale baseline).
export GOPATH=/opt/go
export PATH=$PATH:$GOPATH/bin
go install tailscale.com/cmd/derper@latest

# Apply nftables.
nft -f /etc/nftables.conf
systemctl enable nftables

echo "=== provisioning complete ==="
'

for VM in bench-relay bench-client; do
  echo "=== Provisioning $VM ==="

  # Upload nftables rules.
  echo "$NFTABLES_RULES" | gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap \
    --command="sudo tee /etc/nftables.conf > /dev/null"

  # Run provisioning script.
  echo "$PROVISION_SCRIPT" | gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap \
    --command="sudo bash -s"
done

# Build and upload hyper-derp to relay VM.
echo "=== Building hyper-derp ==="
cd "$REPO_DIR"

# Cross-check: build locally then scp.
cmake --preset default
cmake --build build -j

echo "=== Uploading hyper-derp to relay ==="
gcloud compute scp \
  build/hyper-derp \
  "bench-relay:/tmp/hyper-derp" \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --tunnel-through-iap

gcloud compute ssh bench-relay \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --tunnel-through-iap \
  --command="sudo mv /tmp/hyper-derp /usr/local/bin/hyper-derp && sudo chmod +x /usr/local/bin/hyper-derp"

# Upload test client + scale test to client VM.
echo "=== Uploading test tools to client ==="
gcloud compute scp \
  build/tools/derp-test-client \
  build/tools/derp-scale-test \
  "bench-client:/tmp/" \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --tunnel-through-iap

gcloud compute ssh bench-client \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --tunnel-through-iap \
  --command="sudo mv /tmp/derp-test-client /tmp/derp-scale-test /usr/local/bin/ && sudo chmod +x /usr/local/bin/derp-test-client /usr/local/bin/derp-scale-test"

# Upload tuning script to both.
for VM in bench-relay bench-client; do
  gcloud compute scp \
    scripts/tune_system.sh \
    "$VM:/tmp/tune_system.sh" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap
  gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap \
    --command="sudo mv /tmp/tune_system.sh /usr/local/bin/tune_system.sh && sudo chmod +x /usr/local/bin/tune_system.sh"
done

# Get internal IPs.
RELAY_IP=$(gcloud compute instances describe bench-relay \
  --project="$PROJECT" --zone="$ZONE" \
  --format="get(networkInterfaces[0].networkIP)")
CLIENT_IP=$(gcloud compute instances describe bench-client \
  --project="$PROJECT" --zone="$ZONE" \
  --format="get(networkInterfaces[0].networkIP)")

echo ""
echo "=== Provisioning complete ==="
echo "  Relay:  $RELAY_IP"
echo "  Client: $CLIENT_IP"
echo ""
echo "Next: ./lockdown.sh   # delete NAT, harden nftables"
