#!/usr/bin/env python3
"""Generate Phase B benchmark report: vCPU scaling on GCP c4-highcpu.

Reads per-vCPU rate and latency JSON results and produces:
  - Per-vCPU throughput vs offered rate charts (4 PNGs)
  - Peak throughput scaling chart
  - HD/TS throughput ratio bar chart
  - Per-vCPU latency profile charts (4 PNGs)
  - Cross-vCPU p99 latency at ceil100
  - PHASE_B_REPORT.md
"""

import json
import os
import re
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULT_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    'bench_results/gcp-c4-phase-b'
OUT_DIR = RESULT_DIR

VCPUS = [2, 4, 8, 16]
RATE_STEPS = [500, 1000, 2000, 3000, 5000, 7500, 10000]
LOAD_LEVELS = ['idle', 'ceil25', 'ceil50', 'ceil75', 'ceil100', 'ceil150']
LOAD_LABELS = {
    'idle': 'Idle',
    'ceil25': '25%',
    'ceil50': '50%',
    'ceil75': '75%',
    'ceil100': '100%',
    'ceil150': '150%',
}

COLORS = {
    'ts': '#4A90D9',
    'hd': '#E5553B',
}
LABELS = {
    'ts': 'Tailscale (Go)',
    'hd': 'Hyper-DERP (kTLS)',
}


def load_json(path):
  """Load a JSON file, returning None on error."""
  try:
    with open(path) as f:
      return json.load(f)
  except (OSError, json.JSONDecodeError):
    return None


def discover_rate_files(vcpu_dir, prefix):
  """Find all rate JSON files for a given prefix (ts or hd).

  Returns dict mapping offered_rate -> [json_data, ...] with all
  repetition runs collected.
  """
  rate_dir = os.path.join(vcpu_dir, 'rate')
  if not os.path.isdir(rate_dir):
    return {}
  by_rate = {}
  for fname in os.listdir(rate_dir):
    if not fname.startswith(prefix + '_') or not fname.endswith('.json'):
      continue
    # Pattern: {prefix}_{rate}_r{N}.json
    m = re.match(rf'{prefix}_(\d+)_r\d+\.json', fname)
    if not m:
      continue
    rate = int(m.group(1))
    d = load_json(os.path.join(rate_dir, fname))
    if d:
      by_rate.setdefault(rate, []).append(d)
  return by_rate


def discover_latency_files(vcpu_dir, prefix):
  """Find all latency JSON files for a given prefix.

  Returns dict mapping load_level -> [json_data, ...].
  """
  lat_dir = os.path.join(vcpu_dir, 'latency')
  if not os.path.isdir(lat_dir):
    return {}
  by_level = {}
  for fname in os.listdir(lat_dir):
    if not fname.startswith(prefix + '_') or not fname.endswith('.json'):
      continue
    # Pattern: {prefix}_{level}_r{N}.json
    m = re.match(rf'{prefix}_([\w]+)_r\d+\.json', fname)
    if not m:
      continue
    level = m.group(1)
    d = load_json(os.path.join(lat_dir, fname))
    if d:
      by_level.setdefault(level, []).append(d)
  return by_level


def load_all():
  """Load all benchmark data across vCPU configs.

  Returns dict: vcpu -> prefix -> {rate: {rate: [runs]},
                                    latency: {level: [runs]}}.
  """
  data = {}
  for vcpu in VCPUS:
    vcpu_dir = os.path.join(RESULT_DIR, f'{vcpu}vcpu')
    if not os.path.isdir(vcpu_dir):
      continue
    entry = {}
    for prefix in ['ts', 'hd']:
      entry[prefix] = {
          'rate': discover_rate_files(vcpu_dir, prefix),
          'latency': discover_latency_files(vcpu_dir, prefix),
      }
    data[vcpu] = entry
  return data


def load_system_info(vcpu):
  """Load system_info.txt for a given vCPU config."""
  path = os.path.join(RESULT_DIR, f'{vcpu}vcpu', 'system_info.txt')
  info = {}
  try:
    with open(path) as f:
      for line in f:
        if ':' in line:
          k, v = line.split(':', 1)
          info[k.strip()] = v.strip()
  except OSError:
    pass
  return info


def compute_stats(values):
  """Compute summary statistics for a list of values.

  Returns dict with median, mean, p5, p25, p75, p95 or None if
  values is empty.
  """
  if not values:
    return None
  arr = np.array(values)
  return {
      'median': float(np.median(arr)),
      'mean': float(np.mean(arr)),
      'p5': float(np.percentile(arr, 5)),
      'p25': float(np.percentile(arr, 25)),
      'p75': float(np.percentile(arr, 75)),
      'p95': float(np.percentile(arr, 95)),
      'min': float(np.min(arr)),
      'max': float(np.max(arr)),
      'n': len(arr),
  }


def rate_throughput_stats(runs):
  """Extract throughput values from rate runs."""
  return [r['throughput_mbps'] for r in runs
          if 'throughput_mbps' in r]


def rate_loss_stats(runs):
  """Extract loss values from rate runs."""
  return [r.get('message_loss_pct', r.get('loss_pct', 0.0))
          for r in runs]


def latency_stat(runs, percentile):
  """Extract latency percentile (in us) from latency runs."""
  vals = []
  for r in runs:
    ln = r.get('latency_ns', {})
    v = ln.get(percentile)
    if v is not None:
      vals.append(v / 1000.0)
  return vals


def plot_throughput_per_vcpu(data):
  """Chart 1: Throughput vs offered rate, one per vCPU config."""
  paths = []
  for vcpu in VCPUS:
    if vcpu not in data:
      continue
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.set_title(
        f'Throughput vs Offered Rate ({vcpu} vCPU, 1400B)',
        fontsize=14, fontweight='bold')
    for prefix in ['ts', 'hd']:
      rate_data = data[vcpu][prefix]['rate']
      if not rate_data:
        continue
      rates = sorted(rate_data.keys())
      medians = []
      p25s = []
      p75s = []
      for rate in rates:
        tp = rate_throughput_stats(rate_data[rate])
        s = compute_stats(tp)
        if s:
          medians.append(s['median'])
          p25s.append(s['p25'])
          p75s.append(s['p75'])
        else:
          medians.append(0)
          p25s.append(0)
          p75s.append(0)
      ax.plot(rates, medians, marker='o', linewidth=2,
              markersize=6, color=COLORS[prefix],
              label=LABELS[prefix], zorder=3)
      ax.fill_between(rates, p25s, p75s,
                       color=COLORS[prefix], alpha=0.15,
                       zorder=2)
    # y=x reference line.
    max_rate = max(RATE_STEPS)
    ax.plot([0, max_rate], [0, max_rate],
            '--', color='gray', alpha=0.3, linewidth=1,
            label='Line rate')
    ax.set_xlabel('Offered Rate (Mbps)')
    ax.set_ylabel('Achieved Throughput (Mbps)')
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(0, max_rate * 1.05)
    ax.set_ylim(0, None)
    ax.yaxis.set_major_formatter(
        ticker.FuncFormatter(
            lambda v, _: f'{v:,.0f}'))
    plt.tight_layout()
    path = os.path.join(OUT_DIR, f'throughput_{vcpu}vcpu.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    paths.append(path)
    print(f'  wrote {path}')
  return paths


def plot_peak_scaling(data):
  """Chart 2: Peak median throughput vs vCPU count."""
  fig, ax = plt.subplots(figsize=(10, 6))
  ax.set_title(
      'Peak Throughput vs vCPU Count (1400B)',
      fontsize=14, fontweight='bold')
  for prefix in ['ts', 'hd']:
    vcpus_found = []
    peaks = []
    for vcpu in VCPUS:
      if vcpu not in data:
        continue
      rate_data = data[vcpu][prefix]['rate']
      if not rate_data:
        continue
      # Find peak median throughput across all rates.
      best = 0
      for rate, runs in rate_data.items():
        tp = rate_throughput_stats(runs)
        s = compute_stats(tp)
        if s and s['median'] > best:
          best = s['median']
      if best > 0:
        vcpus_found.append(vcpu)
        peaks.append(best)
    if vcpus_found:
      ax.plot(vcpus_found, peaks, marker='o', linewidth=2.5,
              markersize=8, color=COLORS[prefix],
              label=LABELS[prefix], zorder=3)
  # Ideal linear scaling reference (from 2 vCPU baseline).
  if 2 in data:
    for prefix in ['ts', 'hd']:
      rate_data = data[2][prefix]['rate']
      if not rate_data:
        continue
      best_2 = 0
      for rate, runs in rate_data.items():
        tp = rate_throughput_stats(runs)
        s = compute_stats(tp)
        if s and s['median'] > best_2:
          best_2 = s['median']
      if best_2 > 0:
        ideal = [best_2 * v / 2 for v in VCPUS]
        ax.plot(VCPUS, ideal, '--', color=COLORS[prefix],
                alpha=0.3, linewidth=1,
                label=f'{LABELS[prefix]} ideal')
        break  # One reference line is enough.
  ax.set_xlabel('vCPU Count')
  ax.set_ylabel('Peak Throughput (Mbps)')
  ax.set_xticks(VCPUS)
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3)
  ax.set_ylim(0, None)
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))
  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'peak_scaling.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def plot_throughput_ratio(data):
  """Chart 3: HD/TS peak throughput ratio bar chart."""
  fig, ax = plt.subplots(figsize=(8, 5))
  ax.set_title(
      'HD / TS Peak Throughput Ratio',
      fontsize=14, fontweight='bold')
  vcpus_found = []
  ratios = []
  for vcpu in VCPUS:
    if vcpu not in data:
      continue
    peaks = {}
    for prefix in ['ts', 'hd']:
      rate_data = data[vcpu][prefix]['rate']
      best = 0
      for rate, runs in rate_data.items():
        tp = rate_throughput_stats(runs)
        s = compute_stats(tp)
        if s and s['median'] > best:
          best = s['median']
      peaks[prefix] = best
    if peaks['ts'] > 0 and peaks['hd'] > 0:
      vcpus_found.append(vcpu)
      ratios.append(peaks['hd'] / peaks['ts'])
  if not vcpus_found:
    plt.close(fig)
    return None
  x = np.arange(len(vcpus_found))
  bar_colors = ['#E5553B' if r >= 1.0 else '#4A90D9'
                for r in ratios]
  bars = ax.bar(x, ratios, color=bar_colors,
                edgecolor='white', width=0.5)
  for bar, r in zip(bars, ratios):
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 0.02,
            f'{r:.2f}x', ha='center', va='bottom',
            fontsize=11, fontweight='bold')
  ax.axhline(y=1.0, color='gray', linestyle='--',
             alpha=0.5, linewidth=1)
  ax.set_xlabel('vCPU Count')
  ax.set_ylabel('HD / TS Ratio')
  ax.set_xticks(x)
  ax.set_xticklabels([str(v) for v in vcpus_found])
  ax.set_ylim(0, max(ratios) * 1.3 if ratios else 2.0)
  ax.grid(axis='y', alpha=0.3)
  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'throughput_ratio.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def plot_latency_profiles(data):
  """Chart 4: Grouped bar latency profiles per vCPU config."""
  paths = []
  for vcpu in VCPUS:
    if vcpu not in data:
      continue
    fig, ax = plt.subplots(figsize=(12, 6))
    ax.set_title(
        f'Latency Profile ({vcpu} vCPU, 1400B)',
        fontsize=14, fontweight='bold')
    # Find which load levels exist.
    levels_present = []
    for level in LOAD_LEVELS:
      ts_has = level in data[vcpu]['ts']['latency']
      hd_has = level in data[vcpu]['hd']['latency']
      if ts_has or hd_has:
        levels_present.append(level)
    if not levels_present:
      plt.close(fig)
      continue
    n_levels = len(levels_present)
    x = np.arange(n_levels)
    width = 0.18
    # 4 bars per level: TS p50, TS p99, HD p50, HD p99.
    bar_specs = [
        ('ts', 'p50', COLORS['ts'], 0.7, 'TS p50'),
        ('ts', 'p99', COLORS['ts'], 1.0, 'TS p99'),
        ('hd', 'p50', COLORS['hd'], 0.7, 'HD p50'),
        ('hd', 'p99', COLORS['hd'], 1.0, 'HD p99'),
    ]
    offsets = [-1.5 * width, -0.5 * width,
               0.5 * width, 1.5 * width]
    for i, (prefix, pctl, color, alpha, label) in \
        enumerate(bar_specs):
      vals = []
      for level in levels_present:
        runs = data[vcpu][prefix]['latency'].get(level, [])
        lat = latency_stat(runs, pctl)
        s = compute_stats(lat)
        vals.append(s['median'] if s else 0)
      bars = ax.bar(x + offsets[i], vals, width,
                    color=color, alpha=alpha, label=label,
                    edgecolor='white', linewidth=0.5)
      for bar, v in zip(bars, vals):
        if v > 0:
          ax.text(bar.get_x() + bar.get_width() / 2,
                  bar.get_height(), f'{v:.0f}',
                  ha='center', va='bottom', fontsize=7,
                  rotation=45)
    ax.set_xlabel('Load Level (% of throughput ceiling)')
    ax.set_ylabel('Latency (us)')
    ax.set_xticks(x)
    ax.set_xticklabels(
        [LOAD_LABELS.get(l, l) for l in levels_present])
    ax.legend(fontsize=9, ncol=4)
    ax.grid(axis='y', alpha=0.3)
    ax.set_ylim(0, None)
    plt.tight_layout()
    path = os.path.join(OUT_DIR,
                        f'latency_{vcpu}vcpu.png')
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    paths.append(path)
    print(f'  wrote {path}')
  return paths


def plot_latency_cross_vcpu(data):
  """Chart 5: p99 at ceil100 across vCPU configs."""
  fig, ax = plt.subplots(figsize=(8, 5))
  ax.set_title(
      'p99 Latency at 100% Load vs vCPU Count (1400B)',
      fontsize=14, fontweight='bold')
  for prefix in ['ts', 'hd']:
    vcpus_found = []
    p99_vals = []
    for vcpu in VCPUS:
      if vcpu not in data:
        continue
      runs = data[vcpu][prefix]['latency'].get('ceil100', [])
      lat = latency_stat(runs, 'p99')
      s = compute_stats(lat)
      if s:
        vcpus_found.append(vcpu)
        p99_vals.append(s['median'])
    if vcpus_found:
      ax.plot(vcpus_found, p99_vals, marker='s',
              linewidth=2, markersize=7,
              color=COLORS[prefix], label=LABELS[prefix])
  ax.set_xlabel('vCPU Count')
  ax.set_ylabel('p99 Latency (us)')
  ax.set_xticks(VCPUS)
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3)
  ax.set_ylim(0, None)
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))
  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_cross_vcpu.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  print(f'  wrote {path}')
  return path


def parse_gc_trace(vcpu):
  """Parse ts_gctrace.log for GC pause statistics.

  Returns dict with count, total_pause_ms, max_pause_ms,
  mean_pause_ms, p99_pause_ms, or None.
  """
  path = os.path.join(RESULT_DIR, f'{vcpu}vcpu',
                      'gc_trace', 'ts_gctrace.log')
  try:
    with open(path) as f:
      lines = f.readlines()
  except OSError:
    return None
  # GC trace format:
  # gc N @T% X: A+B+C ms clock, ...
  # The total STW pause is A + C from the clock times.
  pauses_ms = []
  pattern = re.compile(
      r'^gc \d+ @[\d.]+s \d+%: '
      r'([\d.]+)\+([\d.]+)\+([\d.]+) ms clock')
  for line in lines:
    m = pattern.match(line)
    if m:
      stw1 = float(m.group(1))
      stw2 = float(m.group(3))
      pauses_ms.append(stw1 + stw2)
  if not pauses_ms:
    return None
  arr = np.array(pauses_ms)
  return {
      'count': len(arr),
      'total_ms': float(np.sum(arr)),
      'mean_ms': float(np.mean(arr)),
      'max_ms': float(np.max(arr)),
      'p50_ms': float(np.median(arr)),
      'p99_ms': float(np.percentile(arr, 99)),
  }


def load_worker_stats(vcpu):
  """Load worker stats (before and after rate test).

  Returns (before, after) dicts or (None, None).
  """
  base = os.path.join(RESULT_DIR, f'{vcpu}vcpu', 'workers')
  before = load_json(os.path.join(base, 'hd_before.json'))
  after = load_json(os.path.join(base, 'hd_rate_after.json'))
  return before, after


def parse_tls_stat(vcpu):
  """Parse kTLS /proc/net/tls_stat before/after.

  Returns dict of {key: delta} for non-zero deltas.
  """
  base = os.path.join(RESULT_DIR, f'{vcpu}vcpu', 'tls_stat')
  def parse_file(path):
    stats = {}
    try:
      with open(path) as f:
        for line in f:
          parts = line.split()
          if len(parts) >= 2:
            stats[parts[0]] = int(parts[1])
    except (OSError, ValueError):
      pass
    return stats
  before = parse_file(os.path.join(base, 'hd_before.txt'))
  after = parse_file(os.path.join(base, 'hd_after.txt'))
  if not before or not after:
    return None
  deltas = {}
  for key in after:
    delta = after.get(key, 0) - before.get(key, 0)
    if delta != 0:
      deltas[key] = delta
  # Also include final absolute values for Curr* fields.
  for key in after:
    if key.startswith('TlsCurr'):
      deltas[f'{key} (final)'] = after[key]
  return deltas


def build_peak_table(data):
  """Build peak throughput summary table rows.

  Returns list of dicts with vcpu, ts_stats, hd_stats, ratio.
  """
  rows = []
  for vcpu in VCPUS:
    if vcpu not in data:
      continue
    row = {'vcpu': vcpu}
    for prefix in ['ts', 'hd']:
      rate_data = data[vcpu][prefix]['rate']
      # Find rate with highest median throughput.
      best_tp = []
      best_median = 0
      for rate, runs in rate_data.items():
        tp = rate_throughput_stats(runs)
        s = compute_stats(tp)
        if s and s['median'] > best_median:
          best_median = s['median']
          best_tp = tp
      row[f'{prefix}_stats'] = compute_stats(best_tp)
    ts_m = (row['ts_stats']['median']
            if row.get('ts_stats') else 0)
    hd_m = (row['hd_stats']['median']
            if row.get('hd_stats') else 0)
    row['ratio'] = hd_m / ts_m if ts_m > 0 else 0
    rows.append(row)
  return rows


def build_latency_table(data):
  """Build latency summary table.

  Returns list of dicts per (vcpu, level) with median p50/p99
  for TS and HD.
  """
  rows = []
  for vcpu in VCPUS:
    if vcpu not in data:
      continue
    for level in LOAD_LEVELS:
      row = {'vcpu': vcpu, 'level': level}
      for prefix in ['ts', 'hd']:
        runs = data[vcpu][prefix]['latency'].get(level, [])
        for pctl in ['p50', 'p99']:
          vals = latency_stat(runs, pctl)
          s = compute_stats(vals)
          key = f'{prefix}_{pctl}'
          row[key] = s['median'] if s else None
      if any(row.get(f'{p}_{q}') is not None
             for p in ['ts', 'hd'] for q in ['p50', 'p99']):
        rows.append(row)
  return rows


def fmt_us(val):
  """Format a latency value in microseconds."""
  if val is None:
    return '-'
  if val >= 1000:
    return f'{val:,.0f}'
  return f'{val:.1f}'


def fmt_mbps(val):
  """Format a throughput value."""
  if val is None:
    return '-'
  return f'{val:,.1f}'


def generate_markdown(data):
  """Generate PHASE_B_REPORT.md content."""
  lines = []
  lines.append('# Phase B: vCPU Scaling on GCP c4-highcpu')
  lines.append('')
  lines.append('**Date**: 2026-03-14 / 2026-03-15')
  lines.append('')

  # Test methodology.
  lines.append('## Test Methodology')
  lines.append('')
  lines.append('Rate sweep (500 - 10000 Mbps offered) and '
               'latency profiling (idle through 150% of '
               'throughput ceiling) on GCP c4-highcpu VMs with '
               '2, 4, 8, and 16 vCPUs. Each relay config was '
               'tested with Tailscale Go derper and Hyper-DERP '
               '(kTLS). Multiple repetitions per data point '
               '(5 at low rates, 25 at high rates) for '
               'statistical confidence.')
  lines.append('')

  # Hardware.
  lines.append('## Hardware')
  lines.append('')
  info = load_system_info(4)
  lines.append(f'- **CPU**: '
               f'{info.get("cpu", "Intel Xeon Platinum 8581C")}')
  lines.append(f'- **Kernel**: '
               f'{info.get("kernel", "N/A")}')
  lines.append('- **Instance type**: GCP c4-highcpu '
               '(2, 4, 8, 16 vCPU)')
  lines.append(f'- **Payload**: '
               f'{info.get("size", "1400B")} (WireGuard MTU)')
  lines.append(f'- **Peers**: '
               f'{info.get("peers", "20")} '
               f'({info.get("pairs", "10")} active pairs)')
  lines.append(f'- **Duration**: '
               f'{info.get("duration", "15s")} per data point')
  lines.append(f'- **Repetitions**: '
               f'{info.get("runs_low", "5")} (low rate) / '
               f'{info.get("runs_high", "25")} (high rate)')
  lines.append(f'- **Ping samples**: '
               f'{info.get("ping_count", "5000")} per run')
  lines.append('- **Software**: Hyper-DERP 0.1.0, '
               'Go derper v1.96.1 (go1.26.1)')
  lines.append('')

  # vCPU configs.
  lines.append('### Worker Mapping')
  lines.append('')
  lines.append('| vCPU | HD Workers | TS GOMAXPROCS |')
  lines.append('|-----:|-----------:|--------------:|')
  for vcpu in VCPUS:
    si = load_system_info(vcpu)
    workers = si.get('workers', str(vcpu // 2))
    lines.append(f'| {vcpu} | {workers} | {vcpu} |')
  lines.append('')

  # Peak throughput table.
  lines.append('## Peak Throughput')
  lines.append('')
  peak_rows = build_peak_table(data)
  lines.append(
      '| vCPU | TS Median | TS Mean | TS p5 | TS p95 '
      '| HD Median | HD Mean | HD p5 | HD p95 '
      '| HD/TS |')
  lines.append(
      '|-----:|----------:|--------:|------:|-------:'
      '|----------:|--------:|------:|-------:'
      '|------:|')
  for row in peak_rows:
    ts = row.get('ts_stats')
    hd = row.get('hd_stats')
    ts_med = fmt_mbps(ts['median']) if ts else '-'
    ts_mean = fmt_mbps(ts['mean']) if ts else '-'
    ts_p5 = fmt_mbps(ts['p5']) if ts else '-'
    ts_p95 = fmt_mbps(ts['p95']) if ts else '-'
    hd_med = fmt_mbps(hd['median']) if hd else '-'
    hd_mean = fmt_mbps(hd['mean']) if hd else '-'
    hd_p5 = fmt_mbps(hd['p5']) if hd else '-'
    hd_p95 = fmt_mbps(hd['p95']) if hd else '-'
    ratio = f'{row["ratio"]:.2f}x'
    lines.append(
        f'| {row["vcpu"]} '
        f'| {ts_med} | {ts_mean} | {ts_p5} | {ts_p95} '
        f'| {hd_med} | {hd_mean} | {hd_p5} | {hd_p95} '
        f'| {ratio} |')
  lines.append('')

  # Throughput charts.
  for vcpu in VCPUS:
    png = f'throughput_{vcpu}vcpu.png'
    if os.path.exists(os.path.join(OUT_DIR, png)):
      lines.append(
          f'### {vcpu} vCPU Throughput vs Offered Rate')
      lines.append('')
      lines.append(f'![{vcpu} vCPU Throughput]({png})')
      lines.append('')

  # Scaling chart.
  lines.append('### Peak Throughput Scaling')
  lines.append('')
  lines.append('![Peak Scaling](peak_scaling.png)')
  lines.append('')
  lines.append('### HD/TS Throughput Ratio')
  lines.append('')
  lines.append('![Throughput Ratio](throughput_ratio.png)')
  lines.append('')

  # Latency summary.
  lines.append('## Latency')
  lines.append('')
  lat_rows = build_latency_table(data)
  if lat_rows:
    lines.append(
        '| vCPU | Load | TS p50 (us) | TS p99 (us) '
        '| HD p50 (us) | HD p99 (us) |')
    lines.append(
        '|-----:|------|------------:|------------:'
        '|------------:|------------:|')
    for row in lat_rows:
      level_label = LOAD_LABELS.get(
          row['level'], row['level'])
      lines.append(
          f'| {row["vcpu"]} '
          f'| {level_label} '
          f'| {fmt_us(row.get("ts_p50"))} '
          f'| {fmt_us(row.get("ts_p99"))} '
          f'| {fmt_us(row.get("hd_p50"))} '
          f'| {fmt_us(row.get("hd_p99"))} |')
    lines.append('')

  # Latency charts.
  for vcpu in VCPUS:
    png = f'latency_{vcpu}vcpu.png'
    if os.path.exists(os.path.join(OUT_DIR, png)):
      lines.append(f'### {vcpu} vCPU Latency Profile')
      lines.append('')
      lines.append(f'![{vcpu} vCPU Latency]({png})')
      lines.append('')
  lines.append('### p99 at 100% Load vs vCPU Count')
  lines.append('')
  lines.append(
      '![p99 Cross-vCPU](latency_cross_vcpu.png)')
  lines.append('')

  # GC trace analysis.
  lines.append('## Go GC Trace Analysis')
  lines.append('')
  gc_found = False
  for vcpu in VCPUS:
    gc = parse_gc_trace(vcpu)
    if gc:
      gc_found = True
      lines.append(f'### {vcpu} vCPU')
      lines.append('')
      lines.append(f'- **GC events**: {gc["count"]}')
      lines.append(
          f'- **Total STW pause**: '
          f'{gc["total_ms"]:.1f} ms')
      lines.append(
          f'- **Mean STW pause**: '
          f'{gc["mean_ms"]:.3f} ms')
      lines.append(
          f'- **Max STW pause**: '
          f'{gc["max_ms"]:.3f} ms')
      lines.append(
          f'- **p50 STW pause**: '
          f'{gc["p50_ms"]:.3f} ms')
      lines.append(
          f'- **p99 STW pause**: '
          f'{gc["p99_ms"]:.3f} ms')
      lines.append('')
  if not gc_found:
    lines.append('No GC trace data found.')
    lines.append('')

  # Worker stats.
  lines.append('## Hyper-DERP Worker Stats')
  lines.append('')
  for vcpu in VCPUS:
    before, after = load_worker_stats(vcpu)
    if not after:
      continue
    lines.append(f'### {vcpu} vCPU')
    lines.append('')
    workers = after.get('workers', [])
    if not workers:
      continue
    total_recv = sum(w.get('recv_bytes', 0) for w in workers)
    total_send = sum(w.get('send_bytes', 0) for w in workers)
    total_drops = sum(w.get('send_drops', 0) for w in workers)
    total_xfer = sum(w.get('xfer_drops', 0) for w in workers)
    total_slab = sum(
        w.get('slab_exhausts', 0) for w in workers)
    total_epipe = sum(
        w.get('send_epipe', 0) for w in workers)
    total_econn = sum(
        w.get('send_econnreset', 0) for w in workers)
    total_eagain = sum(
        w.get('send_eagain', 0) for w in workers)
    lines.append(
        f'- **Workers**: {len(workers)}')
    lines.append(
        f'- **Total recv**: '
        f'{total_recv / 1e9:.2f} GB')
    lines.append(
        f'- **Total send**: '
        f'{total_send / 1e9:.2f} GB')
    lines.append(
        f'- **Send drops**: {total_drops}')
    lines.append(
        f'- **Xfer drops**: {total_xfer}')
    lines.append(
        f'- **Slab exhausts**: {total_slab}')
    lines.append(
        f'- **EPIPE errors**: {total_epipe}')
    lines.append(
        f'- **ECONNRESET errors**: {total_econn}')
    lines.append(
        f'- **EAGAIN errors**: {total_eagain}')
    # Per-worker peer distribution.
    peer_dist = [w.get('peers', 0) for w in workers]
    lines.append(
        f'- **Peer distribution**: '
        f'{peer_dist}')
    lines.append('')

  # kTLS stats.
  lines.append('## kTLS Stats')
  lines.append('')
  tls_found = False
  for vcpu in VCPUS:
    deltas = parse_tls_stat(vcpu)
    if deltas:
      tls_found = True
      lines.append(f'### {vcpu} vCPU')
      lines.append('')
      for key, val in sorted(deltas.items()):
        lines.append(f'- **{key}**: {val}')
      lines.append('')
  if not tls_found:
    lines.append('No kTLS stat deltas found.')
    lines.append('')

  # Key findings.
  lines.append('## Key Findings')
  lines.append('')
  if peak_rows:
    # Compute overall trends.
    ratios = [r['ratio'] for r in peak_rows if r['ratio'] > 0]
    if ratios:
      min_r = min(ratios)
      max_r = max(ratios)
      lines.append(
          f'1. **HD/TS throughput ratio**: '
          f'{min_r:.2f}x - {max_r:.2f}x across '
          f'vCPU configs.')
    # Scaling efficiency.
    if len(peak_rows) >= 2:
      first = peak_rows[0]
      last = peak_rows[-1]
      for prefix, label in [('ts', 'TS'), ('hd', 'HD')]:
        s0 = first.get(f'{prefix}_stats')
        s1 = last.get(f'{prefix}_stats')
        if s0 and s1:
          vcpu_ratio = last['vcpu'] / first['vcpu']
          tp_ratio = s1['median'] / s0['median']
          eff = tp_ratio / vcpu_ratio * 100
          lines.append(
              f'2. **{label} scaling efficiency** '
              f'({first["vcpu"]} -> {last["vcpu"]} vCPU): '
              f'{tp_ratio:.1f}x throughput gain '
              f'({eff:.0f}% of linear).')
          break
  lines.append('')

  return '\n'.join(lines)


def main():
  """Entry point: load data, generate plots and report."""
  data = load_all()
  total = sum(
      sum(len(runs) for runs in data[v][p]['rate'].values())
      + sum(len(runs)
            for runs in data[v][p]['latency'].values())
      for v in data for p in ['ts', 'hd'])
  print(f'Loaded {total} data points from {RESULT_DIR}')
  if total == 0:
    print('No data found.')
    sys.exit(1)
  print('Generating plots...')
  plot_throughput_per_vcpu(data)
  plot_peak_scaling(data)
  plot_throughput_ratio(data)
  plot_latency_profiles(data)
  plot_latency_cross_vcpu(data)
  print('Generating markdown...')
  md = generate_markdown(data)
  md_path = os.path.join(OUT_DIR, 'PHASE_B_REPORT.md')
  with open(md_path, 'w') as f:
    f.write(md)
  print(f'Report: {md_path}')


if __name__ == '__main__':
  main()
