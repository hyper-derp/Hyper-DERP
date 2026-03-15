# AWS Sweep Plan — c7i Instance Replication

## Goal

Replicate the GCP Phase B (kTLS) benchmark on AWS to prove
the HD advantage is platform-independent. Same methodology,
different cloud. "Same code, same configuration, different
infrastructure — the advantage holds."

## Instance Selection

### GCP → AWS Mapping

| GCP | AWS | vCPU | Sustained BW | Notes |
|-----|-----|-----:|-------------:|-------|
| c4-highcpu-2 | c7i.large | 2 | Up to 12.5G | Burst, baseline lower |
| c4-highcpu-4 | c7i.xlarge | 4 | Up to 12.5G | Baseline ~2.5 Gbps |
| c4-highcpu-8 | c7i.2xlarge | 8 | Up to 12.5G | Baseline ~5 Gbps |
| c4-highcpu-16 | c7i.4xlarge | 16 | Up to 25G | Best match |

**CPU:** c7i uses Intel Xeon Sapphire Rapids (4th Gen) — same
generation as GCP c4. Direct comparison is fair.

### AWS Bandwidth Constraints

**Critical difference from GCP:** AWS ENA bandwidth is tied to
instance size and has baseline vs burst rates.

| Instance | Baseline BW | Burst BW | Burst Duration |
|----------|------------:|---------:|---------------:|
| c7i.large | ~0.78 Gbps | 12.5 Gbps | Limited credits |
| c7i.xlarge | ~1.25 Gbps | 12.5 Gbps | Limited credits |
| c7i.2xlarge | ~2.5 Gbps | 12.5 Gbps | Limited credits |
| c7i.4xlarge | ~5 Gbps | 25 Gbps | Sustained |

This means:
- **c7i.large and c7i.xlarge**: sustained tests above baseline
  will drain burst credits and hit the bandwidth shaper. 15-second
  runs may be fine (within burst window), but 25 consecutive
  high-rate runs will deplete credits mid-sweep.
- **c7i.2xlarge**: baseline 2.5 Gbps, burst to 12.5 Gbps. More
  headroom but still shaper-limited at high rates.
- **c7i.4xlarge**: 5 Gbps sustained, 25 Gbps burst. Best for
  clean comparison.

### Mitigation

- Run high-rate tests with **gaps between runs** (30s cooldown)
  to allow burst credit recovery. This extends sweep time but
  avoids the shaper corrupting results.
- Alternatively, use **c7i.metal** (128 vCPU, 50 Gbps sustained)
  and restrict HD workers to match the vCPU count being tested.
  Expensive but eliminates the bandwidth variable entirely.
- **Document bandwidth credits** in the report. Include
  `aws ec2 describe-instance-types` output showing baseline BW.
- Monitor `EBSIOBalance%` and `NetworkBandwidthIn/Out` CloudWatch
  metrics during tests.

### Recommendation

Start with c7i.4xlarge (16 vCPU). It has the most bandwidth
headroom and matches the GCP config where we have the most data.
If budget allows, do c7i.2xlarge (8 vCPU). Skip c7i.large and
c7i.xlarge unless you can confirm burst credits last through the
full sweep — the bandwidth shaper would corrupt the data.

## Region and Network

- **Region**: eu-central-1 (Frankfurt) — matches GCP europe-west3
  for latency baseline comparison
- **AZ**: same AZ for relay and client VMs
- **Placement group**: cluster strategy for lowest inter-VM latency
- **VPC**: dedicated VPC with internal subnet (10.20.0.0/24)
- **Security group**: allow all TCP/UDP between relay and client
  SG, deny all external
- **Enhanced networking**: ENA enabled (default on c7i)

```bash
# Create placement group
aws ec2 create-placement-group \
  --group-name hd-bench \
  --strategy cluster

# Launch in same placement group
aws ec2 run-instances \
  --placement "GroupName=hd-bench" \
  ...
```

## Configurations

### Phase B Only (kTLS)

No need to rerun Phase A (plain TCP) on AWS. The GCP Phase A
data established the architectural properties (worker scaling,
backpressure, xfer_drops). AWS tests are about proving the
production (kTLS) advantage is platform-independent.

| vCPU | Instance | Workers | Est. TS TLS Ceiling |
|-----:|----------|--------:|--------------------:|
| 16 | c7i.4xlarge | 8 | ~7-8 Gbps |
| 8 | c7i.2xlarge | 4 | ~3.5-4.5 Gbps |

4 and 2 vCPU only if bandwidth credits are confirmed viable.

## Rate Selection

Adjusted for AWS bandwidth caps:

| vCPU | Rates |
|-----:|-------|
| 16 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G, 20G |
| 8 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G |

Cap rates below the instance's burst bandwidth. If burst
credits deplete during a sweep, you'll see throughput suddenly
drop on both HD and TS equally — that's the shaper, not the
relay.

## Methodology

Same as GCP Phase B:
- 25 runs at high rates, 5 at low rates
- 15 seconds per run
- TS ceiling probe first (3 runs × 5 rates)
- Latency: 6 load levels scaled to TS TLS ceiling
- `modprobe tls` on relay, verify kTLS active
- Worker stats via --metrics-port 9090

### Additional AWS-specific collection

```bash
# Instance metadata
curl -s http://169.254.169.254/latest/meta-data/instance-type
curl -s http://169.254.169.254/latest/meta-data/placement/availability-zone

# Network baseline
ethtool -i ens5  # ENA driver version
aws ec2 describe-instance-types \
  --instance-types c7i.4xlarge \
  --query "InstanceTypes[0].NetworkInfo" \
  --output json

# Monitor during test
aws cloudwatch get-metric-statistics \
  --namespace AWS/EC2 \
  --metric-name NetworkIn \
  --dimensions Name=InstanceId,Value=<id> \
  --start-time <start> --end-time <end> \
  --period 60 --statistics Average
```

## Comparison Strategy

### Matched by vCPU

Same vCPU count, different cloud:

| Metric | GCP c4 16 vCPU | AWS c7i 16 vCPU |
|--------|:-:|:-:|
| HD kTLS peak | X Gbps | Y Gbps |
| TS TLS ceiling | X Gbps | Y Gbps |
| HD/TS ratio | 1.6x | ?x |
| p99 at ceiling | Xus | Yus |

If ratios are similar across clouds, the advantage is
architectural, not platform-specific.

### What if AWS numbers differ?

- **HD ratio similar**: confirms architectural advantage.
  Different absolute numbers are expected (different hypervisor,
  different NIC virtualization).
- **HD ratio higher on AWS**: ENA may be less efficient than
  gVNIC, hurting Go's userspace TLS more. Good for HD.
- **HD ratio lower on AWS**: ENA may have optimizations that
  help Go. Report honestly.

## Cost Estimate

| Instance | On-demand/hr | Sweep time | Cost |
|----------|------------:|-----------:|-----:|
| c7i.4xlarge (relay) | ~$0.71 | ~3.5 hrs | ~$2.50 |
| c7i.2xlarge (relay) | ~$0.36 | ~3 hrs | ~$1.10 |
| Client VM (c7i.2xlarge) | ~$0.36 | ~6.5 hrs | ~$2.35 |
| **Total** | | | **~$5.95** |

On-demand — not worth the complexity of spot for a $6 run.

**Egress warning:** all traffic MUST stay within the VPC.
Internal IPs only. No public IP routing for bench traffic.
Verify security group rules before starting.

## Scripts

Reuse `tools/run_phase_b.sh` unchanged — it runs on the client
VM and doesn't know about the cloud provider.

New: `tools/phase_b_aws.sh` (to create) — equivalent of
`phase_b_gcp.sh` but using AWS CLI:
- Launch instances in placement group
- Deploy binaries via scp
- Resize by terminating and relaunching (AWS doesn't support
  live resize like GCP)
- Download results
- Terminate instances

### Key differences from GCP orchestrator

- No live VM resize — must terminate and relaunch per config.
  Pre-bake an AMI with all dependencies (Go, HD binary,
  Tailscale, certs, sysctl tuning) to speed up launches.
- Spot instance request handling (use `--instance-market-options`)
- Placement group management
- Security group setup
- EBS volume for results persistence across instance changes

## Pre-flight Checklist

- [ ] AWS CLI configured with credentials
- [ ] VPC + subnet + security group created
- [ ] Placement group created
- [ ] AMI baked with: HD binary, Go derper, bench client,
      certs, sysctl config, TLS module
- [ ] Spot instance pricing checked for eu-central-1
- [ ] Verify ENA driver supports kTLS (`modprobe tls` on
      Amazon Linux or Debian on AWS)
- [ ] Test connectivity: client → relay, internal IPs only
- [ ] Verify no egress charges (all traffic internal)
- [ ] Dry run: 1 rate, 1 run, both servers on c7i.4xlarge

## Data Organization

```
bench_results/aws-c7i-phase-b/
  16vcpu/
    system_info.txt
    aws_instance_info.json
    rate/
    latency/
    workers/
    cpu/
    gc_trace/
    tls_stat/
    DONE
  8vcpu/
    ...
  REPORT.md
  plots/
```

## Timeline

1. Set up AWS infrastructure (VPC, SG, AMI): ~2 hours
2. Write phase_b_aws.sh: ~1 hour (adapt from phase_b_gcp.sh)
3. Dry run on c7i.4xlarge: ~30 min
4. Full sweep 16 vCPU: ~3.5 hours
5. Full sweep 8 vCPU: ~3 hours
6. Analysis and report: ~1 hour

**Total: ~11 hours** (can be split across days).
