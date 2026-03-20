#!/bin/bash
# Phase B GCP orchestrator.
# Manages VM lifecycle, resizing, binary deployment, then
# invokes run_phase_b.sh on bench-client for each vCPU config.
#
# Run from local workstation (not on a VM).
#
# Usage:
#   phase_b_gcp.sh [--start-at <vcpu>] [--skip-deploy]
set -euo pipefail

ZONE=europe-west3-b
RELAY_VM=bench-relay
CLIENT_VM=bench-client
SSH_KEY=${SSH_KEY:?Set SSH_KEY env var}
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
  -o ConnectTimeout=30"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

START_AT=16
SKIP_DEPLOY=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --start-at)    START_AT=$2; shift 2 ;;
    --skip-deploy) SKIP_DEPLOY=1; shift ;;
    *)             echo "unknown: $1"; exit 1 ;;
  esac
done

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Maps vCPU count to GCP machine type.
machine_type() {
  echo "c4-highcpu-$1"
}

relay_ip() {
  gcloud compute instances describe "$RELAY_VM" \
    --zone="$ZONE" \
    --format='get(networkInterfaces[0].accessConfigs[0].natIP)' \
    2>/dev/null
}

client_ip() {
  gcloud compute instances describe "$CLIENT_VM" \
    --zone="$ZONE" \
    --format='get(networkInterfaces[0].accessConfigs[0].natIP)' \
    2>/dev/null
}

relay_internal_ip() {
  gcloud compute instances describe "$RELAY_VM" \
    --zone="$ZONE" \
    --format='get(networkInterfaces[0].networkIP)' \
    2>/dev/null
}

wait_ssh() {
  local ip=$1
  log "Waiting for SSH on $ip..."
  local deadline=$((SECONDS + 120))
  while ! ssh $SSH_OPTS "worker@$ip" "true" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: SSH timeout on $ip"
      return 1
    fi
    sleep 5
  done
  log "SSH ready on $ip"
}

# ---- VM lifecycle -------------------------------------------

start_vm() {
  local vm=$1
  local status
  status=$(gcloud compute instances describe "$vm" \
    --zone="$ZONE" --format='get(status)' 2>/dev/null)
  if [ "$status" = "RUNNING" ]; then
    log "$vm already running"
    return 0
  fi
  log "Starting $vm..."
  gcloud compute instances start "$vm" \
    --zone="$ZONE" 2>/dev/null
}

stop_vm() {
  local vm=$1
  log "Stopping $vm..."
  gcloud compute instances stop "$vm" \
    --zone="$ZONE" --quiet 2>/dev/null || true
}

resize_relay() {
  local vcpu=$1
  local target
  target=$(machine_type "$vcpu")

  local current
  current=$(gcloud compute instances describe "$RELAY_VM" \
    --zone="$ZONE" \
    --format='get(machineType)' 2>/dev/null | \
    awk -F/ '{print $NF}')

  if [ "$current" = "$target" ]; then
    log "Relay already $target"
    return 0
  fi

  log "Resizing relay: $current → $target"
  stop_vm "$RELAY_VM"
  # Wait for full stop.
  local deadline=$((SECONDS + 120))
  while true; do
    local status
    status=$(gcloud compute instances describe "$RELAY_VM" \
      --zone="$ZONE" --format='get(status)' 2>/dev/null)
    if [ "$status" = "TERMINATED" ]; then
      break
    fi
    if ((SECONDS >= deadline)); then
      log "ERROR: VM stop timeout"
      return 1
    fi
    sleep 5
  done

  gcloud compute instances set-machine-type "$RELAY_VM" \
    --zone="$ZONE" --machine-type="$target" 2>/dev/null

  start_vm "$RELAY_VM"
  local ip
  ip=$(relay_ip)
  wait_ssh "$ip"
}

# ---- Binary deployment --------------------------------------

deploy_binaries() {
  log "Building HD and tools..."
  (cd "$PROJECT_DIR" && cmake --build build -j) || {
    log "ERROR: build failed"
    return 1
  }

  local rip cip
  rip=$(relay_ip)
  cip=$(client_ip)

  log "Deploying to relay ($rip)..."
  scp $SSH_OPTS \
    "$PROJECT_DIR/build/hyper-derp" \
    "worker@$rip:/tmp/hyper-derp"
  ssh $SSH_OPTS "worker@$rip" \
    "sudo cp /tmp/hyper-derp /usr/local/bin/hyper-derp && \
     sudo chmod +x /usr/local/bin/hyper-derp"

  log "Deploying to client ($cip)..."
  scp $SSH_OPTS \
    "$PROJECT_DIR/build/tools/derp-scale-test" \
    "$PROJECT_DIR/build/tools/derp-test-client" \
    "worker@$cip:/tmp/"
  ssh $SSH_OPTS "worker@$cip" \
    "sudo cp /tmp/derp-scale-test /tmp/derp-test-client \
       /usr/local/bin/ && \
     sudo chmod +x /usr/local/bin/derp-scale-test \
       /usr/local/bin/derp-test-client"

  # Deploy the sweep script to client.
  scp $SSH_OPTS \
    "$SCRIPT_DIR/run_phase_b.sh" \
    "worker@$cip:/tmp/run_phase_b.sh"
  ssh $SSH_OPTS "worker@$cip" \
    "chmod +x /tmp/run_phase_b.sh"

  log "Deployment complete"
}

# ---- System tuning ------------------------------------------

tune_vm() {
  local ip=$1
  log "Tuning $ip..."
  ssh $SSH_OPTS "worker@$ip" "
    sudo sysctl -w net.core.rmem_max=67108864 >/dev/null
    sudo sysctl -w net.core.wmem_max=67108864 >/dev/null
    sudo sysctl -w net.core.somaxconn=65536 >/dev/null
    sudo sysctl -w 'net.ipv4.tcp_wmem=4096 262144 67108864' \
      >/dev/null
    sudo sysctl -w 'net.ipv4.tcp_rmem=4096 262144 67108864' \
      >/dev/null
    sudo sysctl -w net.core.netdev_max_backlog=131072 \
      >/dev/null
    # CPU governor.
    for f in /sys/devices/system/cpu/cpu*/cpufreq/\
scaling_governor; do
      echo performance | sudo tee \$f >/dev/null 2>&1
    done
    # Disable NMI watchdog.
    echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog \
      >/dev/null 2>&1 || true
    # Load TLS module.
    sudo modprobe tls 2>/dev/null || true
  " 2>/dev/null
}

# ---- Collect results ----------------------------------------

collect_results() {
  local cip=$1
  local local_dir="${PROJECT_DIR}/bench_results/\
gcp-c4-phase-b"
  mkdir -p "$local_dir"

  log "Downloading results to $local_dir..."
  scp -r $SSH_OPTS \
    "worker@$cip:/tmp/bench_phase_b/*" \
    "$local_dir/" 2>/dev/null || true

  log "Results saved to $local_dir"
}

# ---- Main ---------------------------------------------------

CONFIGS=(
  "16 8"
  "8 4"
  "4 2"
  "2 1"
)

main() {
  log "Phase B GCP Orchestrator"
  log "Starting at: ${START_AT} vCPU"
  log ""

  # Start client VM (stays the same size throughout).
  start_vm "$CLIENT_VM"
  local cip
  cip=$(client_ip)
  wait_ssh "$cip"

  # Deploy binaries (once).
  if [ "$SKIP_DEPLOY" -eq 0 ]; then
    # Need relay running to deploy to it.
    start_vm "$RELAY_VM"
    local rip
    rip=$(relay_ip)
    wait_ssh "$rip"
    deploy_binaries
  fi

  tune_vm "$cip"

  for cfg in "${CONFIGS[@]}"; do
    local vcpu workers
    read -r vcpu workers <<< "$cfg"

    if [ "$vcpu" -gt "$START_AT" ]; then
      log "Skipping ${vcpu} vCPU (--start-at ${START_AT})"
      continue
    fi

    log ""
    log "========================================"
    log "  Phase B: ${vcpu} vCPU / ${workers}w"
    log "========================================"

    # Resize relay VM.
    resize_relay "$vcpu"
    local rip
    rip=$(relay_ip)
    local rint
    rint=$(relay_internal_ip)

    # Re-deploy HD binary (resize restarts the VM).
    if [ "$SKIP_DEPLOY" -eq 0 ]; then
      scp $SSH_OPTS \
        "$PROJECT_DIR/build/hyper-derp" \
        "worker@$rip:/tmp/hyper-derp"
      ssh $SSH_OPTS "worker@$rip" \
        "sudo cp /tmp/hyper-derp /usr/local/bin/hyper-derp && \
         sudo chmod +x /usr/local/bin/hyper-derp"
    fi

    # Tune relay.
    tune_vm "$rip"

    # Run sweep on client VM (--only runs just this config).
    log "Running sweep on client (relay=$rint)..."
    ssh $SSH_OPTS -o ServerAliveInterval=60 \
      "worker@$cip" \
      "RELAY=$rint /tmp/run_phase_b.sh \
        --only $vcpu" 2>&1 | \
      tee -a "${PROJECT_DIR}/bench_results/\
phase_b_${vcpu}vcpu.log" || {
      log "WARNING: ${vcpu} vCPU sweep had errors"
    }

    # Download results after each config.
    collect_results "$cip"
  done

  log ""
  log "Phase B complete. Stopping VMs..."
  stop_vm "$RELAY_VM"
  stop_vm "$CLIENT_VM"
  log "Done."
}

main
