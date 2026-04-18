#!/bin/bash
# Create relay and client VMs for HD Protocol benchmarking.
#
# Usage: ./tools/gcp/setup_vms.sh [relay_vcpu]
#
# Creates:
#   hd-relay        — c4-highcpu-{relay_vcpu}
#   hd-client-{0..3} — c4-highcpu-8

set -euo pipefail

PROJECT="hyper-derp-bench"
ZONE="europe-west4-a"
RELAY_TYPE="${1:-16}"

echo "Creating relay VM (c4-highcpu-${RELAY_TYPE})..."
gcloud compute instances create hd-relay \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --machine-type="c4-highcpu-${RELAY_TYPE}" \
  --image-family=debian-13 \
  --image-project=debian-cloud \
  --boot-disk-size=20GB \
  --metadata=startup-script='#!/bin/bash
apt-get update
apt-get install -y liburing2 libsodium23 libbpf1
modprobe tls
echo tls > /etc/modules-load.d/tls.conf
# Generate self-signed TLS cert for kTLS.
openssl req -x509 -newkey ec \
  -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /etc/hd-key.pem -out /etc/hd-cert.pem \
  -days 365 -nodes -subj "/CN=hd-relay"
# Tune kernel buffers for high throughput.
sysctl -w net.core.wmem_max=4194304
sysctl -w net.core.rmem_max=4194304
sysctl -w net.ipv4.tcp_wmem="4096 262144 4194304"
sysctl -w net.ipv4.tcp_rmem="4096 262144 4194304"
'

echo "Creating 4 client VMs (c4-highcpu-8)..."
for i in 0 1 2 3; do
  gcloud compute instances create "hd-client-${i}" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --machine-type=c4-highcpu-8 \
    --image-family=debian-13 \
    --image-project=debian-cloud \
    --boot-disk-size=20GB \
    --metadata=startup-script='#!/bin/bash
apt-get update
apt-get install -y liburing2 libsodium23
' &
done
wait

echo "Waiting for startup scripts to complete..."
sleep 30

echo "=== VM IPs ==="
gcloud compute instances list \
  --project="$PROJECT" \
  --filter="name~hd-" \
  --format="table(name,networkInterfaces[0].networkIP)"
