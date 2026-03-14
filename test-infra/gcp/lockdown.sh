#!/bin/bash
# lockdown.sh — Delete Cloud NAT and harden nftables.
#
# After this, VMs have NO path to the internet:
#   - No external IPs (never had them)
#   - No Cloud NAT (deleted)
#   - nftables OUTPUT: only 10.10.0.0/24 + loopback
#   - VPC firewall: only IAP SSH + internal
#
# The only way to reach these VMs is IAP SSH.
# The only traffic they can generate is internal.
set -euo pipefail

PROJECT="hyper-derp"
REGION="us-central1"
ZONE="us-central1-a"
NAT_ROUTER="bench-nat-router"
NAT_NAME="bench-nat"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Verify account safety.
ACCOUNT=$(gcloud config get-value account 2>/dev/null)
if [[ "$ACCOUNT" == *"optris"* ]]; then
  echo "ABORT: work account active" >&2
  exit 1
fi

# Install locked-down nftables on both VMs.
echo "=== Hardening nftables ==="
for VM in bench-relay bench-client; do
  gcloud compute scp \
    "$SCRIPT_DIR/nftables-locked.conf" \
    "$VM:/tmp/nftables.conf" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap

  gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap \
    --command="sudo mv /tmp/nftables.conf /etc/nftables.conf && sudo nft -f /etc/nftables.conf"

  echo "  $VM: nftables locked"
done

# Delete Cloud NAT.
echo "=== Deleting Cloud NAT ==="
gcloud compute routers nats delete "$NAT_NAME" \
  --project="$PROJECT" \
  --router="$NAT_ROUTER" \
  --region="$REGION" \
  --quiet

echo "=== Deleting NAT router ==="
gcloud compute routers delete "$NAT_ROUTER" \
  --project="$PROJECT" \
  --region="$REGION" \
  --quiet

echo ""
echo "=== Lockdown complete ==="
echo "VMs have NO internet access."
echo "Only IAP SSH and internal 10.10.0.0/24 traffic work."
