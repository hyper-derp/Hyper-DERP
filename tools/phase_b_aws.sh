#!/bin/bash
# Phase B AWS orchestrator.
# Manages EC2 lifecycle, resizing, binary deployment, then
# invokes run_phase_b.sh on bench-client for each vCPU config.
#
# Run from local workstation (not on an EC2 instance).
#
# Prerequisites:
#   - AWS CLI configured (aws configure)
#   - VPC, subnet, security group, placement group created
#   - SSH key imported to AWS (hd-bench-key)
#
# Usage:
#   phase_b_aws.sh [--start-at <vcpu>] [--skip-deploy]
#                  [--only <vcpu>]
set -euo pipefail

REGION=eu-central-1
SUBNET=${SUBNET:?Set SUBNET env var}
SG=${SG:?Set SG env var}
KEY_NAME=hd-bench-key
AMI=${AMI:?Set AMI env var}
PLACEMENT_GROUP=hd-bench
RELAY_INTERNAL_IP=${RELAY_INTERNAL_IP:?Set RELAY_INTERNAL_IP env var}
CLIENT_INTERNAL_IP=${CLIENT_INTERNAL_IP:?Set CLIENT_INTERNAL_IP env var}

SSH_KEY=${SSH_KEY:?Set SSH_KEY env var}
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
  -o ConnectTimeout=30"
# Debian AMI default user is "admin".
SSH_USER=${SSH_USER:-admin}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

START_AT=16
ONLY=0
SKIP_DEPLOY=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --start-at)    START_AT=$2; shift 2 ;;
    --only)        ONLY=$2; START_AT=$2; shift 2 ;;
    --skip-deploy) SKIP_DEPLOY=1; shift ;;
    *)             echo "unknown: $1"; exit 1 ;;
  esac
done

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Maps vCPU count to AWS instance type.
instance_type() {
  case $1 in
    2)  echo "c7i.large" ;;
    4)  echo "c7i.xlarge" ;;
    8)  echo "c7i.2xlarge" ;;
    16) echo "c7i.4xlarge" ;;
    *)  echo "c7i.4xlarge" ;;
  esac
}

# ---- Instance ID lookups ------------------------------------

relay_id() {
  aws ec2 describe-instances \
    --region "$REGION" \
    --filters "Name=tag:Name,Values=bench-relay" \
              "Name=instance-state-name,Values=running,stopped,pending,stopping" \
    --query 'Reservations[0].Instances[0].InstanceId' \
    --output text 2>/dev/null
}

client_id() {
  aws ec2 describe-instances \
    --region "$REGION" \
    --filters "Name=tag:Name,Values=bench-client" \
              "Name=instance-state-name,Values=running,stopped,pending,stopping" \
    --query 'Reservations[0].Instances[0].InstanceId' \
    --output text 2>/dev/null
}

relay_public_ip() {
  aws ec2 describe-instances \
    --region "$REGION" \
    --instance-ids "$1" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' \
    --output text 2>/dev/null
}

client_public_ip() {
  relay_public_ip "$1"
}

wait_ssh() {
  local ip=$1
  log "Waiting for SSH on $ip..."
  local deadline=$((SECONDS + 180))
  while ! ssh $SSH_OPTS "${SSH_USER}@$ip" "true" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: SSH timeout on $ip"
      return 1
    fi
    sleep 5
  done
  log "SSH ready on $ip"
}

# ---- Instance lifecycle -------------------------------------

launch_instance() {
  local name=$1 itype=$2 private_ip=$3
  log "Launching $name ($itype)..."

  local id
  id=$(aws ec2 run-instances \
    --image-id "$AMI" \
    --instance-type "$itype" \
    --key-name "$KEY_NAME" \
    --subnet-id "$SUBNET" \
    --security-group-ids "$SG" \
    --placement "GroupName=$PLACEMENT_GROUP" \
    --private-ip-address "$private_ip" \
    --tag-specifications \
      "ResourceType=instance,Tags=[{Key=Name,Value=$name}]" \
    --region "$REGION" \
    --query 'Instances[0].InstanceId' \
    --output text 2>&1)

  if [[ "$id" == *"error"* ]] || [[ "$id" == "None" ]]; then
    log "ERROR: Failed to launch $name: $id"
    return 1
  fi

  echo "$id"
  log "$name launched: $id"

  # Wait until running.
  aws ec2 wait instance-running \
    --instance-ids "$id" --region "$REGION"
  log "$name is running"
}

stop_instance() {
  local id=$1 name=$2
  log "Stopping $name ($id)..."
  aws ec2 stop-instances \
    --instance-ids "$id" --region "$REGION" \
    >/dev/null 2>&1 || true
  aws ec2 wait instance-stopped \
    --instance-ids "$id" --region "$REGION" 2>/dev/null
  log "$name stopped"
}

terminate_instance() {
  local id=$1 name=$2
  log "Terminating $name ($id)..."
  aws ec2 terminate-instances \
    --instance-ids "$id" --region "$REGION" \
    >/dev/null 2>&1 || true
}

resize_relay() {
  local vcpu=$1
  local target
  target=$(instance_type "$vcpu")

  local id
  id=$(relay_id)

  if [ "$id" = "None" ] || [ -z "$id" ]; then
    # No existing relay — launch fresh.
    id=$(launch_instance "bench-relay" "$target" \
      "$RELAY_INTERNAL_IP")
    echo "$id"
    return
  fi

  # Check current type.
  local current
  current=$(aws ec2 describe-instances \
    --region "$REGION" \
    --instance-ids "$id" \
    --query 'Reservations[0].Instances[0].InstanceType' \
    --output text 2>/dev/null)

  if [ "$current" = "$target" ]; then
    # Ensure it's running.
    local state
    state=$(aws ec2 describe-instances \
      --region "$REGION" \
      --instance-ids "$id" \
      --query 'Reservations[0].Instances[0].State.Name' \
      --output text 2>/dev/null)
    if [ "$state" != "running" ]; then
      aws ec2 start-instances \
        --instance-ids "$id" --region "$REGION" \
        >/dev/null
      aws ec2 wait instance-running \
        --instance-ids "$id" --region "$REGION"
    fi
    log "Relay already $target ($id)"
    echo "$id"
    return
  fi

  log "Resizing relay: $current → $target"
  stop_instance "$id" "bench-relay"

  aws ec2 modify-instance-attribute \
    --instance-id "$id" \
    --instance-type "$target" \
    --region "$REGION"

  aws ec2 start-instances \
    --instance-ids "$id" --region "$REGION" \
    >/dev/null
  aws ec2 wait instance-running \
    --instance-ids "$id" --region "$REGION"

  log "Relay resized to $target and running"
  echo "$id"
}

# ---- Instance setup (first boot) ---------------------------

setup_instance() {
  local ip=$1 role=$2
  log "Setting up $role ($ip)..."

  ssh $SSH_OPTS "${SSH_USER}@$ip" "
    # Check if already set up.
    if [ -f /tmp/.hd_setup_done ]; then
      echo 'Already set up'
      exit 0
    fi

    # Install packages.
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      sysstat linux-cpupower ethtool curl wget 2>/dev/null

    # Install Go (for derper).
    if ! command -v go &>/dev/null; then
      wget -q https://go.dev/dl/go1.24.1.linux-amd64.tar.gz \
        -O /tmp/go.tar.gz
      sudo tar -C /usr/local -xzf /tmp/go.tar.gz
      echo 'export PATH=\$PATH:/usr/local/go/bin:/root/go/bin' | \
        sudo tee /etc/profile.d/go.sh >/dev/null
      export PATH=\$PATH:/usr/local/go/bin:/root/go/bin
    fi

    # Build derper.
    if ! command -v derper &>/dev/null; then
      export PATH=\$PATH:/usr/local/go/bin
      sudo /usr/local/go/bin/go install -trimpath \
        -ldflags='-s -w' \
        tailscale.com/cmd/derper@latest
      sudo cp ~/go/bin/derper /usr/local/bin/ 2>/dev/null || \
        sudo cp /root/go/bin/derper /usr/local/bin/ 2>/dev/null
    fi

    touch /tmp/.hd_setup_done
    echo 'Setup complete'
  " 2>&1 | tail -5
}

# ---- System tuning ------------------------------------------

tune_vm() {
  local ip=$1
  log "Tuning $ip..."
  ssh $SSH_OPTS "${SSH_USER}@$ip" "
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

# ---- Binary deployment --------------------------------------

deploy_binaries() {
  log "Building HD and tools..."
  (cd "$PROJECT_DIR" && cmake --build build -j) || {
    log "ERROR: build failed"
    return 1
  }

  local relay_ip=$1 client_ip=$2

  log "Deploying to relay ($relay_ip)..."
  scp $SSH_OPTS \
    "$PROJECT_DIR/build/hyper-derp" \
    "${SSH_USER}@$relay_ip:/tmp/hyper-derp"
  ssh $SSH_OPTS "${SSH_USER}@$relay_ip" \
    "sudo cp /tmp/hyper-derp /usr/local/bin/hyper-derp && \
     sudo chmod +x /usr/local/bin/hyper-derp"

  log "Deploying to client ($client_ip)..."
  scp $SSH_OPTS \
    "$PROJECT_DIR/build/tools/derp-scale-test" \
    "$PROJECT_DIR/build/tools/derp-test-client" \
    "${SSH_USER}@$client_ip:/tmp/"
  ssh $SSH_OPTS "${SSH_USER}@$client_ip" \
    "sudo cp /tmp/derp-scale-test /tmp/derp-test-client \
       /usr/local/bin/ && \
     sudo chmod +x /usr/local/bin/derp-scale-test \
       /usr/local/bin/derp-test-client"

  # Deploy the sweep script to client.
  scp $SSH_OPTS \
    "$SCRIPT_DIR/run_phase_b.sh" \
    "${SSH_USER}@$client_ip:/tmp/run_phase_b.sh"
  ssh $SSH_OPTS "${SSH_USER}@$client_ip" \
    "chmod +x /tmp/run_phase_b.sh"

  log "Deployment complete"
}

# ---- Collect results ----------------------------------------

collect_results() {
  local client_ip=$1
  local local_dir="${PROJECT_DIR}/bench_results/\
aws-c7i-phase-b"
  mkdir -p "$local_dir"

  log "Downloading results to $local_dir..."
  scp -r $SSH_OPTS \
    "${SSH_USER}@$client_ip:/tmp/bench_phase_b/*" \
    "$local_dir/" 2>/dev/null || true

  log "Results saved to $local_dir"
}

# ---- Collect AWS-specific info ------------------------------

collect_aws_info() {
  local ip=$1 dest=$2
  log "Collecting AWS instance info..."
  ssh $SSH_OPTS "${SSH_USER}@$ip" "
    echo '{' > /tmp/aws_info.json
    echo '  \"instance_type\": \"'\$(curl -s \
      http://169.254.169.254/latest/meta-data/instance-type\
      2>/dev/null || echo unknown)'\",' >> /tmp/aws_info.json
    echo '  \"az\": \"'\$(curl -s \
      http://169.254.169.254/latest/meta-data/placement/\
availability-zone 2>/dev/null || echo unknown)'\",' \
      >> /tmp/aws_info.json
    echo '  \"ami\": \"'\$(curl -s \
      http://169.254.169.254/latest/meta-data/ami-id \
      2>/dev/null || echo unknown)'\",' >> /tmp/aws_info.json
    echo '  \"ena_driver\": \"'\$(ethtool -i ens5 2>/dev/null | \
      grep version | head -1 || echo unknown)'\"' \
      >> /tmp/aws_info.json
    echo '}' >> /tmp/aws_info.json
  " 2>/dev/null
  scp $SSH_OPTS "${SSH_USER}@$ip:/tmp/aws_info.json" \
    "$dest/aws_instance_info.json" 2>/dev/null || true
}

# ---- Main ---------------------------------------------------

CONFIGS=(
  "16 8"
  "8 4"
)

main() {
  log "Phase B AWS Orchestrator"
  log "Starting at: ${START_AT} vCPU"
  log ""

  # Launch or find client VM (stays same size throughout).
  local client_iid
  client_iid=$(client_id)
  if [ "$client_iid" = "None" ] || [ -z "$client_iid" ]; then
    client_iid=$(launch_instance "bench-client" "c7i.2xlarge" \
      "$CLIENT_INTERNAL_IP")
  else
    # Ensure running.
    local state
    state=$(aws ec2 describe-instances \
      --region "$REGION" \
      --instance-ids "$client_iid" \
      --query 'Reservations[0].Instances[0].State.Name' \
      --output text)
    if [ "$state" != "running" ]; then
      aws ec2 start-instances \
        --instance-ids "$client_iid" --region "$REGION" \
        >/dev/null
      aws ec2 wait instance-running \
        --instance-ids "$client_iid" --region "$REGION"
    fi
  fi

  local cip
  cip=$(client_public_ip "$client_iid")
  wait_ssh "$cip"
  setup_instance "$cip" "client"
  tune_vm "$cip"

  for cfg in "${CONFIGS[@]}"; do
    local vcpu workers
    read -r vcpu workers <<< "$cfg"

    if [ "$vcpu" -gt "$START_AT" ]; then
      log "Skipping ${vcpu} vCPU (--start-at ${START_AT})"
      continue
    fi
    if [ "$ONLY" -gt 0 ] && [ "$vcpu" -ne "$ONLY" ]; then
      continue
    fi

    log ""
    log "========================================"
    log "  Phase B: ${vcpu} vCPU / ${workers}w"
    log "========================================"

    # Resize relay VM.
    local relay_iid
    relay_iid=$(resize_relay "$vcpu")
    local rip
    rip=$(relay_public_ip "$relay_iid")
    wait_ssh "$rip"

    # Set up relay (install Go, derper, etc).
    setup_instance "$rip" "relay"

    # Deploy binaries.
    if [ "$SKIP_DEPLOY" -eq 0 ]; then
      deploy_binaries "$rip" "$cip"
    fi

    # Tune relay.
    tune_vm "$rip"

    # Collect AWS-specific info.
    local out_dir="${PROJECT_DIR}/bench_results/\
aws-c7i-phase-b/${vcpu}vcpu"
    mkdir -p "$out_dir"
    collect_aws_info "$rip" "$out_dir"

    # Run sweep on client.
    # run_phase_b.sh uses RELAY (internal IP) and
    # RELAY_USER/RELAY_KEY for SSH to relay.
    log "Running sweep on client (relay=$RELAY_INTERNAL_IP)..."
    ssh $SSH_OPTS -o ServerAliveInterval=60 \
      "${SSH_USER}@$cip" \
      "RELAY=$RELAY_INTERNAL_IP \
       RELAY_USER=$SSH_USER \
       RELAY_KEY=\$HOME/.ssh/id_relay \
       /tmp/run_phase_b.sh \
        --only $vcpu" 2>&1 | \
      tee -a "${PROJECT_DIR}/bench_results/\
phase_b_aws_${vcpu}vcpu.log" || {
      log "WARNING: ${vcpu} vCPU sweep had errors"
    }

    # Download results after each config.
    collect_results "$cip"
  done

  log ""
  log "Phase B AWS complete. Stopping instances..."
  local rid cid
  rid=$(relay_id)
  cid=$(client_id)
  [ "$rid" != "None" ] && stop_instance "$rid" "bench-relay"
  [ "$cid" != "None" ] && stop_instance "$cid" "bench-client"
  log "Done."
}

main
