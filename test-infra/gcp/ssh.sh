#!/bin/bash
# ssh.sh — SSH into benchmark VMs via IAP tunnel.
# Usage: ./ssh.sh relay|client [command...]
set -euo pipefail

PROJECT="hyper-derp"
ZONE="us-central1-a"

case "${1:-}" in
  relay)  VM="bench-relay" ;;
  client) VM="bench-client" ;;
  *)
    echo "Usage: $0 relay|client [command...]" >&2
    exit 1
    ;;
esac
shift

if [[ $# -gt 0 ]]; then
  gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap \
    --command="$*"
else
  gcloud compute ssh "$VM" \
    --project="$PROJECT" \
    --zone="$ZONE" \
    --tunnel-through-iap
fi
