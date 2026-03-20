#!/bin/bash
# Full vCPU sweep orchestrator.
# Resizes the relay VM between configs and kicks off
# run_full_config.sh on the client VM for each one.
#
# Runs from the local workstation (needs gcloud + SSH).
#
# Usage:
#   run_full_sweep.sh [--configs "16 8 4 2"]
#                     [--results-dir <path>]
#                     [--skip-resize]
#                     [--dry-run]
set -uo pipefail

# ---- Configuration ------------------------------------------

ZONE="europe-west3-b"
RELAY_VM="bench-relay"
CLIENT_VM="bench-client"
CLIENT_IP=${CLIENT_IP:?Set CLIENT_IP env var}
RELAY_IP=${RELAY_IP:?Set RELAY_IP env var}
SSH_KEY=${SSH_KEY:?Set SSH_KEY env var}
SSH_USER=${SSH_USER:-worker}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CONFIGS="${CONFIGS:-16 8 4 2}"
RESULTS_DIR="${RESULTS_DIR:-${PROJECT_DIR}/bench_results/\
gcp-c4-full-$(date +%Y%m%d)}"
SKIP_RESIZE=0
DRY_RUN=0

# Parse flags.
while [[ $# -gt 0 ]]; do
  case $1 in
    --configs)   CONFIGS="$2"; shift 2 ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --skip-resize) SKIP_RESIZE=1; shift ;;
    --dry-run)   DRY_RUN=1; shift ;;
    *)           echo "unknown flag: $1"; exit 1 ;;
  esac
done

# Resolve RESULTS_DIR to absolute path.
if [[ "$RESULTS_DIR" != /* ]]; then
  RESULTS_DIR="${PROJECT_DIR}/${RESULTS_DIR}"
fi

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

ssh_client() {
  ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT_IP}" "$@" 2>/dev/null
}

ssh_relay() {
  ssh -i "$SSH_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${SSH_USER}@${RELAY_IP}" "$@" 2>/dev/null
}

wait_for_ssh() {
  local host=$1 max_wait=120 elapsed=0
  log "  waiting for SSH on $host..."
  while [ $elapsed -lt $max_wait ]; do
    if ssh -i "$SSH_KEY" \
         -o StrictHostKeyChecking=no \
         -o UserKnownHostsFile=/dev/null \
         -o ConnectTimeout=5 \
         -o LogLevel=ERROR \
         "${SSH_USER}@${host}" \
         'echo ok' 2>/dev/null | grep -q ok; then
      log "  SSH ready ($elapsed s)"
      return 0
    fi
    sleep 5
    elapsed=$((elapsed + 5))
  done
  log "  ERROR: SSH timeout after ${max_wait}s"
  return 1
}

# ---- VM management -------------------------------------------

resize_relay() {
  local vcpus=$1
  local machine_type="c4-highcpu-${vcpus}"

  if [ "$SKIP_RESIZE" -eq 1 ]; then
    log "  skipping resize (--skip-resize)"
    return 0
  fi

  log "  stopping ${RELAY_VM}..."
  gcloud compute instances stop "$RELAY_VM" \
    --zone="$ZONE" --quiet 2>&1 | tail -1

  log "  resizing to ${machine_type}..."
  gcloud compute instances set-machine-type "$RELAY_VM" \
    --zone="$ZONE" \
    --machine-type="$machine_type" --quiet 2>&1 | tail -1

  log "  starting ${RELAY_VM}..."
  gcloud compute instances start "$RELAY_VM" \
    --zone="$ZONE" --quiet 2>&1 | tail -1

  # External IP may change after restart.
  RELAY_IP=$(gcloud compute instances describe \
    "$RELAY_VM" --zone="$ZONE" \
    --format='get(networkInterfaces[0].\
accessConfigs[0].natIP)' 2>/dev/null)
  log "  new relay IP: $RELAY_IP"

  wait_for_ssh "$RELAY_IP"
}

start_vms() {
  log "Ensuring VMs are running..."
  for vm in "$RELAY_VM" "$CLIENT_VM"; do
    local status
    status=$(gcloud compute instances describe "$vm" \
      --zone="$ZONE" \
      --format='get(status)' 2>/dev/null)
    if [ "$status" != "RUNNING" ]; then
      log "  starting $vm..."
      gcloud compute instances start "$vm" \
        --zone="$ZONE" --quiet 2>&1 | tail -1
    fi
  done
  # Refresh IPs.
  RELAY_IP=$(gcloud compute instances describe \
    "$RELAY_VM" --zone="$ZONE" \
    --format='get(networkInterfaces[0].\
accessConfigs[0].natIP)' 2>/dev/null)
  CLIENT_IP=$(gcloud compute instances describe \
    "$CLIENT_VM" --zone="$ZONE" \
    --format='get(networkInterfaces[0].\
accessConfigs[0].natIP)' 2>/dev/null)
  log "  relay=$RELAY_IP  client=$CLIENT_IP"
  wait_for_ssh "$RELAY_IP"
  wait_for_ssh "$CLIENT_IP"
}

# ---- Deploy --------------------------------------------------

deploy_binaries() {
  log "Deploying binaries..."

  # HD binary to relay.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    build/hyper-derp \
    "${SSH_USER}@${RELAY_IP}:/tmp/hyper-derp" 2>/dev/null
  ssh_relay \
    'sudo cp /tmp/hyper-derp /usr/local/bin/hyper-derp && \
     sudo chmod +x /usr/local/bin/hyper-derp'
  log "  HD deployed to relay"

  # Test binaries to client.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    build/tools/derp-scale-test \
    "${SSH_USER}@${CLIENT_IP}:/tmp/derp-scale-test" \
    2>/dev/null
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    build/tools/derp-test-client \
    "${SSH_USER}@${CLIENT_IP}:/tmp/derp-test-client" \
    2>/dev/null
  ssh_client \
    'sudo cp /tmp/derp-scale-test \
       /usr/local/bin/derp-scale-test && \
     sudo cp /tmp/derp-test-client \
       /usr/bin/derp-test-client && \
     sudo chmod +x /usr/local/bin/derp-scale-test \
       /usr/bin/derp-test-client'
  log "  test clients deployed to client"

  # Bench script to client.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    tools/run_full_config.sh \
    "${SSH_USER}@${CLIENT_IP}:/tmp/run_full_config.sh" \
    2>/dev/null
  ssh_client 'chmod +x /tmp/run_full_config.sh'
  log "  bench script deployed to client"

  # Ensure SSH key on client for relay access.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    "$SSH_KEY" \
    "${SSH_USER}@${CLIENT_IP}:/tmp/id_key" 2>/dev/null
  ssh_client \
    "sudo mkdir -p /home/${SSH_USER}/.ssh && \
     sudo cp /tmp/id_key \
       /home/${SSH_USER}/.ssh/id_relay && \
     sudo chown ${SSH_USER}:${SSH_USER} \
       /home/${SSH_USER}/.ssh/id_relay && \
     sudo chmod 600 \
       /home/${SSH_USER}/.ssh/id_relay && \
     rm -f /tmp/id_key"
  log "  SSH key installed on client"
}

# ---- Run config ----------------------------------------------

run_config() {
  local vcpus=$1
  local workers=$2
  local label=$3

  log "Starting benchmark: $label..."

  if [ "$DRY_RUN" -eq 1 ]; then
    log "  [dry-run] would run: \
run_full_config.sh $vcpus $workers $label"
    return 0
  fi

  # Kill any previous benchmark.
  ssh_client \
    'sudo pkill -9 -f run_full_config 2>/dev/null; \
     sudo pkill -9 -f derp-scale-test 2>/dev/null; \
     sudo pkill -9 -f derp-test-client 2>/dev/null' \
    || true
  sleep 2

  # Start benchmark in screen.
  ssh_client \
    "screen -dmS bench bash -c '\
      bash /tmp/run_full_config.sh $vcpus $workers $label \
      > /tmp/bench_${label}.log 2>&1'"
  log "  benchmark running in screen on client"

  # Poll for completion.
  local max_wait=18000  # 5 hours max per config.
  local elapsed=0
  local poll_interval=60
  while [ $elapsed -lt $max_wait ]; do
    sleep $poll_interval
    elapsed=$((elapsed + poll_interval))
    local mins=$((elapsed / 60))

    # Check for DONE file.
    if ssh_client \
         "test -f /tmp/bench_${label}/DONE" \
         2>/dev/null; then
      log "  $label complete (${mins} min)"
      break
    fi

    # Show progress every 5 minutes.
    if [ $((mins % 5)) -eq 0 ]; then
      local progress
      progress=$(ssh_client \
        "tail -1 /tmp/bench_${label}.log" \
        2>/dev/null || echo "?")
      log "  [${mins}m] $progress"
    fi
  done

  if [ $elapsed -ge $max_wait ]; then
    log "  WARNING: $label timed out after \
$((max_wait / 3600)) hrs"
  fi

  # Download results.
  local dest="$RESULTS_DIR/$label"
  mkdir -p "$dest"
  scp -r -i "$SSH_KEY" -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT_IP}:/tmp/bench_${label}/*" \
    "$dest/" 2>/dev/null
  log "  results downloaded to $dest"

  # Download console log.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT_IP}:/tmp/bench_${label}.log" \
    "$dest/console.log" 2>/dev/null
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  FULL vCPU SWEEP"
log "  Configs: $CONFIGS"
log "  Results: $RESULTS_DIR"
log "========================================="
log ""

mkdir -p "$RESULTS_DIR"

# Copy test design for provenance.
cp test-design/FULL_SWEEP_DESIGN.md \
  "$RESULTS_DIR/" 2>/dev/null || true

# Ensure VMs are up and deploy.
start_vms
deploy_binaries

# Workers per config.
workers_for() {
  case $1 in
    2)  echo 1 ;;
    4)  echo 2 ;;
    8)  echo 4 ;;
    16) echo 8 ;;
    *)  echo $(($1 / 2)) ;;
  esac
}

# Run each config (16 → 8 → 4 → 2).
for vcpus in $CONFIGS; do
  log ""
  log "===== $vcpus vCPU ====="

  resize_relay "$vcpus"

  local workers
  workers=$(workers_for "$vcpus")
  run_config "$vcpus" "$workers" "${vcpus}vcpu"

  # 2 vCPU supplemental: 2 workers (oversubscription).
  if [ "$vcpus" -eq 2 ]; then
    log ""
    log "===== 2 vCPU supplemental (2 workers) ====="
    run_config 2 2 "2vcpu_2w"
  fi
done

# ---- Summary -------------------------------------------------

log ""
log "========================================="
log "  SWEEP COMPLETE"
log "========================================="
for vcpus in $CONFIGS; do
  local label="${vcpus}vcpu"
  local done_file="$RESULTS_DIR/$label/DONE"
  if [ -f "$done_file" ]; then
    log "  $label: $(cat "$done_file")"
  else
    log "  $label: INCOMPLETE"
  fi
  if [ "$vcpus" -eq 2 ]; then
    local done2="$RESULTS_DIR/2vcpu_2w/DONE"
    if [ -f "$done2" ]; then
      log "  2vcpu_2w: $(cat "$done2")"
    else
      log "  2vcpu_2w: INCOMPLETE"
    fi
  fi
done
log "Results: $RESULTS_DIR"
