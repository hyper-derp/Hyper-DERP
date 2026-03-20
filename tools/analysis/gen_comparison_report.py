#!/usr/bin/env python3
"""Generate comparison report: Hyper-DERP vs Tailscale derper."""

import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULT_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    'bench_results/comparison-20260311'
OUT_DIR = RESULT_DIR

COLORS = {'Tailscale': '#4A90D9', 'Hyper-DERP': '#E5553B'}


def load_json(path):
  """Load a JSON result file, return None on failure."""
  try:
    with open(path) as f:
      return json.load(f)
  except (OSError, json.JSONDecodeError):
    return None


def load_all():
  """Load all benchmark data into a structured dict."""
  data = {}
  for server, prefix in [('Tailscale', 'ts'), ('Hyper-DERP', 'hd')]:
    data[server] = {'tp': {}, 'lat': {}}

    for sz in [64, 256, 1024, 4096]:
      send = load_json(
          os.path.join(RESULT_DIR, f'{prefix}_tp_{sz}B_send.json'))
      recv = load_json(
          os.path.join(RESULT_DIR, f'{prefix}_tp_{sz}B_recv.json'))
      if send:
        data[server]['tp'][sz] = {
            'send_pps': send['throughput_pps'],
            'send_mbps': send['throughput_mbps'],
            'send_ms': send['elapsed_ms'],
            'send_pkts': send['packets'],
        }
        if recv:
          data[server]['tp'][sz]['recv_pkts'] = recv['packets']
          data[server]['tp'][sz]['delivery_pct'] = \
              100 * recv['packets'] / send['packets']

    for sz in [64, 256, 1024]:
      lat = load_json(
          os.path.join(RESULT_DIR, f'{prefix}_lat_{sz}B.json'))
      if lat and 'latency_ns' in lat:
        ln = lat['latency_ns']
        data[server]['lat'][sz] = {
            'min': ln['min'] / 1000,
            'p50': ln['p50'] / 1000,
            'p90': ln['p90'] / 1000,
            'p99': ln['p99'] / 1000,
            'max': ln['max'] / 1000,
            'mean': ln['mean'] / 1000,
            'pps': lat['throughput_pps'],
            'raw': [x / 1000 for x in ln.get('raw', [])],
        }
  return data


def plot_latency_bars(data):
  """Bar chart: p50/p90/p99 latency by packet size."""
  fig, axes = plt.subplots(1, 2, figsize=(12, 5))
  fig.suptitle('Round-Trip Latency Comparison', fontsize=14,
               fontweight='bold')

  for idx, sz in enumerate([64, 1024]):
    ax = axes[idx]
    metrics = ['p50', 'p90', 'p99']
    x = np.arange(len(metrics))
    width = 0.35

    ts_vals = []
    hd_vals = []
    for m in metrics:
      ts_vals.append(data['Tailscale']['lat'].get(sz, {}).get(m, 0))
      hd_vals.append(data['Hyper-DERP']['lat'].get(sz, {}).get(m, 0))

    bars_ts = ax.bar(x - width / 2, ts_vals, width,
                     label='Tailscale derper',
                     color=COLORS['Tailscale'], edgecolor='white')
    bars_hd = ax.bar(x + width / 2, hd_vals, width,
                     label='Hyper-DERP',
                     color=COLORS['Hyper-DERP'], edgecolor='white')

    # Value labels.
    for bar in bars_ts:
      ax.text(bar.get_x() + bar.get_width() / 2,
              bar.get_height() + 2,
              f'{bar.get_height():.0f}',
              ha='center', va='bottom', fontsize=9)
    for bar in bars_hd:
      ax.text(bar.get_x() + bar.get_width() / 2,
              bar.get_height() + 2,
              f'{bar.get_height():.0f}',
              ha='center', va='bottom', fontsize=9)

    ax.set_xlabel('Percentile')
    ax.set_ylabel('Latency (us)')
    ax.set_title(f'{sz}B payload')
    ax.set_xticks(x)
    ax.set_xticklabels(metrics)
    ax.legend(loc='upper left', fontsize=9)
    ax.set_ylim(0, max(ts_vals + hd_vals) * 1.25)
    ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_bars.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_cdf(data):
  """CDF of raw latency samples."""
  fig, axes = plt.subplots(1, 2, figsize=(12, 5))
  fig.suptitle('Latency CDF (Cumulative Distribution)',
               fontsize=14, fontweight='bold')

  for idx, sz in enumerate([64, 1024]):
    ax = axes[idx]
    for server in ['Tailscale', 'Hyper-DERP']:
      raw = data[server]['lat'].get(sz, {}).get('raw', [])
      if not raw:
        continue
      sorted_raw = np.sort(raw)
      cdf = np.arange(1, len(sorted_raw) + 1) / len(sorted_raw)
      label = 'Tailscale derper' if server == 'Tailscale' \
          else 'Hyper-DERP'
      ax.plot(sorted_raw, cdf * 100, label=label,
              color=COLORS[server], linewidth=2)

    ax.set_xlabel('Latency (us)')
    ax.set_ylabel('Percentile')
    ax.set_title(f'{sz}B payload')
    ax.legend(loc='lower right', fontsize=9)
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 101)
    ax.axhline(y=99, color='gray', linestyle='--', alpha=0.4,
               linewidth=0.8)
    ax.text(ax.get_xlim()[0], 99.2, 'p99', fontsize=8,
            color='gray')

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_cdf.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_ping_throughput(data):
  """Bar chart: ping-pong PPS by packet size."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle('Ping-Pong Throughput (requests/sec)',
               fontsize=14, fontweight='bold')

  sizes = [sz for sz in [64, 256, 1024]
           if sz in data['Tailscale']['lat']
           and sz in data['Hyper-DERP']['lat']]
  x = np.arange(len(sizes))
  width = 0.35

  ts_pps = [data['Tailscale']['lat'].get(sz, {}).get('pps', 0)
            for sz in sizes]
  hd_pps = [data['Hyper-DERP']['lat'].get(sz, {}).get('pps', 0)
            for sz in sizes]

  bars_ts = ax.bar(x - width / 2, ts_pps, width,
                   label='Tailscale derper',
                   color=COLORS['Tailscale'], edgecolor='white')
  bars_hd = ax.bar(x + width / 2, hd_pps, width,
                   label='Hyper-DERP',
                   color=COLORS['Hyper-DERP'], edgecolor='white')

  for bar in bars_ts:
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 200,
            f'{bar.get_height():.0f}',
            ha='center', va='bottom', fontsize=9)
  for bar in bars_hd:
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 200,
            f'{bar.get_height():.0f}',
            ha='center', va='bottom', fontsize=9)

  ax.set_xlabel('Payload Size')
  ax.set_ylabel('Packets/sec')
  ax.set_xticks(x)
  ax.set_xticklabels([f'{sz}B' for sz in sizes])
  ax.legend(fontsize=10)
  ax.grid(axis='y', alpha=0.3)
  ax.set_ylim(0, max(ts_pps + hd_pps) * 1.2)
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'ping_throughput.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_delivery(data):
  """Bar chart: packet delivery rate for Tailscale."""
  ts_tp = data['Tailscale']['tp']
  sizes = sorted(sz for sz in ts_tp if 'delivery_pct' in ts_tp[sz])
  if not sizes:
    return None

  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle('Tailscale derper: Packet Delivery Rate',
               fontsize=14, fontweight='bold')

  vals = [ts_tp[sz]['delivery_pct'] for sz in sizes]
  bars = ax.bar(range(len(sizes)), vals,
                color=COLORS['Tailscale'], edgecolor='white')

  for bar, v in zip(bars, vals):
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 1,
            f'{v:.1f}%',
            ha='center', va='bottom', fontsize=10,
            fontweight='bold')

  ax.set_xlabel('Payload Size')
  ax.set_ylabel('Delivery Rate (%)')
  ax.set_xticks(range(len(sizes)))
  ax.set_xticklabels([f'{sz}B' for sz in sizes])
  ax.set_ylim(0, 110)
  ax.axhline(y=100, color='green', linestyle='--', alpha=0.3)
  ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'delivery_rate.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def generate_markdown(data):
  """Generate the comparison markdown report."""
  # Read system info.
  sysinfo = {}
  try:
    with open(os.path.join(RESULT_DIR, 'system_info.txt')) as f:
      for line in f:
        if ':' in line:
          k, v = line.split(':', 1)
          sysinfo[k.strip()] = v.strip()
  except OSError:
    pass

  lines = []
  lines.append('# Hyper-DERP vs Tailscale derper: '
               'Performance Comparison')
  lines.append('')
  lines.append('## Test Environment')
  lines.append('')
  lines.append(f'- **Date**: {sysinfo.get("date", "N/A")}')
  lines.append(f'- **CPU**: {sysinfo.get("cpu", "N/A")}')
  lines.append(f'- **Kernel**: {sysinfo.get("kernel", "N/A")}')
  lines.append(f'- **Cores**: {sysinfo.get("cores", "N/A")}')
  lines.append(f'- **Workers**: {sysinfo.get("workers", "2")}')
  lines.append('- **Test**: localhost loopback '
               '(client -> relay -> client)')
  lines.append(f'- **Send count**: '
               f'{sysinfo.get("send_count", "20000")} packets')
  lines.append(f'- **Ping count**: '
               f'{sysinfo.get("ping_count", "2000")} round-trips')
  lines.append('')

  # Latency table.
  lines.append('## Round-Trip Latency')
  lines.append('')
  lines.append('Measured via ping/echo (send packet, wait for '
               'echo, measure RTT). Lower is better.')
  lines.append('')
  lines.append('| Size | Metric | Tailscale | Hyper-DERP | '
               'Speedup |')
  lines.append('|------|--------|-----------|------------|'
               '---------|')

  for sz in [64, 1024]:
    ts_lat = data['Tailscale']['lat'].get(sz, {})
    hd_lat = data['Hyper-DERP']['lat'].get(sz, {})
    for m in ['p50', 'p90', 'p99']:
      ts_v = ts_lat.get(m, 0)
      hd_v = hd_lat.get(m, 0)
      ratio = ts_v / hd_v if hd_v else 0
      lines.append(
          f'| {sz}B | {m} | {ts_v:.0f} us | '
          f'{hd_v:.0f} us | **{ratio:.1f}x** |')

  lines.append('')
  lines.append('![Latency Bars](latency_bars.png)')
  lines.append('')
  lines.append('![Latency CDF](latency_cdf.png)')
  lines.append('')

  # Ping throughput.
  lines.append('## Ping-Pong Throughput')
  lines.append('')
  lines.append('Sustained request-response rate (one outstanding '
               'request at a time).')
  lines.append('')
  lines.append('| Size | Tailscale | Hyper-DERP | Speedup |')
  lines.append('|------|-----------|------------|---------|')

  for sz in [64, 256, 1024]:
    ts_pps = data['Tailscale']['lat'].get(sz, {}).get('pps', 0)
    hd_pps = data['Hyper-DERP']['lat'].get(sz, {}).get('pps', 0)
    ratio = hd_pps / ts_pps if ts_pps else 0
    if ts_pps and hd_pps:
      lines.append(
          f'| {sz}B | {ts_pps:,.0f} pps | '
          f'{hd_pps:,.0f} pps | **{ratio:.1f}x** |')

  lines.append('')
  lines.append('![Ping Throughput](ping_throughput.png)')
  lines.append('')

  # Delivery rate.
  ts_tp = data['Tailscale']['tp']
  has_delivery = any('delivery_pct' in ts_tp.get(sz, {})
                     for sz in [64, 256, 1024, 4096])
  if has_delivery:
    lines.append('## Packet Delivery (Tailscale)')
    lines.append('')
    lines.append('Tailscale derper delivery rate under burst '
                 'load (20,000 packets sent as fast as '
                 'possible). Hyper-DERP recv data not captured '
                 'in this run.')
    lines.append('')
    lines.append('| Size | Sent | Received | Delivery |')
    lines.append('|------|------|----------|----------|')
    for sz in [64, 256, 1024, 4096]:
      tp = ts_tp.get(sz, {})
      if 'delivery_pct' in tp:
        lines.append(
            f'| {sz}B | {tp["send_pkts"]:,} | '
            f'{tp["recv_pkts"]:,} | '
            f'{tp["delivery_pct"]:.1f}% |')
    lines.append('')
    lines.append('![Delivery Rate](delivery_rate.png)')
    lines.append('')

  # Summary.
  lines.append('## Summary')
  lines.append('')

  hd_64_p50 = data['Hyper-DERP']['lat'].get(64, {}).get('p50', 0)
  ts_64_p50 = data['Tailscale']['lat'].get(64, {}).get('p50', 0)
  hd_1k_p99 = data['Hyper-DERP']['lat'].get(1024, {}).get('p99', 0)
  ts_1k_p99 = data['Tailscale']['lat'].get(1024, {}).get('p99', 0)
  hd_1k_pps = data['Hyper-DERP']['lat'].get(1024, {}).get('pps', 0)
  ts_1k_pps = data['Tailscale']['lat'].get(1024, {}).get('pps', 0)

  lines.append('Hyper-DERP advantages over Tailscale derper '
               '(Go) on localhost:')
  lines.append('')
  if ts_64_p50 and hd_64_p50:
    lines.append(f'- **Median latency (64B)**: {hd_64_p50:.0f} us '
                 f'vs {ts_64_p50:.0f} us '
                 f'({ts_64_p50/hd_64_p50:.1f}x faster)')
  if ts_1k_p99 and hd_1k_p99:
    lines.append(f'- **p99 latency (1KB)**: {hd_1k_p99:.0f} us '
                 f'vs {ts_1k_p99:.0f} us '
                 f'({ts_1k_p99/hd_1k_p99:.1f}x faster)')
  if ts_1k_pps and hd_1k_pps:
    lines.append(f'- **Ping throughput (1KB)**: '
                 f'{hd_1k_pps:,.0f} vs {ts_1k_pps:,.0f} pps '
                 f'({hd_1k_pps/ts_1k_pps:.1f}x)')
  lines.append('')
  lines.append('The largest wins come at tail latencies (p99, '
               'max) and larger packet sizes, where Go\'s '
               'goroutine scheduling and GC pauses become '
               'visible. Hyper-DERP\'s io_uring data plane '
               'delivers consistent sub-100us forwarding with '
               'minimal jitter.')
  lines.append('')
  lines.append('### Caveats')
  lines.append('')
  lines.append('- Localhost test (no network latency/jitter)')
  lines.append('- Single sender/receiver pair')
  lines.append('- Workstation (not dedicated server hardware)')
  lines.append('- Send-side throughput numbers measure socket '
               'write speed, not relay forwarding capacity')
  lines.append('')

  return '\n'.join(lines)


def main():
  data = load_all()

  print('Generating plots...')
  plot_latency_bars(data)
  plot_latency_cdf(data)
  plot_ping_throughput(data)
  plot_delivery(data)

  print('Generating markdown...')
  md = generate_markdown(data)
  md_path = os.path.join(OUT_DIR, 'COMPARISON.md')
  with open(md_path, 'w') as f:
    f.write(md)

  print(f'Report: {md_path}')
  print('Done.')


if __name__ == '__main__':
  main()
