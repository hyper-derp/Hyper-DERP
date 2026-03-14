#!/bin/bash
# stop.sh — Stop benchmark VMs (saves money between tests).
set -euo pipefail

PROJECT="hyper-derp"
ZONE="us-central1-a"

echo "Stopping bench-relay and bench-client..."
gcloud compute instances stop \
  bench-relay bench-client \
  --project="$PROJECT" \
  --zone="$ZONE" \
  --quiet

echo "VMs stopped. No compute charges while stopped."
echo "Restart with: ./start.sh"
