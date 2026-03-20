#!/usr/bin/env python3
"""Generate scaling comparison report and plots.

Reads per-pair-count JSON results and produces:
  - throughput_vs_pairs.png  (main chart, NetBird-style)
  - loss_vs_pairs.png
  - connect_time_vs_pairs.png
  - SCALING_REPORT.md
"""

import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULT_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    'bench_results/scaling-20260311'
OUT_DIR = RESULT_DIR

RELAYS = [
    ('Tailscale', 'ts', '#4A90D9'),
    ('HD (plain TCP)', 'hd', '#E5553B'),
    ('HD (kTLS)', 'ktls', '#2ECC71'),
]

PAIR_COUNTS = [1, 2, 5, 10, 25, 50, 100, 250, 500]


def load_json(path):
  try:
    with open(path) as f:
      return json.load(f)
  except (OSError, json.JSONDecodeError):
    return None


def load_all():
  """Load results for all relays and pair counts."""
  data = {}
  for label, prefix, _ in RELAYS:
    points = {}
    for pairs in PAIR_COUNTS:
      d = load_json(os.path.join(
          RESULT_DIR, f'{prefix}_{pairs}p.json'))
      if d:
        points[pairs] = d
    data[label] = points
  return data


def plot_throughput(data):
  """Throughput (Mbps) vs active peer pairs."""
  fig, ax = plt.subplots(figsize=(11, 6.5))
  ax.set_title(
      'Relay Throughput vs Active Peer Pairs\n'
      '(1400B payload, unlimited rate, veth bridge)',
      fontsize=13, fontweight='bold')

  for label, prefix, color in RELAYS:
    points = data[label]
    if not points:
      continue
    pairs = sorted(points.keys())
    tp = [points[p]['throughput_mbps'] for p in pairs]
    ax.plot(pairs, tp, marker='o', linewidth=2.5,
            markersize=7, color=color, label=label,
            zorder=3)

  ax.set_xlabel('Active Peer Pairs', fontsize=12)
  ax.set_ylabel('Throughput (Mbps)', fontsize=12)
  ax.set_xscale('log')
  ax.set_xlim(0.8, max(PAIR_COUNTS) * 1.5)
  ax.set_ylim(0, None)
  ax.legend(fontsize=11, loc='upper left')
  ax.grid(True, alpha=0.3, which='both')
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{int(v):,}'))
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'throughput_vs_pairs.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def plot_loss(data):
  """Message loss (%) vs active peer pairs."""
  fig, ax = plt.subplots(figsize=(11, 6.5))
  ax.set_title(
      'Message Loss vs Active Peer Pairs\n'
      '(1400B payload, unlimited rate)',
      fontsize=13, fontweight='bold')

  for label, prefix, color in RELAYS:
    points = data[label]
    if not points:
      continue
    pairs = sorted(points.keys())
    loss = [points[p]['message_loss_pct'] for p in pairs]
    ax.plot(pairs, loss, marker='s', linewidth=2,
            markersize=6, color=color, label=label)

  ax.set_xlabel('Active Peer Pairs', fontsize=12)
  ax.set_ylabel('Message Loss (%)', fontsize=12)
  ax.set_xscale('log')
  ax.legend(fontsize=11)
  ax.grid(True, alpha=0.3, which='both')
  ax.set_xlim(0.8, max(PAIR_COUNTS) * 1.5)
  ax.set_ylim(-1, None)
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{int(v):,}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'loss_vs_pairs.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def plot_connect_time(data):
  """Connect time (ms) vs peer count."""
  fig, ax = plt.subplots(figsize=(11, 6.5))
  ax.set_title(
      'Connection Setup Time vs Peer Count\n'
      '(sequential TCP connect + DERP handshake)',
      fontsize=13, fontweight='bold')

  for label, prefix, color in RELAYS:
    points = data[label]
    if not points:
      continue
    pairs = sorted(points.keys())
    ct = []
    peer_counts = []
    for p in pairs:
      d = points[p]
      if 'connect_time_ms' in d:
        ct.append(d['connect_time_ms'])
        peer_counts.append(d.get('total_peers', p * 2))
    if ct:
      ax.plot(peer_counts, ct, marker='^', linewidth=2,
              markersize=6, color=color, label=label)

  ax.set_xlabel('Total Peers', fontsize=12)
  ax.set_ylabel('Connect Time (ms)', fontsize=12)
  ax.set_xscale('log')
  ax.set_yscale('log')
  ax.legend(fontsize=11)
  ax.grid(True, alpha=0.3, which='both')
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{int(v):,}'))
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'connect_time.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def plot_throughput_bar(data):
  """Grouped bar chart at key pair counts."""
  key_pairs = [1, 10, 50, 100, 500]
  # Only include pair counts that have data for all relays.
  key_pairs = [p for p in key_pairs
               if all(p in data[label]
                      for label, _, _ in RELAYS
                      if data[label])]

  if not key_pairs:
    return None

  fig, ax = plt.subplots(figsize=(12, 6.5))
  ax.set_title(
      'Throughput by Relay Type at Key Scales\n'
      '(1400B payload, unlimited rate)',
      fontsize=13, fontweight='bold')

  x = np.arange(len(key_pairs))
  width = 0.25
  offsets = [-width, 0, width]

  for i, (label, prefix, color) in enumerate(RELAYS):
    points = data[label]
    vals = []
    for p in key_pairs:
      d = points.get(p)
      vals.append(d['throughput_mbps'] if d else 0)
    bars = ax.bar(x + offsets[i], vals, width,
                  label=label, color=color,
                  edgecolor='white', linewidth=0.5)
    for bar, v in zip(bars, vals):
      if v > 0:
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 20,
                f'{v:.0f}',
                ha='center', va='bottom', fontsize=8)

  ax.set_xlabel('Active Peer Pairs', fontsize=12)
  ax.set_ylabel('Throughput (Mbps)', fontsize=12)
  ax.set_xticks(x)
  ax.set_xticklabels([str(p) for p in key_pairs])
  ax.legend(fontsize=11)
  ax.grid(axis='y', alpha=0.3)
  ax.set_ylim(0, None)
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'throughput_bars.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def generate_markdown(data):
  """Generate SCALING_REPORT.md."""
  sysinfo = {}
  try:
    with open(os.path.join(RESULT_DIR,
                           'system_info.txt')) as f:
      for line in f:
        if ':' in line:
          k, v = line.split(':', 1)
          sysinfo[k.strip()] = v.strip()
  except OSError:
    pass

  lines = []
  lines.append('# Relay Scaling Comparison')
  lines.append('')
  lines.append('Tailscale (Go) vs Hyper-DERP (plain TCP) '
               'vs Hyper-DERP (kTLS)')
  lines.append('')
  lines.append('## Test Environment')
  lines.append('')
  lines.append(f'- **Date**: {sysinfo.get("date", "N/A")}')
  lines.append(f'- **CPU**: {sysinfo.get("cpu", "N/A")}')
  lines.append(f'- **Kernel**: '
               f'{sysinfo.get("kernel", "N/A")}')
  lines.append(f'- **Relay cores**: '
               f'{sysinfo.get("relay_cores", "N/A")} '
               f'({sysinfo.get("workers", "2")} workers)')
  lines.append(f'- **Client cores**: '
               f'{sysinfo.get("client_cores", "N/A")}')
  lines.append('- **Network**: veth on bridge')
  lines.append(f'- **Payload**: '
               f'{sysinfo.get("size", "1400B")}')
  lines.append(f'- **Duration**: '
               f'{sysinfo.get("duration", "5s")} per point')
  lines.append(f'- **Rate**: '
               f'{sysinfo.get("rate", "unlimited")}')
  lines.append('')

  # Main throughput chart.
  lines.append('## Throughput vs Peer Pairs')
  lines.append('')
  lines.append('![Throughput vs Pairs]'
               '(throughput_vs_pairs.png)')
  lines.append('')
  lines.append('![Throughput Bars](throughput_bars.png)')
  lines.append('')

  # Table.
  lines.append('| Pairs | TS Mbps | TS Loss | '
               'HD Mbps | HD Loss | '
               'kTLS Mbps | kTLS Loss | HD/TS |')
  lines.append('|------:|--------:|--------:|'
               '--------:|--------:|'
               '----------:|----------:|------:|')

  all_pairs = sorted(set(
      p for label in data
      for p in data[label].keys()))

  for pairs in all_pairs:
    ts = data.get('Tailscale', {}).get(pairs)
    hd = data.get('HD (plain TCP)', {}).get(pairs)
    kt = data.get('HD (kTLS)', {}).get(pairs)
    ts_tp = ts['throughput_mbps'] if ts else 0
    ts_l = (f'{ts["message_loss_pct"]:.1f}%'
            if ts else '-')
    hd_tp = hd['throughput_mbps'] if hd else 0
    hd_l = (f'{hd["message_loss_pct"]:.1f}%'
            if hd else '-')
    kt_tp = kt['throughput_mbps'] if kt else 0
    kt_l = (f'{kt["message_loss_pct"]:.1f}%'
            if kt else '-')
    ratio = f'{hd_tp / ts_tp:.1f}x' if ts_tp > 0 else '-'
    lines.append(
        f'| {pairs:,} | {ts_tp:.0f} | {ts_l} | '
        f'{hd_tp:.0f} | {hd_l} | '
        f'{kt_tp:.0f} | {kt_l} | {ratio} |')

  lines.append('')
  lines.append('![Loss vs Pairs](loss_vs_pairs.png)')
  lines.append('')
  lines.append('![Connect Time](connect_time.png)')
  lines.append('')

  # Analysis.
  lines.append('## Analysis')
  lines.append('')

  # Find peak values.
  hd_points = data.get('HD (plain TCP)', {})
  kt_points = data.get('HD (kTLS)', {})
  ts_points = data.get('Tailscale', {})

  if hd_points:
    hd_peak = max(hd_points.values(),
                  key=lambda d: d['throughput_mbps'])
    lines.append(
        f'- **HD peak**: '
        f'{hd_peak["throughput_mbps"]:.0f} Mbps')
  if kt_points:
    kt_peak = max(kt_points.values(),
                  key=lambda d: d['throughput_mbps'])
    lines.append(
        f'- **kTLS peak**: '
        f'{kt_peak["throughput_mbps"]:.0f} Mbps')
  if ts_points:
    ts_peak = max(ts_points.values(),
                  key=lambda d: d['throughput_mbps'])
    lines.append(
        f'- **TS peak**: '
        f'{ts_peak["throughput_mbps"]:.0f} Mbps')

  if hd_points and ts_points:
    # Compare at 10 pairs.
    hd_10 = hd_points.get(10)
    ts_10 = ts_points.get(10)
    if hd_10 and ts_10 and ts_10['throughput_mbps'] > 0:
      ratio = (hd_10['throughput_mbps'] /
               ts_10['throughput_mbps'])
      lines.append(
          f'- **HD/TS at 10 pairs**: {ratio:.1f}x')

  lines.append('')

  # kTLS vs HD comparison.
  if hd_points and kt_points:
    lines.append('### kTLS vs Plain TCP')
    lines.append('')
    lines.append(
        'kTLS adds software AES-GCM encryption overhead '
        'on veth (no NIC offload). However, TLS record '
        'framing provides natural TCP backpressure, '
        'reducing loss under overload.')
    lines.append('')

  lines.append('### Caveats')
  lines.append('')
  lines.append(
      '1. **Test tool limitation**: The pthread-based '
      'client uses 2 threads per pair (sender + receiver). '
      'Above ~250 pairs, thread scheduling overhead '
      'degrades client-side throughput.')
  lines.append(
      '2. **Loss numbers**: Senders run at unlimited rate '
      'without backpressure. Loss reflects relay capacity, '
      'not reliability.')
  lines.append(
      '3. **veth network**: No NIC TLS offload. '
      'kTLS throughput is CPU-bound by software AES-GCM.')
  lines.append('')

  return '\n'.join(lines)


def main():
  data = load_all()

  total = sum(len(v) for v in data.values())
  print(f'Loaded {total} data points from {RESULT_DIR}')

  if total == 0:
    print('No data found. Run run_scaling_sweep.sh first.')
    sys.exit(1)

  print('Generating plots...')
  plot_throughput(data)
  plot_loss(data)
  plot_connect_time(data)
  plot_throughput_bar(data)

  print('Generating markdown...')
  md = generate_markdown(data)
  md_path = os.path.join(OUT_DIR, 'SCALING_REPORT.md')
  with open(md_path, 'w') as f:
    f.write(md)
  print(f'Report: {md_path}')


if __name__ == '__main__':
  main()
