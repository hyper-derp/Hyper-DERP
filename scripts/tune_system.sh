#!/bin/bash
# tune_system.sh — Apply system tuning for Hyper-DERP.
#
# Applies sysctl, CPU governor, THP, and optional NIC
# tuning. Must run as root. All settings are idempotent.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "error: must run as root" >&2
  exit 1
fi

echo "=== Hyper-DERP system tuning ==="

# -- Network buffers --------------------------------------------------

echo "[net] tuning socket buffers..."
sysctl -w net.core.wmem_max=67108864
sysctl -w net.core.rmem_max=67108864
sysctl -w net.ipv4.tcp_wmem="4096 524288 67108864"
sysctl -w net.ipv4.tcp_rmem="4096 524288 67108864"
sysctl -w net.core.somaxconn=65536
sysctl -w net.core.netdev_max_backlog=131072

# -- Memory -----------------------------------------------------------

echo "[mem] tuning THP and swappiness..."
# madvise: only our explicit madvise calls get huge pages.
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
# defrag=never: avoid compaction jitter.
echo never > /sys/kernel/mm/transparent_hugepage/defrag
sysctl -w vm.swappiness=0
sysctl -w vm.dirty_ratio=10

# -- CPU --------------------------------------------------------------

echo "[cpu] setting performance governor..."
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  if [[ -f "$gov" ]]; then
    echo performance > "$gov" 2>/dev/null || true
  fi
done

# Disable NMI watchdog to free a perf counter.
sysctl -w kernel.nmi_watchdog=0

# -- NAPI tuning (bare metal, kernel 6.0+) ----------------------------

if [[ -f /proc/sys/net/core/default_qdisc ]]; then
  echo "[net] tuning NAPI..."
  sysctl -w net.core.busy_read=50 2>/dev/null || true
  sysctl -w net.core.busy_poll=50 2>/dev/null || true
fi

# Per-interface NAPI defer (kernel 6.1+).
for iface in /sys/class/net/*/; do
  name=$(basename "$iface")
  [[ "$name" == "lo" ]] && continue
  napi_file="$iface/napi_defer_hard_irqs"
  gro_file="$iface/gro_flush_timeout"
  if [[ -f "$napi_file" ]]; then
    echo 200 > "$napi_file" 2>/dev/null || true
    echo 200000 > "$gro_file" 2>/dev/null || true
    echo "[net] $name: napi_defer=200 gro_flush=200us"
  fi
done

# -- Interrupt coalescing (ethtool) ------------------------------------

echo "[net] tuning interrupt coalescing..."
for iface in /sys/class/net/*/; do
  name=$(basename "$iface")
  [[ "$name" == "lo" ]] && continue
  # Best-effort: ethtool may not be available or the
  # driver may not support coalescing.
  if command -v ethtool &>/dev/null; then
    ethtool -C "$name" \
      rx-usecs 50 rx-frames 64 \
      tx-usecs 50 tx-frames 64 \
      2>/dev/null || true
  fi
done

# -- memlock limit (for io_uring buffer registration) -----------------

echo "[sys] setting memlock unlimited..."
ulimit -l unlimited 2>/dev/null || true

echo ""
echo "=== tuning complete ==="
echo "  wmem_max/rmem_max = 64 MB"
echo "  tcp defaults      = 512 KB"
echo "  THP               = madvise (defrag=never)"
echo "  swappiness         = 0"
echo "  CPU governor      = performance"
echo "  NMI watchdog      = disabled"
