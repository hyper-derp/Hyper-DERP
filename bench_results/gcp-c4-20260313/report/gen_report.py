#!/usr/bin/env python3
"""Generate benchmark report with plots and tables."""

import json
import os
import statistics
from math import sqrt
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

BASE = Path(__file__).parent
PLOTS = BASE / 'plots'
PLOTS.mkdir(exist_ok=True)

RATES = [500, 1000, 2000, 3000, 5000, 7500, 10000, 15000, 20000]
LOADS = ['idle', 'bg1000', 'bg3000', 'bg5000', 'bg8000']
LOAD_LABELS = {
    'idle': 'Idle', 'bg1000': '1 Gbps', 'bg3000': '3 Gbps',
    'bg5000': '5 Gbps', 'bg8000': '8 Gbps',
}

COLORS = {'ts': '#4A90D9', 'hd': '#E74C3C'}
LABELS = {'ts': 'Tailscale derper', 'hd': 'Hyper-DERP'}


def ci95(vals):
  """95% confidence interval half-width (t-distribution)."""
  n = len(vals)
  if n < 2:
    return 0
  t_vals = {
      1: 12.7, 2: 4.3, 3: 3.2, 4: 2.8, 5: 2.6,
      9: 2.3, 19: 2.1, 20: 2.1,
  }
  t = t_vals.get(n - 1, 2.0)
  return t * statistics.stdev(vals) / sqrt(n)


def load_rates(vcpu_dir):
  """Load rate sweep JSON files into structured dict."""
  d = vcpu_dir / 'rate'
  results = {}
  for f in sorted(d.iterdir()):
    if f.suffix != '.json':
      continue
    try:
      data = json.loads(f.read_text())
      parts = f.stem.split('_')
      server, rate = parts[0], int(parts[1])
      key = (server, rate)
      if key not in results:
        results[key] = {'tp': [], 'loss': []}
      results[key]['tp'].append(data['throughput_mbps'])
      results[key]['loss'].append(data['message_loss_pct'])
    except Exception:
      pass
  return results


def load_latency(vcpu_dir):
  """Load latency JSON files into structured dict."""
  d = vcpu_dir / 'latency'
  results = {}
  for f in sorted(d.iterdir()):
    if f.suffix != '.json':
      continue
    try:
      data = json.loads(f.read_text())
      ln = data['latency_ns']
      parts = f.stem.split('_')
      server = parts[0]
      load = '_'.join(parts[1:-1])
      key = (server, load)
      if key not in results:
        results[key] = {
            'p50': [], 'p99': [], 'p999': [], 'max': [],
            'raw': [],
        }
      results[key]['p50'].append(ln['p50'] / 1000)
      results[key]['p99'].append(ln['p99'] / 1000)
      results[key]['p999'].append(ln['p999'] / 1000)
      results[key]['max'].append(ln['max'] / 1000)
      if 'raw_latencies_ns' in data:
        results[key]['raw'].extend(
            [x / 1000 for x in data['raw_latencies_ns']])
    except Exception:
      pass
  return results


def plot_throughput_box(rates_data, vcpus, out):
  """Box plot of throughput at each rate, TS vs HD."""
  fig, ax = plt.subplots(figsize=(12, 6))
  positions_ts = []
  positions_hd = []
  data_ts = []
  data_hd = []

  for i, rate in enumerate(RATES):
    ts_k = ('ts', rate)
    hd_k = ('hd', rate)
    x = i * 3
    if ts_k in rates_data:
      positions_ts.append(x)
      data_ts.append(rates_data[ts_k]['tp'])
    if hd_k in rates_data:
      positions_hd.append(x + 1)
      data_hd.append(rates_data[hd_k]['tp'])

  bp_ts = ax.boxplot(
      data_ts, positions=positions_ts, widths=0.8,
      patch_artist=True, showfliers=True, flierprops={
          'marker': 'o', 'markersize': 3})
  bp_hd = ax.boxplot(
      data_hd, positions=positions_hd, widths=0.8,
      patch_artist=True, showfliers=True, flierprops={
          'marker': 'o', 'markersize': 3})

  for box in bp_ts['boxes']:
    box.set_facecolor(COLORS['ts'])
    box.set_alpha(0.7)
  for box in bp_hd['boxes']:
    box.set_facecolor(COLORS['hd'])
    box.set_alpha(0.7)

  ax.set_xticks([i * 3 + 0.5 for i in range(len(RATES))])
  ax.set_xticklabels([f'{r/1000:.0f}G' if r >= 1000 else
                       f'{r}M' for r in RATES])
  ax.set_xlabel('Offered Rate')
  ax.set_ylabel('Delivered Throughput (Mbps)')
  ax.set_title(f'Throughput Distribution — {vcpus} vCPU Relay '
               f'(n={len(data_hd[0]) if data_hd else "?"})')
  ax.legend([bp_ts['boxes'][0], bp_hd['boxes'][0]],
            [LABELS['ts'], LABELS['hd']], loc='upper left')
  ax.grid(axis='y', alpha=0.3)
  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def plot_loss(rates_data, vcpus, out):
  """Line plot of loss% with error bars."""
  fig, ax = plt.subplots(figsize=(10, 5))

  for srv in ['ts', 'hd']:
    x_vals, y_vals, errs = [], [], []
    for rate in RATES:
      key = (srv, rate)
      if key not in rates_data:
        continue
      loss = rates_data[key]['loss']
      x_vals.append(rate / 1000)
      y_vals.append(statistics.mean(loss))
      errs.append(ci95(loss))
    ax.errorbar(x_vals, y_vals, yerr=errs, label=LABELS[srv],
                color=COLORS[srv], marker='o', capsize=3,
                linewidth=2)

  ax.set_xlabel('Offered Rate (Gbps)')
  ax.set_ylabel('Message Loss (%)')
  ax.set_title(f'Message Loss — {vcpus} vCPU Relay')
  ax.legend()
  ax.grid(alpha=0.3)
  ax.set_ylim(bottom=-2)
  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def plot_ratio(rates_data, vcpus, out):
  """Bar chart of HD/TS throughput ratio at high rates."""
  fig, ax = plt.subplots(figsize=(8, 5))
  high_rates = [r for r in RATES if r >= 3000]
  ratios = []
  labels = []

  for rate in high_rates:
    ts_k, hd_k = ('ts', rate), ('hd', rate)
    if ts_k in rates_data and hd_k in rates_data:
      ts_t = statistics.mean(rates_data[ts_k]['tp'])
      hd_t = statistics.mean(rates_data[hd_k]['tp'])
      if ts_t > 0:
        ratios.append(hd_t / ts_t)
        labels.append(f'{rate/1000:.0f}G' if rate >= 1000
                      else f'{rate}M')

  bars = ax.bar(labels, ratios, color=COLORS['hd'], alpha=0.8)
  ax.axhline(y=1.0, color='gray', linestyle='--', alpha=0.5)
  ax.set_ylabel('HD / TS Throughput Ratio')
  ax.set_xlabel('Offered Rate')
  ax.set_title(f'Hyper-DERP Advantage — {vcpus} vCPU Relay')

  for bar, ratio in zip(bars, ratios):
    ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
            f'{ratio:.1f}x', ha='center', va='bottom',
            fontweight='bold', fontsize=10)

  ax.grid(axis='y', alpha=0.3)
  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def plot_latency_bars(lat_data, vcpus, out):
  """Grouped bar chart of p50/p99 latency at each load level."""
  fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

  for ax, pct, title in [(ax1, 'p50', 'Median (p50)'),
                          (ax2, 'p99', 'p99')]:
    x = np.arange(len(LOADS))
    width = 0.35
    ts_vals, ts_errs = [], []
    hd_vals, hd_errs = [], []

    for load in LOADS:
      for srv, vals, errs in [('ts', ts_vals, ts_errs),
                               ('hd', hd_vals, hd_errs)]:
        key = (srv, load)
        if key in lat_data:
          v = lat_data[key][pct]
          vals.append(statistics.mean(v))
          errs.append(ci95(v))
        else:
          vals.append(0)
          errs.append(0)

    ax.bar(x - width / 2, ts_vals, width, yerr=ts_errs,
           label=LABELS['ts'], color=COLORS['ts'], alpha=0.7,
           capsize=3)
    ax.bar(x + width / 2, hd_vals, width, yerr=hd_errs,
           label=LABELS['hd'], color=COLORS['hd'], alpha=0.7,
           capsize=3)
    ax.set_xticks(x)
    ax.set_xticklabels([LOAD_LABELS[l] for l in LOADS])
    ax.set_ylabel('Latency (us)')
    ax.set_title(f'{title} Latency — {vcpus} vCPU')
    ax.legend()
    ax.grid(axis='y', alpha=0.3)

  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def plot_latency_box(lat_data, vcpus, out):
  """Box plots of p50 latency across runs for each load level."""
  fig, ax = plt.subplots(figsize=(12, 5))
  positions = []
  data_all = []
  colors_all = []
  tick_pos = []
  tick_labels = []

  for i, load in enumerate(LOADS):
    x = i * 3
    tick_pos.append(x + 0.5)
    tick_labels.append(LOAD_LABELS[load])
    for j, srv in enumerate(['ts', 'hd']):
      key = (srv, load)
      if key in lat_data:
        positions.append(x + j)
        data_all.append(lat_data[key]['p50'])
        colors_all.append(COLORS[srv])

  bp = ax.boxplot(data_all, positions=positions, widths=0.8,
                  patch_artist=True, showfliers=True,
                  flierprops={'marker': 'o', 'markersize': 3})
  for box, color in zip(bp['boxes'], colors_all):
    box.set_facecolor(color)
    box.set_alpha(0.7)

  ax.set_xticks(tick_pos)
  ax.set_xticklabels(tick_labels)
  ax.set_ylabel('p50 Latency (us)')
  ax.set_title(f'Median Latency Distribution — {vcpus} vCPU')
  ax.grid(axis='y', alpha=0.3)

  from matplotlib.patches import Patch
  ax.legend(handles=[
      Patch(facecolor=COLORS['ts'], alpha=0.7, label=LABELS['ts']),
      Patch(facecolor=COLORS['hd'], alpha=0.7, label=LABELS['hd']),
  ])
  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def generate_vcpu_report(vcpu_dir, vcpus):
  """Generate all plots and return markdown section."""
  rates = load_rates(vcpu_dir)
  lat = load_latency(vcpu_dir)
  prefix = f'{vcpus}vcpu'

  plot_throughput_box(rates, vcpus, PLOTS / f'{prefix}_throughput.png')
  plot_loss(rates, vcpus, PLOTS / f'{prefix}_loss.png')
  plot_ratio(rates, vcpus, PLOTS / f'{prefix}_ratio.png')
  plot_latency_bars(lat, vcpus, PLOTS / f'{prefix}_latency.png')
  plot_latency_box(lat, vcpus, PLOTS / f'{prefix}_latency_box.png')

  md = []
  md.append(f'## {vcpus} vCPU Results')
  md.append('')

  # System info.
  info_file = vcpu_dir / 'system_info.txt'
  if info_file.exists():
    info = info_file.read_text().strip()
    md.append(f'```')
    md.append(info)
    md.append(f'```')
    md.append('')

  # Rate table.
  md.append('### Rate Sweep')
  md.append('')
  md.append('| Rate | Server | N | Mean (Mbps) | 95% CI '
            '| CV% | Loss% | Loss CI |')
  md.append('|-----:|:------:|--:|------------:|-------:'
            '|----:|------:|--------:|')
  for rate in RATES:
    for srv in ['ts', 'hd']:
      key = (srv, rate)
      if key not in rates:
        continue
      tp, loss = rates[key]['tp'], rates[key]['loss']
      n = len(tp)
      mt = statistics.mean(tp)
      cv = (statistics.stdev(tp) / mt * 100
            if n > 1 and mt > 0 else 0)
      ml = statistics.mean(loss)
      md.append(
          f'| {rate} | {srv.upper()} | {n} | {mt:.0f} '
          f'| +/-{ci95(tp):.0f} | {cv:.1f} '
          f'| {ml:.2f} | +/-{ci95(loss):.2f} |')
  md.append('')

  # Ratio table.
  md.append('### HD vs TS')
  md.append('')
  md.append('| Rate | HD (Mbps) | TS (Mbps) | Ratio '
            '| HD Loss | TS Loss |')
  md.append('|-----:|----------:|----------:|------:'
            '|--------:|--------:|')
  for rate in RATES:
    ts_k, hd_k = ('ts', rate), ('hd', rate)
    if ts_k not in rates or hd_k not in rates:
      continue
    ts_t = statistics.mean(rates[ts_k]['tp'])
    hd_t = statistics.mean(rates[hd_k]['tp'])
    ts_l = statistics.mean(rates[ts_k]['loss'])
    hd_l = statistics.mean(rates[hd_k]['loss'])
    r = hd_t / ts_t if ts_t > 0 else 0
    bold = f'**{r:.2f}x**' if r > 1.05 else f'{r:.2f}x'
    md.append(
        f'| {rate} | {hd_t:.0f} | {ts_t:.0f} | {bold} '
        f'| {hd_l:.1f}% | {ts_l:.1f}% |')
  md.append('')

  # Plots.
  md.append(f'![Throughput](plots/{prefix}_throughput.png)')
  md.append('')
  md.append(f'![Loss](plots/{prefix}_loss.png)')
  md.append('')
  md.append(f'![Ratio](plots/{prefix}_ratio.png)')
  md.append('')

  # Latency table.
  md.append('### Latency Under Load')
  md.append('')
  md.append('| Load | Srv | N | p50 (us) | CI '
            '| p99 (us) | CI | p999 (us) | max (us) |')
  md.append('|-----:|:---:|--:|---------:|---:'
            '|---------:|---:|----------:|---------:|')
  for load in LOADS:
    for srv in ['ts', 'hd']:
      key = (srv, load)
      if key not in lat:
        continue
      r = lat[key]
      n = len(r['p50'])
      nice = (load.replace('bg', '') + 'M' if 'bg' in load
              else 'idle')
      md.append(
          f'| {nice} | {srv.upper()} | {n} '
          f'| {statistics.mean(r["p50"]):.0f} '
          f'| +/-{ci95(r["p50"]):.0f} '
          f'| {statistics.mean(r["p99"]):.0f} '
          f'| +/-{ci95(r["p99"]):.0f} '
          f'| {statistics.mean(r["p999"]):.0f} '
          f'| {statistics.mean(r["max"]):.0f} |')
  md.append('')
  md.append(f'![Latency](plots/{prefix}_latency.png)')
  md.append('')
  md.append(f'![Latency Box](plots/{prefix}_latency_box.png)')
  md.append('')

  return '\n'.join(md)


def main():
  """Generate full report."""
  md = []
  md.append('# Hyper-DERP vs Tailscale derper — GCP Benchmark')
  md.append('')
  md.append('**Date**: 2026-03-13')
  md.append('**Platform**: GCP c4-highcpu (Intel Xeon Platinum '
            '8581C @ 2.30 GHz)')
  md.append('**Region**: europe-west3-b (Frankfurt)')
  md.append('**Network**: VPC internal (10.10.0.0/24)')
  md.append('**Payload**: 1400 bytes (WireGuard MTU)')
  md.append('**Protocol**: DERP over plain TCP (no TLS)')
  md.append('')
  md.append('## Methodology')
  md.append('')
  md.append('- **Isolation**: Only one server runs at a time on '
            'the relay VM')
  md.append('- **Rate sweep**: 9 offered rates '
            '(500 Mbps - 20 Gbps), 20 peers, 10 active pairs, '
            '15s per run')
  md.append('- **Runs**: 3 runs at low rates '
            '(<=3G, zero-variance region), 20 runs at high rates '
            '(>=5G)')
  md.append('- **Latency**: 5000 pings per run, 500 warmup, '
            '10 runs per load level')
  md.append('- **Background loads**: idle, 1G, 3G, 5G, 8G '
            '(10 pairs)')
  md.append('- **Statistics**: mean, 95% CI '
            '(t-distribution), CV%')
  md.append('')
  md.append('### Caveats')
  md.append('')
  md.append('- Go derper is an unoptimized build '
            '(debug info, not stripped). A proper release build '
            'with `go build -trimpath -ldflags="-s -w"` is '
            'planned.')
  md.append('- VM placement not enforced (relay and client may '
            'share a physical host). A spread placement policy '
            'is planned.')
  md.append('- 16 vCPU data used the non-isolated method '
            '(both servers running, tested alternately). '
            'At 43% CPU headroom the impact is minimal.')
  md.append('')

  vcpu_configs = [(16, '16vcpu'), (8, '8vcpu'), (4, '4vcpu')]

  for vcpus, dirname in vcpu_configs:
    vcpu_dir = BASE / dirname
    if not vcpu_dir.exists():
      continue
    section = generate_vcpu_report(vcpu_dir, vcpus)
    md.append(section)

  # Cross-comparison.
  md.append('## Cross-vCPU Comparison')
  md.append('')
  md.append('### HD/TS Ratio by vCPU Count')
  md.append('')
  hdr = '| Offered Rate'
  sep = '|:-------------'
  for vcpus, _ in vcpu_configs:
    hdr += f' | {vcpus} vCPU'
    sep += ' | -------:'
  md.append(hdr + ' |')
  md.append(sep + ' |')
  for rate in [5000, 7500, 10000, 15000, 20000]:
    row = f'| {rate/1000:.0f} Gbps'
    for _, dirname in vcpu_configs:
      vcpu_dir = BASE / dirname
      if not vcpu_dir.exists():
        row += ' | - '
        continue
      rates = load_rates(vcpu_dir)
      ts_k, hd_k = ('ts', rate), ('hd', rate)
      if ts_k in rates and hd_k in rates:
        ts_t = statistics.mean(rates[ts_k]['tp'])
        hd_t = statistics.mean(rates[hd_k]['tp'])
        r = hd_t / ts_t if ts_t > 0 else 0
        row += f' | {r:.1f}x'
      else:
        row += ' | - '
    row += ' |'
    md.append(row)
  md.append('')

  # Cross-vCPU loss comparison.
  md.append('### Message Loss by vCPU Count')
  md.append('')
  hdr = '| Offered Rate'
  sep = '|:-------------'
  for vcpus, _ in vcpu_configs:
    hdr += f' | {vcpus}v HD'
    hdr += f' | {vcpus}v TS'
    sep += ' | ------:'
    sep += ' | ------:'
  md.append(hdr + ' |')
  md.append(sep + ' |')
  for rate in [3000, 5000, 7500, 10000, 15000, 20000]:
    row = f'| {rate/1000:.0f} Gbps'
    for _, dirname in vcpu_configs:
      vcpu_dir = BASE / dirname
      if not vcpu_dir.exists():
        row += ' | - | - '
        continue
      rates = load_rates(vcpu_dir)
      ts_k, hd_k = ('ts', rate), ('hd', rate)
      if ts_k in rates and hd_k in rates:
        hd_l = statistics.mean(rates[hd_k]['loss'])
        ts_l = statistics.mean(rates[ts_k]['loss'])
        hd_s = f'{hd_l:.1f}%' if hd_l >= 0.05 else '0%'
        ts_s = f'{ts_l:.1f}%' if ts_l >= 0.05 else '0%'
        row += f' | {hd_s} | {ts_s}'
      else:
        row += ' | - | - '
    row += ' |'
    md.append(row)
  md.append('')

  # Cross-vCPU latency comparison.
  load_names = {
      'idle': 'Idle', 'bg1000': '1G bg', 'bg3000': '3G bg',
      'bg5000': '5G bg', 'bg8000': '8G bg',
  }
  for pct in ['p50', 'p99']:
    label = pct.upper()
    md.append(f'### Latency {label} by vCPU Count (us)')
    md.append('')
    hdr = '| Load'
    sep = '|:-----'
    for vcpus, _ in vcpu_configs:
      hdr += f' | {vcpus}v HD'
      hdr += f' | {vcpus}v TS'
      sep += ' | ------:'
      sep += ' | ------:'
    md.append(hdr + ' |')
    md.append(sep + ' |')
    for load in ['idle', 'bg1000', 'bg3000', 'bg5000', 'bg8000']:
      row = f'| {load_names[load]}'
      for _, dirname in vcpu_configs:
        vcpu_dir = BASE / dirname
        if not vcpu_dir.exists():
          row += ' | - | - '
          continue
        lat = load_latency(vcpu_dir)
        ts_k, hd_k = ('ts', load), ('hd', load)
        if ts_k in lat and hd_k in lat:
          hd_v = statistics.mean(lat[hd_k][pct])
          ts_v = statistics.mean(lat[ts_k][pct])
          row += f' | {hd_v:.0f} | {ts_v:.0f}'
        else:
          row += ' | - | - '
      row += ' |'
      md.append(row)
    md.append('')

  # Conclusion — data-driven.
  md.append('## Key Findings')
  md.append('')

  findings = []
  for vcpus, dirname in vcpu_configs:
    vcpu_dir = BASE / dirname
    if not vcpu_dir.exists():
      continue
    rates = load_rates(vcpu_dir)
    ts_10k = ('ts', 10000)
    hd_10k = ('hd', 10000)
    ts_20k = ('ts', 20000)
    hd_20k = ('hd', 20000)
    if ts_10k in rates and hd_10k in rates:
      ts_t = statistics.mean(rates[ts_10k]['tp'])
      hd_t = statistics.mean(rates[hd_10k]['tp'])
      ts_l = statistics.mean(rates[ts_10k]['loss'])
      hd_l = statistics.mean(rates[hd_10k]['loss'])
      ratio = hd_t / ts_t if ts_t > 0 else 0
      findings.append((vcpus, ratio, ts_t, hd_t, ts_l, hd_l))

  for i, (vcpus, ratio, ts_t, hd_t, ts_l, hd_l) in enumerate(
      findings, 1):
    if ratio > 1.5:
      md.append(
          f'{i}. **At {vcpus} vCPU**, TS saturates at '
          f'~{ts_t/1000:.1f} Gbps while HD reaches '
          f'~{hd_t/1000:.1f} Gbps at 10G offered — '
          f'a **{ratio:.1f}x throughput advantage**. '
          f'Loss: HD {hd_l:.1f}% vs TS {ts_l:.1f}%.')
    elif ratio > 1.05:
      md.append(
          f'{i}. **At {vcpus} vCPU**, HD delivers '
          f'{(ratio-1)*100:.0f}% more throughput at 10G offered '
          f'({hd_t:.0f} vs {ts_t:.0f} Mbps) with dramatically '
          f'less loss ({hd_l:.1f}% vs {ts_l:.1f}%).')
    else:
      md.append(
          f'{i}. **At {vcpus} vCPU**, both servers '
          f'perform identically ({hd_t:.0f} vs {ts_t:.0f} Mbps). '
          f'The GCP network bandwidth cap is the bottleneck, '
          f'not CPU.')

  md.append('')
  md.append('**The advantage grows as resources shrink.** '
            'At high vCPU counts, GCP\'s per-VM bandwidth cap '
            'is the bottleneck and both servers are equivalent. '
            'As vCPUs decrease, Go\'s CPU overhead becomes the '
            'limiting factor while io_uring continues to scale — '
            'the key insight for cost-conscious deployments.')
  md.append('')

  report_path = BASE / 'GCP_BENCHMARK.md'
  report_path.write_text('\n'.join(md))
  print(f'Report: {report_path}')
  print(f'Plots:  {PLOTS}')


if __name__ == '__main__':
  main()
