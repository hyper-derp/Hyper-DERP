#!/usr/bin/env python3
"""Generate GCP c4 benchmark report: Hyper-DERP vs Tailscale derper.

Reads scale test JSON from the gcp-c4 results directory.
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
    'bench_results/gcp-c4-20260313'
OUT_DIR = RESULT_DIR

COLORS = {'Tailscale': '#4A90D9', 'Hyper-DERP': '#E5553B'}
RATES = [10, 50, 100, 250, 500, 1000, 2000, 3000,
         5000, 7500, 10000, 15000, 20000]


def load_json(path):
  """Load a JSON result file, return None on failure."""
  try:
    with open(path) as f:
      return json.load(f)
  except (OSError, json.JSONDecodeError):
    return None


def load_scale_data():
  """Load throughput-vs-rate scale test results."""
  data = {}
  for server, prefix in [('Tailscale', 'ts'), ('Hyper-DERP', 'hd')]:
    data[server] = {}
    for rate in RATES:
      d = load_json(
          os.path.join(RESULT_DIR, f'{prefix}_{rate}mbps.json'))
      if d:
        data[server][rate] = d
  return data


def plot_throughput(scale):
  """Line chart: delivered throughput vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  fig.suptitle(
      'Delivered Throughput vs Offered Rate\n'
      '(20 peers, 10 active pairs, 1400B, 10s, '
      'GCP c4-highcpu-16)',
      fontsize=13, fontweight='bold')

  for server in ['Tailscale', 'Hyper-DERP']:
    rates_plot = []
    tp_plot = []
    for rate in RATES:
      d = scale[server].get(rate)
      if d:
        rates_plot.append(rate)
        tp_plot.append(d['throughput_mbps'])
    label = 'Tailscale derper' if server == 'Tailscale' \
        else 'Hyper-DERP'
    ax.plot(rates_plot, tp_plot, 'o-', label=label,
            color=COLORS[server], linewidth=2, markersize=6)

  # Ideal line (rate / ~2 for relay overhead).
  ideal_rates = [10, 20000]
  ideal_tp = [r * 0.81 for r in ideal_rates]
  ax.plot(ideal_rates, ideal_tp, '--', color='gray',
          alpha=0.4, linewidth=1, label='Ideal (~81% of offered)')

  ax.set_xlabel('Offered Rate (Mbps aggregate)')
  ax.set_ylabel('Delivered Throughput (Mbps)')
  ax.set_xscale('log')
  ax.set_yscale('log')
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3, which='both')
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'throughput.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_loss(scale):
  """Line chart: message loss % vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  fig.suptitle(
      'Message Loss vs Offered Rate\n'
      '(20 peers, 10 active pairs, 1400B, 10s, '
      'GCP c4-highcpu-16)',
      fontsize=13, fontweight='bold')

  for server in ['Tailscale', 'Hyper-DERP']:
    rates_plot = []
    loss_plot = []
    for rate in RATES:
      d = scale[server].get(rate)
      if d and d['messages_sent'] > 0:
        rates_plot.append(rate)
        loss_plot.append(d['message_loss_pct'])
    label = 'Tailscale derper' if server == 'Tailscale' \
        else 'Hyper-DERP'
    ax.plot(rates_plot, loss_plot, 'o-', label=label,
            color=COLORS[server], linewidth=2, markersize=6)

  ax.set_xlabel('Offered Rate (Mbps aggregate)')
  ax.set_ylabel('Message Loss (%)')
  ax.set_xscale('log')
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3, which='both')
  ax.set_ylim(-1, 35)
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))
  ax.axhline(y=1, color='orange', linestyle='--', alpha=0.5,
             linewidth=1)
  ax.text(RATES[0], 1.5, '1% loss threshold', fontsize=8,
          color='orange')

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'loss.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_ratio(scale):
  """Bar chart: HD/TS throughput ratio at each rate."""
  fig, ax = plt.subplots(figsize=(10, 5))
  fig.suptitle(
      'Hyper-DERP / Tailscale Throughput Ratio\n'
      '(>1.0 = HD faster)',
      fontsize=13, fontweight='bold')

  rates_plot = []
  ratios = []
  for rate in RATES:
    ts = scale['Tailscale'].get(rate, {})
    hd = scale['Hyper-DERP'].get(rate, {})
    ts_tp = ts.get('throughput_mbps', 0)
    hd_tp = hd.get('throughput_mbps', 0)
    if ts_tp > 0 and hd_tp > 0:
      rates_plot.append(str(rate))
      ratios.append(hd_tp / ts_tp)

  colors = ['#E5553B' if r > 1.0 else '#4A90D9' for r in ratios]
  ax.bar(rates_plot, ratios, color=colors, edgecolor='white')
  ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=1)

  for i, (r, v) in enumerate(zip(rates_plot, ratios)):
    ax.text(i, v + 0.02, f'{v:.2f}x', ha='center',
            fontsize=8, fontweight='bold')

  ax.set_xlabel('Offered Rate (Mbps)')
  ax.set_ylabel('HD/TS Ratio')
  ax.set_ylim(0.8, max(ratios) * 1.15)
  ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'ratio.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def generate_markdown(scale):
  """Generate the GCP benchmark markdown report."""
  lines = []
  lines.append('# Hyper-DERP vs Tailscale derper: '
               'GCP c4-highcpu-16 Benchmark')
  lines.append('')

  # System info.
  info_path = os.path.join(RESULT_DIR, 'system_info.txt')
  if os.path.exists(info_path):
    with open(info_path) as f:
      info = f.read()
  else:
    info = ''

  lines.append('## Test Environment')
  lines.append('')
  lines.append('- **Date**: 2026-03-13')
  lines.append('- **CPU**: Intel Xeon Platinum 8581C @ 2.30 GHz')
  lines.append('- **Kernel**: 6.12.73+deb13-cloud-amd64')
  lines.append('- **Relay VM**: c4-highcpu-16 (16 vCPU, 32 GB)')
  lines.append('- **Client VM**: c4-highcpu-8 (8 vCPU, 16 GB)')
  lines.append('- **Network**: GCP VPC internal '
               '(10.10.0.0/24, same zone)')
  lines.append('- **Payload**: 1400B (WireGuard MTU)')
  lines.append('- **Topology**: 20 peers, 10 active '
               'sender/receiver pairs')
  lines.append('- **Duration**: 10 seconds per rate point')
  lines.append('- **HD Workers**: 8 (DEFER_TASKRUN)')
  lines.append('- **TS**: Go derper (dev mode, port 3340)')
  lines.append('- **Tuning**: wmem/rmem 64MB, THP madvise, '
               'NAPI defer, busy_poll')
  lines.append('')

  # Throughput table.
  lines.append('## Throughput Scaling')
  lines.append('')
  lines.append(
      '| Rate | TS Throughput | HD Throughput | '
      'TS Loss | HD Loss | HD/TS |')
  lines.append(
      '|------|-------------|-------------|'
      '--------|--------|-------|')

  for rate in RATES:
    ts = scale['Tailscale'].get(rate, {})
    hd = scale['Hyper-DERP'].get(rate, {})
    ts_tp = ts.get('throughput_mbps', 0)
    hd_tp = hd.get('throughput_mbps', 0)
    ts_loss = ts.get('message_loss_pct', 0)
    hd_loss = hd.get('message_loss_pct', 0)
    ratio = hd_tp / ts_tp if ts_tp > 0 else 0
    lines.append(
        f'| {rate:,} | {ts_tp:,.1f} Mbps | '
        f'{hd_tp:,.1f} Mbps | '
        f'{ts_loss:.1f}% | {hd_loss:.1f}% | '
        f'**{ratio:.2f}x** |')

  lines.append('')
  lines.append('![Throughput](throughput.png)')
  lines.append('')
  lines.append('![Loss](loss.png)')
  lines.append('')
  lines.append('![Ratio](ratio.png)')
  lines.append('')

  # Analysis.
  lines.append('## Analysis')
  lines.append('')

  # Find peaks.
  ts_peak = max(
      (scale['Tailscale'].get(r, {}).get(
          'throughput_mbps', 0) for r in RATES), default=0)
  hd_peak = max(
      (scale['Hyper-DERP'].get(r, {}).get(
          'throughput_mbps', 0) for r in RATES), default=0)

  lines.append(f'- **HD peak throughput**: {hd_peak:,.0f} Mbps '
               f'({hd_peak/1000:.1f} Gbps)')
  lines.append(f'- **TS peak throughput**: {ts_peak:,.0f} Mbps '
               f'({ts_peak/1000:.1f} Gbps)')
  if ts_peak > 0:
    lines.append(f'- **Peak ratio**: '
                 f'**{hd_peak/ts_peak:.2f}x** (HD/TS)')
  lines.append('')

  # Where HD pulls ahead.
  for rate in RATES:
    ts = scale['Tailscale'].get(rate, {})
    hd = scale['Hyper-DERP'].get(rate, {})
    ts_tp = ts.get('throughput_mbps', 0)
    hd_tp = hd.get('throughput_mbps', 0)
    if ts_tp > 0 and hd_tp > ts_tp * 1.05:
      lines.append(
          f'HD first pulls measurably ahead at '
          f'{rate:,} Mbps offered '
          f'({hd_tp:,.0f} vs {ts_tp:,.0f} Mbps).')
      break

  lines.append('')

  # Loss analysis.
  ts_first_loss = None
  hd_first_loss = None
  for rate in RATES:
    ts = scale['Tailscale'].get(rate, {})
    hd = scale['Hyper-DERP'].get(rate, {})
    if not ts_first_loss and ts.get('message_loss_pct', 0) > 0.5:
      ts_first_loss = rate
    if not hd_first_loss and hd.get('message_loss_pct', 0) > 0.5:
      hd_first_loss = rate

  if ts_first_loss:
    lines.append(
        f'TS first drops >0.5% at {ts_first_loss:,} Mbps. ')
  if hd_first_loss:
    lines.append(
        f'HD first drops >0.5% at {hd_first_loss:,} Mbps.')
  else:
    lines.append(
        'HD stays below 0.5% loss through 15 Gbps offered.')
  lines.append('')

  # Crossover.
  ts_20k = scale['Tailscale'].get(20000, {})
  hd_20k = scale['Hyper-DERP'].get(20000, {})
  if ts_20k and hd_20k:
    lines.append(
        f'At maximum offered rate (20 Gbps): '
        f'HD delivers {hd_20k["throughput_mbps"]:,.0f} Mbps '
        f'with {hd_20k["message_loss_pct"]:.1f}% loss vs '
        f'TS {ts_20k["throughput_mbps"]:,.0f} Mbps '
        f'with {ts_20k["message_loss_pct"]:.1f}% loss.')
  lines.append('')

  return '\n'.join(lines)


def main():
  scale = load_scale_data()

  print('Generating throughput plot...')
  plot_throughput(scale)

  print('Generating loss plot...')
  plot_loss(scale)

  print('Generating ratio plot...')
  plot_ratio(scale)

  print('Generating markdown...')
  md = generate_markdown(scale)
  md_path = os.path.join(OUT_DIR, 'GCP_BENCHMARK.md')
  with open(md_path, 'w') as f:
    f.write(md)

  print(f'Report: {md_path}')
  print('Done.')


if __name__ == '__main__':
  main()
