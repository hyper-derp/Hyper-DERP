#!/bin/bash
# Deploy Hyper-DERP binaries to GCP benchmark VMs.
#
# Usage: ./tools/gcp/deploy.sh
#
# Builds a release binary locally and copies the server and
# test tools to the relay and all client VMs.

set -euo pipefail

PROJECT="hyper-derp-bench"
ZONE="europe-west4-a"
SCP="gcloud compute scp --project=$PROJECT --zone=$ZONE"

# Build release.
echo "Building release..."
cmake --preset default
cmake --build build -j

echo "Deploying to relay..."
$SCP \
  build/hyper-derp \
  build/tools/bench/hd-scale-test \
  build/tools/bench/derp-scale-test \
  hd-relay:~/

echo "Deploying to clients..."
for i in 0 1 2 3; do
  $SCP \
    build/tools/bench/hd-scale-test \
    build/tools/bench/derp-scale-test \
    "hdclient-${i}":~/ &
done
wait

echo "Deploy complete."
