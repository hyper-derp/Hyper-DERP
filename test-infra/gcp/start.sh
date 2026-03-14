#!/bin/bash
# start.sh — Start stopped benchmark VMs.
set -euo pipefail

PROJECT="hyper-derp"
ZONE="us-central1-a"

echo "Starting bench-relay and bench-client..."
gcloud compute instances start \
  bench-relay bench-client \
  --project="$PROJECT" \
  --zone="$ZONE"

echo ""
gcloud compute instances list \
  --project="$PROJECT" \
  --filter="name:(bench-relay OR bench-client)" \
  --format="table(name,networkInterfaces[0].networkIP,status)"
