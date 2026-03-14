#!/bin/bash
# Worker scaling orchestrator.
# Deploys updated binary (with larger SPSC rings) and
# kicks off run_worker_scaling.sh on the client VM.
#
# Runs from the local workstation (needs gcloud + SSH).
#
# Usage:
#   run_worker_sweep.sh [--skip-deploy]
#                       [--skip-test2]
#                       [--skip-test3]
#                       [--skip-ktls]
#                       [--results-dir <path>]
#                       [--dry-run]
set -uo pipefail

# ---- Configuration ------------------------------------------

ZONE="europe-west3-b"
RELAY_VM="bench-relay"
CLIENT_VM="bench-client"
SSH_KEY="$HOME/.ssh/id_ed25519_targets"
SSH_USER="worker"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

RESULTS_DIR="${RESULTS_DIR:-${PROJECT_DIR}/bench_results/\
gcp-c4-worker-scaling-$(date +%Y%m%d)}"
SKIP_DEPLOY=0
SKIP_TEST2=0
SKIP_TEST3=0
SKIP_KTLS=0
DRY_RUN=0

# Parse flags.
while [[ $# -gt 0 ]]; do
  case $1 in
    --skip-deploy) SKIP_DEPLOY=1; shift ;;
    --skip-test2)  SKIP_TEST2=1; shift ;;
    --skip-test3)  SKIP_TEST3=1; shift ;;
    --skip-ktls)   SKIP_KTLS=1; shift ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --dry-run)     DRY_RUN=1; shift ;;
    *)             echo "unknown flag: $1"; exit 1 ;;
  esac
done

if [[ "$RESULTS_DIR" != /* ]]; then
  RESULTS_DIR="${PROJECT_DIR}/${RESULTS_DIR}"
fi

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Resolve external IPs from gcloud.
refresh_ips() {
  RELAY_IP=$(gcloud compute instances describe \
    "$RELAY_VM" --zone="$ZONE" \
    --format='get(networkInterfaces[0].\
accessConfigs[0].natIP)' 2>/dev/null)
  CLIENT_IP=$(gcloud compute instances describe \
    "$CLIENT_VM" --zone="$ZONE" \
    --format='get(networkInterfaces[0].\
accessConfigs[0].natIP)' 2>/dev/null)
}

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

# ---- Verify relay is 16 vCPU ---------------------------------

verify_relay() {
  log "Verifying relay VM..."
  local vcpus
  vcpus=$(ssh_relay "nproc" 2>/dev/null)
  if [ "${vcpus:-0}" -ne 16 ]; then
    log "ERROR: relay has ${vcpus:-?} vCPUs, expected 16"
    log "  Resize to c4-highcpu-16 first."
    exit 1
  fi
  log "  relay: $vcpus vCPUs — OK"
}

# ---- Ensure VMs are running ---------------------------------

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
  refresh_ips
  log "  relay=$RELAY_IP  client=$CLIENT_IP"
  wait_for_ssh "$RELAY_IP"
  wait_for_ssh "$CLIENT_IP"
}

# ---- Deploy --------------------------------------------------

deploy() {
  log "Building with updated SPSC ring size..."
  cd "$PROJECT_DIR"
  cmake --build build -j || {
    log "ERROR: build failed"; exit 1
  }
  log "  Build OK"

  # HD binary to relay.
  log "Deploying binaries..."
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
  ssh_client \
    'sudo cp /tmp/derp-scale-test \
       /usr/local/bin/derp-scale-test && \
     sudo chmod +x /usr/local/bin/derp-scale-test'
  log "  test client deployed to client"

  # Bench script to client.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    tools/run_worker_scaling.sh \
    "${SSH_USER}@${CLIENT_IP}:/tmp/run_worker_scaling.sh" \
    2>/dev/null
  ssh_client 'chmod +x /tmp/run_worker_scaling.sh'
  log "  bench script deployed to client"

  # Ensure SSH key on client for relay access.
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    "$SSH_KEY" \
    "${SSH_USER}@${CLIENT_IP}:/tmp/id_key" 2>/dev/null
  ssh_client \
    'sudo mkdir -p /home/worker/.ssh && \
     sudo cp /tmp/id_key \
       /home/worker/.ssh/id_ed25519_targets && \
     sudo chown worker:worker \
       /home/worker/.ssh/id_ed25519_targets && \
     sudo chmod 600 \
       /home/worker/.ssh/id_ed25519_targets && \
     rm -f /tmp/id_key'
  log "  SSH key installed on client"

  # Ensure TLS certs on relay.
  ssh_relay \
    'test -f /tmp/cert.pem || \
     openssl req -x509 -newkey ec \
     -pkeyopt ec_paramgen_curve:prime256v1 \
     -keyout /tmp/key.pem -out /tmp/cert.pem \
     -days 1 -nodes -subj "/CN=bench-relay" \
     2>/dev/null'
  log "  TLS certs ensured on relay"
}

# ---- Run test ------------------------------------------------

run_test() {
  log "Starting worker scaling test..."

  if [ "$DRY_RUN" -eq 1 ]; then
    log "  [dry-run] would run: run_worker_scaling.sh"
    return 0
  fi

  # Kill any previous benchmark.
  ssh_client \
    'pkill -9 -f run_worker_scaling 2>/dev/null; \
     pkill -9 -f derp-scale-test 2>/dev/null' || true
  sleep 2

  # Build skip flags.
  local flags=""
  [ "$SKIP_TEST2" -eq 1 ] && flags+=" --skip-test2"
  [ "$SKIP_TEST3" -eq 1 ] && flags+=" --skip-test3"
  [ "$SKIP_KTLS" -eq 1 ] && flags+=" --skip-ktls"

  ssh_client \
    "screen -dmS bench bash -c '\
      bash /tmp/run_worker_scaling.sh $flags \
      2>&1 | tee /tmp/bench_worker_scaling.log'"
  log "  test running in screen on client"
}

# ---- Monitor -------------------------------------------------

monitor() {
  local max_wait=36000  # 10 hours max
  local elapsed=0
  local poll=60

  while [ $elapsed -lt $max_wait ]; do
    sleep $poll
    elapsed=$((elapsed + poll))
    local mins=$((elapsed / 60))

    if ssh_client \
         "test -f /tmp/bench_worker_scaling/DONE" \
         2>/dev/null; then
      log "Test complete ($mins min)"
      break
    fi

    if [ $((mins % 5)) -eq 0 ]; then
      local progress
      progress=$(ssh_client \
        "tail -1 /tmp/bench_worker_scaling.log" \
        2>/dev/null || echo "?")
      log "  [${mins}m] $progress"
    fi
  done

  if [ $elapsed -ge $max_wait ]; then
    log "WARNING: timed out after $((max_wait / 3600)) hrs"
  fi
}

# ---- Download ------------------------------------------------

download() {
  mkdir -p "$RESULTS_DIR"
  scp -r -i "$SSH_KEY" -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT_IP}:/tmp/bench_worker_scaling" \
    "${RESULTS_DIR}/data" 2>/dev/null
  scp -i "$SSH_KEY" -o LogLevel=ERROR \
    "${SSH_USER}@${CLIENT_IP}:\
/tmp/bench_worker_scaling.log" \
    "$RESULTS_DIR/console.log" 2>/dev/null
  log "Results downloaded to $RESULTS_DIR"
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  WORKER SCALING INVESTIGATION"
log "  Results: $RESULTS_DIR"
log "========================================="
log ""

mkdir -p "$RESULTS_DIR"

# Copy test design for provenance.
cp "${PROJECT_DIR}/test-design/WORKER_SCALING_DESIGN.md" \
  "$RESULTS_DIR/" 2>/dev/null || true

start_vms
verify_relay

if [ "$SKIP_DEPLOY" -eq 0 ]; then
  deploy
fi

run_test

if [ "$DRY_RUN" -eq 0 ]; then
  monitor
  download
fi

log ""
log "========================================="
log "  WORKER SCALING COMPLETE"
log "  Results: $RESULTS_DIR"
log "========================================="
