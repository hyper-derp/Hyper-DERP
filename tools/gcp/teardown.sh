#!/bin/bash
# Delete GCP benchmark VMs.
#
# Usage: ./tools/gcp/teardown.sh

set -euo pipefail

PROJECT="hyper-derp-bench"
ZONE="europe-west4-a"

echo "Deleting benchmark VMs..."
gcloud compute instances delete \
  hd-relay hd-client-0 hd-client-1 hd-client-2 hd-client-3 \
  --project="$PROJECT" --zone="$ZONE" --quiet

echo "Teardown complete."
