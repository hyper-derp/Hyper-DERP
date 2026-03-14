#!/bin/bash
# teardown.sh — Delete ALL benchmark infrastructure.
# Run when done with benchmarking to stop all charges.
set -euo pipefail

PROJECT="hyper-derp"
REGION="us-central1"
ZONE="us-central1-a"

# Verify account safety.
ACCOUNT=$(gcloud config get-value account 2>/dev/null)
if [[ "$ACCOUNT" == *"optris"* ]]; then
  echo "ABORT: work account active" >&2
  exit 1
fi

echo "This will DELETE all benchmark infrastructure."
read -p "Type 'yes' to confirm: " confirm
if [[ "$confirm" != "yes" ]]; then
  echo "Aborted."
  exit 1
fi

echo "=== Deleting VMs ==="
gcloud compute instances delete \
  bench-relay bench-client \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --quiet 2>/dev/null || true

echo "=== Deleting firewall rules ==="
gcloud compute firewall-rules delete \
  bench-allow-iap-ssh \
  bench-allow-internal \
  bench-deny-ingress \
  --project="$PROJECT" \
  --quiet 2>/dev/null || true

echo "=== Deleting Cloud NAT (if exists) ==="
gcloud compute routers nats delete bench-nat \
  --project="$PROJECT" \
  --router=bench-nat-router \
  --region="$REGION" \
  --quiet 2>/dev/null || true
gcloud compute routers delete bench-nat-router \
  --project="$PROJECT" \
  --region="$REGION" \
  --quiet 2>/dev/null || true

echo "=== Deleting subnet ==="
gcloud compute networks subnets delete bench-subnet \
  --project="$PROJECT" \
  --region="$REGION" \
  --quiet 2>/dev/null || true

echo "=== Deleting VPC ==="
gcloud compute networks delete bench-vpc \
  --project="$PROJECT" \
  --quiet 2>/dev/null || true

echo ""
echo "=== Teardown complete ==="
echo "All benchmark resources deleted."
