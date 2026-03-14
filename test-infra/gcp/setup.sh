#!/bin/bash
# setup.sh — Create GCP benchmark infrastructure.
#
# Creates:
#   - Custom VPC with single subnet (no default routes)
#   - Cloud NAT for provisioning (deleted by lockdown.sh)
#   - 2x c4-highcpu-16 VMs with NO external IPs
#   - Firewall: IAP SSH + internal only
#   - nftables on each VM: hard-block non-internal traffic
#
# All test traffic stays on the internal 10.10.0.0/24
# subnet. VMs have no external IPs. After provisioning,
# run lockdown.sh to delete Cloud NAT.
set -euo pipefail

PROJECT="hyper-derp"
REGION="us-central1"
ZONE="us-central1-a"
VPC="bench-vpc"
SUBNET="bench-subnet"
SUBNET_RANGE="10.10.0.0/24"
NAT_ROUTER="bench-nat-router"
NAT_NAME="bench-nat"
IMAGE_FAMILY="debian-12"
IMAGE_PROJECT="debian-cloud"
MACHINE="c4-highcpu-16"

# Verify account safety.
ACCOUNT=$(gcloud config get-value account 2>/dev/null)
if [[ "$ACCOUNT" == *"optris"* ]]; then
  echo "ABORT: work account active ($ACCOUNT)" >&2
  echo "Run: gcloud config set account khruskowski@gmail.com" >&2
  exit 1
fi
echo "Account: $ACCOUNT"
echo "Project: $PROJECT"

# ---- VPC + Subnet ----

echo "=== Creating VPC ==="
gcloud compute networks create "$VPC" \
  --project="$PROJECT" \
  --subnet-mode=custom \
  --bgp-routing-mode=regional

echo "=== Creating subnet ==="
gcloud compute networks subnets create "$SUBNET" \
  --project="$PROJECT" \
  --network="$VPC" \
  --region="$REGION" \
  --range="$SUBNET_RANGE"

# ---- Firewall ----

echo "=== Creating firewall rules ==="

# Allow SSH from IAP (35.235.240.0/20).
gcloud compute firewall-rules create bench-allow-iap-ssh \
  --project="$PROJECT" \
  --network="$VPC" \
  --direction=INGRESS \
  --action=ALLOW \
  --rules=tcp:22 \
  --source-ranges=35.235.240.0/20 \
  --priority=1000

# Allow ALL internal traffic within the subnet.
gcloud compute firewall-rules create bench-allow-internal \
  --project="$PROJECT" \
  --network="$VPC" \
  --direction=INGRESS \
  --action=ALLOW \
  --rules=all \
  --source-ranges="$SUBNET_RANGE" \
  --priority=1000

# Deny all other ingress (belt-and-suspenders).
gcloud compute firewall-rules create bench-deny-ingress \
  --project="$PROJECT" \
  --network="$VPC" \
  --direction=INGRESS \
  --action=DENY \
  --rules=all \
  --source-ranges=0.0.0.0/0 \
  --priority=65000

# ---- Cloud NAT (temporary, for provisioning) ----

echo "=== Creating Cloud NAT (for provisioning) ==="
gcloud compute routers create "$NAT_ROUTER" \
  --project="$PROJECT" \
  --network="$VPC" \
  --region="$REGION"

gcloud compute routers nats create "$NAT_NAME" \
  --project="$PROJECT" \
  --router="$NAT_ROUTER" \
  --region="$REGION" \
  --nat-all-subnet-ip-ranges \
  --auto-allocate-nat-external-ips

# ---- VMs ----

echo "=== Creating relay VM ==="
gcloud compute instances create bench-relay \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --machine-type="$MACHINE" \
  --network-interface="subnet=$SUBNET,no-address" \
  --image-family="$IMAGE_FAMILY" \
  --image-project="$IMAGE_PROJECT" \
  --boot-disk-size=20GB \
  --boot-disk-type=hyperdisk-balanced \
  --metadata=enable-oslogin=true

echo "=== Creating client VM ==="
gcloud compute instances create bench-client \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --machine-type="$MACHINE" \
  --network-interface="subnet=$SUBNET,no-address" \
  --image-family="$IMAGE_FAMILY" \
  --image-project="$IMAGE_PROJECT" \
  --boot-disk-size=20GB \
  --boot-disk-type=hyperdisk-balanced \
  --metadata=enable-oslogin=true

# ---- Print IPs ----

echo ""
echo "=== Internal IPs ==="
gcloud compute instances list \
  --project="$PROJECT" \
  --filter="name:(bench-relay OR bench-client)" \
  --format="table(name,zone,machineType.basename(),networkInterfaces[0].networkIP,status)"

echo ""
echo "=== Setup complete ==="
echo "Next steps:"
echo "  1. ./provision.sh        # install software"
echo "  2. ./lockdown.sh         # delete Cloud NAT"
echo "  3. ./ssh.sh relay|client # SSH via IAP"
