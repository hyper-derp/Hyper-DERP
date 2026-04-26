#!/bin/bash
# Delete GCP benchmark VMs.
#
# Usage: ./tools/gcp/teardown.sh

set -euo pipefail

PROJECT="hyper-derp-bench"
ZONE="europe-west4-a"

echo "Deleting benchmark VMs..."
gcloud compute instances delete \
  hd-relay hdclient-0 hdclient-1 hdclient-2 hdclient-3 \
  --project="$PROJECT" --zone="$ZONE" --quiet

echo "Teardown complete."
