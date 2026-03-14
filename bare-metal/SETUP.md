# Bare Metal Setup — ConnectX-4 LX 25GbE

## Hardware

- **NICs**: 2x Mellanox ConnectX-4 LX (MCX4121A-ACAT)
  - Dual-port 25GbE SFP28
  - Old stock from 2020/2021
  - PCIe 3.0 x8
- **DAC**: SFP28 25GbE direct-attach copper cable
  - **NOT QSFP28** — the CX4 LX dual-port uses SFP28 connectors
- **Workstations**: 2x Dell Precision Tower 5810 (2014)
  - CPU: Xeon E5-1650 v3 (Haswell), 6C/12T, 3.5 GHz base / 3.8 turbo
  - RAM: arriving Monday (DDR4-2133 ECC, 4 channels)
  - PCIe: 3.0 x16 slots available (x8 electrical sufficient for CX4 LX)
  - Single-socket, single NUMA node — no NUMA concerns

## Step 0: Inventory and Verify Hardware

Before anything else:

```bash
# Part numbers on the NIC sticker or box
# MCX4131A-BCAT = 1x 50GbE QSFP28
# MCX4121A-ACAT = 2x 25GbE SFP28
# This determines port count and max speed

# Check DAC cable spec matches NIC port type
# QSFP28 for 50GbE, SFP28 for 25GbE
```

## Step 1: Firmware

ConnectX-4 LX from 2020/2021 likely has old firmware. Update
before anything else — old firmware has bugs with newer kernels
and missing features.

### Get current firmware version

```bash
# After NIC is physically installed
lspci | grep Mellanox
# Note the PCI address (e.g., 03:00.0)

# Check current firmware
ethtool -i enp3s0f0  # or whatever the interface is called
# driver: mlx5_core
# firmware-version: XX.YY.ZZZZ

# Or via mstflint
mstflint -d 03:00.0 query
```

### Update firmware

```bash
# Install Mellanox firmware tools
# Debian/Ubuntu:
apt install mstflint

# Download latest firmware from NVIDIA/Mellanox:
# https://network.nvidia.com/support/firmware/connectx4lx/
# Select by part number (from NIC sticker)
# Current latest for CX4 LX is likely 14.32.xxxx or newer

# Flash (requires reboot after)
mstflint -d 03:00.0 -i fw-ConnectX4Lx-rel-XX_YY_ZZZZ-MCX4131A-BCA_Ax.bin burn

# Verify
mstflint -d 03:00.0 verify

# REBOOT required for new firmware to take effect
```

### Alternative: NVIDIA MLNX_OFED driver bundle

The MLNX_OFED package includes firmware + drivers + tools.
More comprehensive but heavier. For benchmarking, the inbox
kernel driver (mlx5_core) with updated firmware is sufficient.

```bash
# Only if you want the full Mellanox stack:
# Download from https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/
# ./mlnxofedinstall --force
# This replaces inbox drivers with Mellanox versions
```

**Recommendation**: start with inbox mlx5_core driver + firmware
update only. MLNX_OFED is overkill for TCP relay benchmarking
and can interfere with kernel kTLS.

## Step 2: Kernel Configuration

### Required modules

```bash
# mlx5 driver (should be built-in or auto-loaded)
modprobe mlx5_core

# kTLS support
modprobe tls

# Verify
lsmod | grep mlx5
lsmod | grep tls
```

### Kernel version

Debian 13 ships 6.12.x which is fine. Key features by version:
- 5.3+: kTLS TX offload for mlx5
- 5.15+: kTLS RX offload for mlx5
- 6.0+: io_uring SEND_ZC
- 6.1+: DEFER_TASKRUN, SINGLE_ISSUER
- 6.12: everything HD needs

### sysctl tuning

```bash
# Save to /etc/sysctl.d/99-benchmark.conf

# Socket buffer sizes — critical for 50GbE
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576
net.ipv4.tcp_rmem = 4096 1048576 16777216
net.ipv4.tcp_wmem = 4096 1048576 16777216

# Backlog and queue sizes
net.core.netdev_max_backlog = 250000
net.core.somaxconn = 65535

# TCP tuning
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_sack = 1
net.ipv4.tcp_no_metrics_save = 1
net.ipv4.tcp_congestion_control = bbr

# Memory pressure
net.ipv4.tcp_mem = 786432 1048576 26777216
net.core.optmem_max = 16777216

# Apply
sysctl -p /etc/sysctl.d/99-benchmark.conf
```

### kTLS (software — no NIC offload)

ConnectX-4 LX does NOT support TLS hardware offload. kTLS
runs in software (kernel AES-GCM, not NIC). This is intentional
— the benchmark story is "cheap off-the-rack hardware crushes
derper", not "expensive SmartNIC features."

```bash
# Load kTLS module
modprobe tls

# Verify
cat /proc/net/tls_stat
# TlsCurrTxSw should increment when connections are active
# TlsCurrTxDevice will stay at 0 (no HW offload)
```

The VM kTLS ceiling (~11 Gbps on 16 vCPU) was likely CPU-limited
by sharing cores between relay workers and crypto. On bare metal
with dedicated cores, software kTLS should push well past that.

## Step 3: Network Setup (Card-to-Card)

### Physical

- Install NIC in each workstation (PCIe x8 or x16 slot)
- Connect DAC cable between the two NICs
- No switch needed — direct connection

### IP configuration

Two DAC cables, two ports per card — use separate subnets
per port. Avoids kernel bonding overhead and switchless bonding
headaches. Bench client distributes connections across both IPs.

```bash
# Workstation A (relay):
ip addr add 10.50.0.1/24 dev enp3s0f0np0
ip addr add 10.50.1.1/24 dev enp3s0f0np1
ip link set enp3s0f0np0 up
ip link set enp3s0f0np1 up
ip link set enp3s0f0np0 mtu 9000
ip link set enp3s0f0np1 mtu 9000

# Workstation B (client):
ip addr add 10.50.0.2/24 dev enp3s0f0np0
ip addr add 10.50.1.2/24 dev enp3s0f0np1
ip link set enp3s0f0np0 up
ip link set enp3s0f0np1 up
ip link set enp3s0f0np0 mtu 9000
ip link set enp3s0f0np1 mtu 9000
```

For 50G aggregate: bench client opens half its connections to
10.50.0.1, half to 10.50.1.1. Each port carries up to 25G.
No bond driver, no reordering, fully deterministic.

For 25G single-port test: use only port 0, leave port 1 down.

### Verify link

```bash
# Check link is up and speed is correct
ethtool enp3s0f0
# Speed: 50000Mb/s (or 25000Mb/s for 25GbE cards)
# Link detected: yes

# Check connectivity
ping -c 5 10.50.0.2  # from relay
ping -c 5 10.50.0.1  # from client

# Quick throughput sanity check
# On relay:
iperf3 -s
# On client:
iperf3 -c 10.50.0.1 -t 10
# Should see ~40+ Gbps with jumbo frames
```

### MTU considerations

```bash
# Jumbo frames (9000) reduce per-packet overhead:
# 50 Gbps / 9000B = ~694K pps (manageable)
# 50 Gbps / 1500B = ~4.2M pps (much harder)
# 50 Gbps / 1400B = ~4.5M pps (WireGuard MTU — worst case)

# For benchmarking, test BOTH:
# - MTU 9000: shows relay ceiling without per-packet pressure
# - MTU 1500: realistic, matches cloud/internet path MTU
# HD payload is always 1400B regardless of NIC MTU, but lower
# MTU means more packets for the same throughput
```

## Step 4: NIC Tuning

### RSS (Receive Side Scaling)

Distributes incoming packets across CPU cores via NIC hardware
hashing. Critical for multi-worker relay performance.

```bash
# Check current RSS queue count
ethtool -l enp3s0f0
# Shows pre-set and current maximums

# Set RSS queues — match to number of relay workers or cores
# For initial testing, match worker count
ethtool -L enp3s0f0 combined 4  # if using 4 workers

# Verify RSS hash distribution
ethtool -x enp3s0f0
# Shows indirection table — should spread across queues
```

### IRQ affinity

By default, irqbalance moves interrupts around, causing
cache thrashing. Pin NIC interrupts to specific cores.

```bash
# Disable irqbalance
systemctl stop irqbalance
systemctl disable irqbalance

# Find NIC IRQs
grep enp3s0f0 /proc/interrupts
# Or:
ls /sys/class/net/enp3s0f0/device/msi_irqs/

# Pin each IRQ to a specific core
# If 4 RSS queues on cores 0-3:
echo 1 > /proc/irq/<irq0>/smp_affinity  # core 0
echo 2 > /proc/irq/<irq1>/smp_affinity  # core 1
echo 4 > /proc/irq/<irq2>/smp_affinity  # core 2
echo 8 > /proc/irq/<irq3>/smp_affinity  # core 3

# Or use the Mellanox set_irq_affinity.sh script:
# /usr/sbin/set_irq_affinity_bynode.sh 0 enp3s0f0
```

### Interrupt coalescing

Trade latency for throughput by batching interrupts.

```bash
# Check current settings
ethtool -c enp3s0f0

# For throughput benchmarks (higher coalescing):
ethtool -C enp3s0f0 adaptive-rx on adaptive-tx on
# Or manual:
ethtool -C enp3s0f0 rx-usecs 50 tx-usecs 50

# For latency benchmarks (lower coalescing):
ethtool -C enp3s0f0 rx-usecs 0 tx-usecs 0
# WARNING: this generates MANY interrupts at 50Gbps
```

### Ring buffer sizes

```bash
# Check max
ethtool -g enp3s0f0

# Set to max
ethtool -G enp3s0f0 rx 8192 tx 8192
```

## Step 5: CPU Pinning for HD Workers

On bare metal, pin relay workers to specific cores and keep
NIC IRQs on separate (or same, depending on test) cores.

```bash
# Example for 32-core machine, 4 workers:
# Cores 0-3: NIC IRQ processing
# Cores 4-7: HD workers (taskset or --cpu-affinity flag)
# Cores 8+: OS, accept thread, control plane

taskset -c 4-7 ./hyper-derp --workers 4 ...

# Or if HD supports affinity flags:
./hyper-derp --workers 4 --cpu-start 4 ...
```

### NUMA awareness

Single-socket E5-1650 v3 — only one NUMA node, so NUMA
pinning is not needed. All cores and memory are local.

## Step 6: Validation Before Benchmarking

### Baseline throughput

```bash
# Raw TCP throughput (no relay)
# Server: iperf3 -s
# Client: iperf3 -c 10.50.0.1 -t 30 -P 8
# Expect: 45-50 Gbps with jumbo frames, 35-45 Gbps at MTU 1500
```

### Baseline latency

```bash
# Raw TCP RTT (no relay)
# sockperf or netperf, not ping (ping uses ICMP, different path)
sockperf ping-pong -i 10.50.0.1 -p 12345 --tcp -t 10
# Expect: 10-30us RTT on direct DAC connection
```

### kTLS verification

```bash
# Start HD with TLS
# Check /proc/net/tls_stat before and after connections
cat /proc/net/tls_stat
# TlsCurrTxDevice should increment (hardware offload)
# If only TlsCurrTxSw increments, HW offload isn't working

# Also check HD startup log for BIO_get_ktls_send
```

### perf sanity check

```bash
# Verify perf works and has PMU access
perf stat -e cycles,instructions,cache-misses ls
# If "permission denied": set perf_event_paranoid
echo 1 > /proc/sys/kernel/perf_event_paranoid
```

## Step 7: Benchmark Plan

See FULL_SWEEP_DESIGN.md for methodology. Key differences
from VM testing:

1. **Higher rates**: test up to 25G offered (wire rate).
   Second port available for future bonded 50G tests.
2. **Per-packet budget**: at 25Gbps / 1400B = 2.2M pps = 450ns
   per packet. Profile with `perf` to find where cycles go.
3. **Worker count sweep**: 6C/12T limits practical range to
   2, 3, 4 workers (need cores for kernel, accept, control).
   Matches the 4-8 vCPU VM configs in core count.
4. **kTLS software**: no HW offload on CX4 LX, but dedicated
   cores for crypto should push past the VM ceiling (~11 Gbps).
5. **RSS alignment**: test whether aligning RSS queues to worker
   cores improves or hurts (fewer cross-core wakeups vs less
   flexible scheduling).

## Troubleshooting

### Link won't come up

```bash
# Check cable is seated properly (QSFP28 clicks in)
# Check for FEC mismatch
ethtool --show-fec enp3s0f0
# Try RS-FEC:
ethtool --set-fec enp3s0f0 encoding rs
# Or auto:
ethtool --set-fec enp3s0f0 encoding auto
```

### Low throughput

```bash
# Check for PCIe bandwidth bottleneck
lspci -vvv -s 03:00.0 | grep -i width
# Should show "Width x8" for CX4 LX
# If "Width x4" or x1 — NIC is in wrong slot

# Check for CPU frequency scaling
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
# Set to performance:
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Check for thermal throttling
sensors  # or watch -n1 cat /sys/class/thermal/thermal_zone*/temp
```

### kTLS not activating

```bash
# Check module is loaded
lsmod | grep tls

# Check kernel config
grep TLS /boot/config-$(uname -r)
# Needs CONFIG_TLS=y or CONFIG_TLS=m

# Check /proc/net/tls_stat under load
# TlsCurrTxSw should be > 0 when HD has active TLS connections
# If all zeros, check HD startup log for kTLS errors
```
