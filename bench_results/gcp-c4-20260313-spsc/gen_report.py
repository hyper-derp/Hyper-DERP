#!/usr/bin/env python3
"""Generate SPSC benchmark report with plots and tables."""

import json
import statistics
from math import sqrt
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
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
LABELS = {'ts': 'Tailscale derper (v1.96.1)', 'hd': 'Hyper-DERP (SPSC)'}


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
        }
      results[key]['p50'].append(ln['p50'] / 1000)
      results[key]['p99'].append(ln['p99'] / 1000)
      results[key]['p999'].append(ln['p999'] / 1000)
      results[key]['max'].append(ln['max'] / 1000)
    except Exception:
      pass
  return results


def plot_throughput(rates_data, vcpus, out):
  """Line plot of throughput at each rate, TS vs HD."""
  fig, ax = plt.subplots(figsize=(10, 5))
  for srv in ['ts', 'hd']:
    x_vals, y_vals, errs = [], [], []
    for rate in RATES:
      key = (srv, rate)
      if key not in rates_data:
        continue
      tp = rates_data[key]['tp']
      x_vals.append(rate / 1000)
      y_vals.append(statistics.mean(tp))
      errs.append(ci95(tp))
    ax.errorbar(x_vals, y_vals, yerr=errs, label=LABELS[srv],
                color=COLORS[srv], marker='o', capsize=3,
                linewidth=2, markersize=6)
  # Reference line.
  max_rate = max(RATES) / 1000
  ax.plot([0, max_rate], [0, max_rate * 1000],
          color='gray', linestyle=':', alpha=0.4,
          label='Wire rate')
  ax.set_xlabel('Offered Rate (Gbps)')
  ax.set_ylabel('Delivered Throughput (Mbps)')
  ax.set_title(
      f'Throughput — {vcpus} vCPU c4-highcpu '
      f'(n={len(rates_data.get(("hd", 5000), {}).get("tp", []))})')
  ax.legend()
  ax.grid(alpha=0.3)
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
  ax.set_title(f'Message Loss — {vcpus} vCPU c4-highcpu')
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
  ax.set_title(
      f'Hyper-DERP Advantage — {vcpus} vCPU c4-highcpu')
  for bar, ratio in zip(bars, ratios):
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height(), f'{ratio:.1f}x',
            ha='center', va='bottom',
            fontweight='bold', fontsize=10)
  ax.grid(axis='y', alpha=0.3)
  fig.tight_layout()
  fig.savefig(out, dpi=150)
  plt.close(fig)


def plot_latency(lat_data, vcpus, out):
  """Grouped bar chart of p50/p99 latency."""
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


def main():
  """Generate full SPSC benchmark report."""
  vcpu_dir = BASE / '8vcpu'
  if not vcpu_dir.exists():
    print('No 8vcpu data found')
    return

  rates = load_rates(vcpu_dir)
  lat = load_latency(vcpu_dir)

  # Plots.
  plot_throughput(rates, 8, PLOTS / 'throughput.png')
  plot_loss(rates, 8, PLOTS / 'loss.png')
  plot_ratio(rates, 8, PLOTS / 'ratio.png')
  plot_latency(lat, 8, PLOTS / 'latency.png')

  md = []
  md.append('# Hyper-DERP SPSC Benchmark — GCP c4-highcpu-8')
  md.append('')
  md.append('**Date**: 2026-03-13')
  md.append('**Platform**: GCP c4-highcpu-8 '
            '(Intel Xeon Platinum 8581C)')
  md.append('**Region**: europe-west3-b (Frankfurt)')
  md.append('**Network**: VPC internal (10.10.0.0/24)')
  md.append('**Payload**: 1400 bytes (WireGuard MTU)')
  md.append('**Protocol**: DERP over plain TCP (no TLS)')
  md.append('**Go derper**: v1.96.1 release build '
            '(stripped, -trimpath)')
  md.append('**Hyper-DERP**: SPSC xfer rings + '
            'per-source frame return inboxes')
  md.append('**Workers**: 4 (8 vCPU / 2)')
  md.append('')

  md.append('## Changes from Previous Benchmark')
  md.append('')
  md.append('1. **SPSC xfer rings** — Replaced MPSC ring '
            '(spinlock contention) with per-source SPSC rings '
            '(N*N total, lock-free)')
  md.append('2. **Batched eventfd signaling** — One '
            'write(eventfd) per destination worker per CQE batch '
            'instead of per frame')
  md.append('3. **SPSC frame return inboxes** — Replaced '
            'Treiber stack (CAS contention) with per-source '
            'return slots (CAS retries at most once)')
  md.append('4. **Go derper release build** — v1.96.1, '
            '-trimpath -ldflags="-s -w", stripped (was '
            'unoptimized debug build)')
  md.append('')

  md.append('## Methodology')
  md.append('')
  md.append('- **Isolation**: Only one server runs at a time')
  md.append('- **Rate sweep**: 9 rates (500M-20G), 20 peers, '
            '10 active pairs, 15s/run')
  md.append('- **Runs**: 3 at low rates (<=3G), '
            '10 at high rates (>=5G)')
  md.append('- **Latency**: 5000 pings, 500 warmup, '
            '10 runs/load')
  md.append('- **Background loads**: idle, 1G, 3G, 5G, 8G')
  md.append('')

  # Rate table.
  md.append('## Rate Sweep')
  md.append('')
  md.append('| Rate | Server | N | Throughput (Mbps) '
            '| 95% CI | Loss% | Loss CI |')
  md.append('|-----:|:------:|--:|------------------:'
            '|-------:|------:|--------:|')
  for rate in RATES:
    for srv in ['ts', 'hd']:
      key = (srv, rate)
      if key not in rates:
        continue
      tp, loss = rates[key]['tp'], rates[key]['loss']
      n = len(tp)
      mt = statistics.mean(tp)
      ml = statistics.mean(loss)
      md.append(
          f'| {rate} | {srv.upper()} | {n} | {mt:.0f} '
          f'| +/-{ci95(tp):.0f} '
          f'| {ml:.2f} | +/-{ci95(loss):.2f} |')
  md.append('')

  # Ratio table.
  md.append('## HD vs TS Comparison')
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

  md.append('![Throughput](plots/throughput.png)')
  md.append('')
  md.append('![Loss](plots/loss.png)')
  md.append('')
  md.append('![Ratio](plots/ratio.png)')
  md.append('')

  # Latency table.
  md.append('## Latency Under Load')
  md.append('')
  md.append('| Load | Server | N | p50 (us) | CI '
            '| p99 (us) | CI | p999 (us) | max (us) |')
  md.append('|-----:|:------:|--:|---------:|---:'
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

  # Latency ratio.
  md.append('### Latency Ratio (TS / HD)')
  md.append('')
  md.append('| Load | p50 TS | p50 HD | p50 Ratio '
            '| p99 TS | p99 HD | p99 Ratio |')
  md.append('|-----:|-------:|-------:|----------:'
            '|-------:|-------:|----------:|')
  for load in LOADS:
    ts_k, hd_k = ('ts', load), ('hd', load)
    if ts_k not in lat or hd_k not in lat:
      continue
    nice = (load.replace('bg', '') + 'M' if 'bg' in load
            else 'idle')
    ts_p50 = statistics.mean(lat[ts_k]['p50'])
    hd_p50 = statistics.mean(lat[hd_k]['p50'])
    ts_p99 = statistics.mean(lat[ts_k]['p99'])
    hd_p99 = statistics.mean(lat[hd_k]['p99'])
    r50 = ts_p50 / hd_p50 if hd_p50 > 0 else 0
    r99 = ts_p99 / hd_p99 if hd_p99 > 0 else 0
    md.append(
        f'| {nice} | {ts_p50:.0f} | {hd_p50:.0f} '
        f'| {r50:.2f}x '
        f'| {ts_p99:.0f} | {hd_p99:.0f} '
        f'| {r99:.2f}x |')
  md.append('')

  md.append('![Latency](plots/latency.png)')
  md.append('')

  # Key findings.
  md.append('## Key Findings')
  md.append('')

  # Peak throughput.
  hd_peak_rate = 0
  hd_peak_tp = 0
  ts_peak_tp = 0
  for rate in RATES:
    hd_k = ('hd', rate)
    if hd_k in rates:
      tp = statistics.mean(rates[hd_k]['tp'])
      if tp > hd_peak_tp:
        hd_peak_tp = tp
        hd_peak_rate = rate
  for rate in RATES:
    ts_k = ('ts', rate)
    if ts_k in rates:
      tp = statistics.mean(rates[ts_k]['tp'])
      if tp > ts_peak_tp:
        ts_peak_tp = tp

  md.append(
      f'1. **Peak throughput**: HD {hd_peak_tp/1000:.1f} Gbps '
      f'vs TS {ts_peak_tp/1000:.1f} Gbps '
      f'({hd_peak_tp/ts_peak_tp:.1f}x)')
  md.append('')

  # Zero-loss ceiling.
  hd_zero_ceil = 0
  ts_zero_ceil = 0
  for rate in RATES:
    hd_k = ('hd', rate)
    if hd_k in rates:
      if statistics.mean(rates[hd_k]['loss']) < 0.1:
        hd_zero_ceil = rate
    ts_k = ('ts', rate)
    if ts_k in rates:
      if statistics.mean(rates[ts_k]['loss']) < 0.1:
        ts_zero_ceil = rate
  md.append(
      f'2. **Zero-loss ceiling**: HD lossless up to '
      f'{hd_zero_ceil/1000:.0f}G offered vs '
      f'TS at {ts_zero_ceil/1000:.0f}G — '
      f'{hd_zero_ceil/ts_zero_ceil:.0f}x headroom')
  md.append('')

  # Latency at peak load.
  ts_bg8k = ('ts', 'bg8000')
  hd_bg8k = ('hd', 'bg8000')
  if ts_bg8k in lat and hd_bg8k in lat:
    ts_p50 = statistics.mean(lat[ts_bg8k]['p50'])
    hd_p50 = statistics.mean(lat[hd_bg8k]['p50'])
    ts_p99 = statistics.mean(lat[ts_bg8k]['p99'])
    hd_p99 = statistics.mean(lat[hd_bg8k]['p99'])
    md.append(
        f'3. **Latency at 8G load**: HD '
        f'p50={hd_p50:.0f}us / p99={hd_p99:.0f}us '
        f'vs TS p50={ts_p50:.0f}us / p99={ts_p99:.0f}us '
        f'({ts_p50/hd_p50:.1f}x / {ts_p99/hd_p99:.1f}x)')
  md.append('')

  report_path = BASE / 'SPSC_BENCHMARK.md'
  report_path.write_text('\n'.join(md))
  print(f'Report: {report_path}')
  print(f'Plots:  {PLOTS}')


if __name__ == '__main__':
  main()
