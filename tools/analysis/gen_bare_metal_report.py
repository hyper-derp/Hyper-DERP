#!/usr/bin/env python3
"""Generate bare-metal comparison report: Hyper-DERP vs Tailscale derper.

Reads throughput-vs-rate JSON, idle latency, loaded latency, and perf
counter data from the bare_metal results directory.
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
    'bench_results/bare_metal-20260311'
OUT_DIR = RESULT_DIR

COLORS = {'Tailscale': '#4A90D9', 'Hyper-DERP': '#E5553B'}
RATES = [100, 500, 1000, 2000, 5000, 10000, 20000, 50000]


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
    for rate in RATES + [0]:
      label = f'{rate}mbps' if rate > 0 else '0mbps_unlimited'
      d = load_json(os.path.join(RESULT_DIR, f'{prefix}_{label}.json'))
      if d:
        data[server][rate] = d
  return data


def load_latency_data():
  """Load 1400B idle latency ping/echo results."""
  data = {}
  for server, prefix in [('Tailscale', 'ts'), ('Hyper-DERP', 'hd')]:
    d = load_json(
        os.path.join(RESULT_DIR, f'{prefix}_lat_1400B.json'))
    if d and 'latency_ns' in d:
      ln = d['latency_ns']
      data[server] = {
          'min': ln['min'] / 1000,
          'p50': ln['p50'] / 1000,
          'p90': ln['p90'] / 1000,
          'p99': ln['p99'] / 1000,
          'p999': ln['p999'] / 1000,
          'max': ln['max'] / 1000,
          'mean': ln['mean'] / 1000,
          'pps': d['throughput_pps'],
          'raw': [x / 1000 for x in ln.get('raw', [])],
      }
  return data


def load_loaded_latency():
  """Load loaded latency results at 500/2000 Mbps background."""
  data = {}
  for server, prefix in [('Tailscale', 'ts'), ('Hyper-DERP', 'hd')]:
    data[server] = {}
    for bg_rate in [500, 2000]:
      d = load_json(os.path.join(
          RESULT_DIR, f'{prefix}_loaded_lat_{bg_rate}mbps.json'))
      if d and 'latency_ns' in d:
        ln = d['latency_ns']
        data[server][bg_rate] = {
            'p50': ln['p50'] / 1000,
            'p90': ln['p90'] / 1000,
            'p99': ln['p99'] / 1000,
            'p999': ln['p999'] / 1000,
            'max': ln['max'] / 1000,
            'mean': ln['mean'] / 1000,
            'pps': d['throughput_pps'],
            'raw': [x / 1000 for x in ln.get('raw', [])],
        }
  return data


def load_perf_data():
  """Load perf stat text files."""
  data = {}
  for server, prefix in [('Tailscale', 'ts'), ('Hyper-DERP', 'hd')]:
    path = os.path.join(RESULT_DIR, f'{prefix}_perf_5000_perf.txt')
    try:
      with open(path) as f:
        data[server] = f.read()
    except OSError:
      pass
  return data


def load_system_info():
  """Load system_info.txt into a dict."""
  sysinfo = {}
  try:
    with open(os.path.join(RESULT_DIR, 'system_info.txt')) as f:
      for line in f:
        if ':' in line:
          k, v = line.split(':', 1)
          sysinfo[k.strip()] = v.strip()
  except OSError:
    pass
  return sysinfo


def plot_throughput_scaling(scale):
  """Line chart: delivered throughput (Mbps) vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  fig.suptitle(
      'Delivered Throughput vs Offered Rate\n'
      '(20 peers, 10 pairs, 1400B, 10s, bare metal loopback)',
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

  # Ideal line.
  ideal_rates = np.array(RATES, dtype=float)
  # At low rates, actual ~= rate * 0.63 (overhead from framing).
  scale_factor = 0.63
  ideal_tp = ideal_rates * scale_factor
  ax.plot(ideal_rates, ideal_tp, '--', color='gray', alpha=0.4,
          linewidth=1, label=f'Ideal ({scale_factor:.0%} efficiency)')

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
  path = os.path.join(OUT_DIR, 'throughput_scaling.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_loss_rate(scale):
  """Line chart: message loss % vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  fig.suptitle(
      'Message Loss vs Offered Rate\n'
      '(20 peers, 10 pairs, 1400B, 10s, bare metal loopback)',
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
  ax.set_ylim(-2, None)
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))
  ax.axhline(y=1, color='orange', linestyle='--', alpha=0.5,
             linewidth=1)
  ax.text(RATES[0], 1.5, '1% loss threshold', fontsize=8,
          color='orange')

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'loss_rate.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_bars(lat):
  """Bar chart: idle p50/p90/p99/p999 latency at 1400B."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle(
      'Idle Round-Trip Latency at 1400B\n'
      '(bare metal loopback, 5000 pings)',
      fontsize=13, fontweight='bold')

  metrics = ['p50', 'p90', 'p99', 'p999']
  x = np.arange(len(metrics))
  width = 0.35

  ts_vals = [lat['Tailscale'].get(m, 0) for m in metrics]
  hd_vals = [lat['Hyper-DERP'].get(m, 0) for m in metrics]

  bars_ts = ax.bar(x - width / 2, ts_vals, width,
                   label='Tailscale derper',
                   color=COLORS['Tailscale'], edgecolor='white')
  bars_hd = ax.bar(x + width / 2, hd_vals, width,
                   label='Hyper-DERP',
                   color=COLORS['Hyper-DERP'], edgecolor='white')

  for bar in list(bars_ts) + list(bars_hd):
    ax.text(bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 1,
            f'{bar.get_height():.0f}',
            ha='center', va='bottom', fontsize=9)

  ax.set_xlabel('Percentile')
  ax.set_ylabel('Latency (us)')
  ax.set_xticks(x)
  ax.set_xticklabels(metrics)
  ax.legend(loc='upper left', fontsize=10)
  ax.set_ylim(0, max(ts_vals + hd_vals) * 1.3)
  ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_idle_1400B.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_cdf(lat):
  """CDF of raw idle latency samples at 1400B."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle(
      'Idle Latency CDF at 1400B\n'
      '(bare metal loopback)',
      fontsize=13, fontweight='bold')

  for server in ['Tailscale', 'Hyper-DERP']:
    raw = lat[server].get('raw', [])
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
  ax.legend(loc='lower right', fontsize=10)
  ax.grid(alpha=0.3)
  ax.set_ylim(0, 101)
  ax.axhline(y=99, color='gray', linestyle='--', alpha=0.4,
             linewidth=0.8)
  ax.text(ax.get_xlim()[0] + 5, 99.3, 'p99', fontsize=8,
          color='gray')

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_cdf_1400B.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_loaded_latency(loaded):
  """Bar chart: loaded latency comparison at 500 and 2000 Mbps."""
  fig, axes = plt.subplots(1, 2, figsize=(14, 5))
  fig.suptitle(
      'Loaded Latency (1400B ping with background traffic)\n'
      '(bare metal loopback)',
      fontsize=13, fontweight='bold')

  for idx, bg_rate in enumerate([500, 2000]):
    ax = axes[idx]
    metrics = ['p50', 'p90', 'p99', 'p999']
    x = np.arange(len(metrics))
    width = 0.35

    ts_data = loaded.get('Tailscale', {}).get(bg_rate, {})
    hd_data = loaded.get('Hyper-DERP', {}).get(bg_rate, {})
    if not ts_data or not hd_data:
      continue

    ts_vals = [ts_data.get(m, 0) for m in metrics]
    hd_vals = [hd_data.get(m, 0) for m in metrics]

    bars_ts = ax.bar(x - width / 2, ts_vals, width,
                     label='Tailscale derper',
                     color=COLORS['Tailscale'], edgecolor='white')
    bars_hd = ax.bar(x + width / 2, hd_vals, width,
                     label='Hyper-DERP',
                     color=COLORS['Hyper-DERP'], edgecolor='white')

    for bar in list(bars_ts) + list(bars_hd):
      ax.text(bar.get_x() + bar.get_width() / 2,
              bar.get_height() + 2,
              f'{bar.get_height():.0f}',
              ha='center', va='bottom', fontsize=8)

    ax.set_xlabel('Percentile')
    ax.set_ylabel('Latency (us)')
    ax.set_title(f'{bg_rate} Mbps background load')
    ax.set_xticks(x)
    ax.set_xticklabels(metrics)
    ax.legend(loc='upper left', fontsize=9)
    ax.set_ylim(0, max(ts_vals + hd_vals) * 1.35)
    ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_loaded.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_loaded_latency_cdf(loaded):
  """CDF of loaded latency samples at 500 and 2000 Mbps."""
  fig, axes = plt.subplots(1, 2, figsize=(14, 5))
  fig.suptitle(
      'Loaded Latency CDF (1400B ping with background traffic)\n'
      '(bare metal loopback)',
      fontsize=13, fontweight='bold')

  for idx, bg_rate in enumerate([500, 2000]):
    ax = axes[idx]
    for server in ['Tailscale', 'Hyper-DERP']:
      raw = loaded.get(server, {}).get(bg_rate, {}).get('raw', [])
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
    ax.set_title(f'{bg_rate} Mbps background')
    ax.legend(loc='lower right', fontsize=9)
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 101)
    ax.axhline(y=99, color='gray', linestyle='--', alpha=0.4,
               linewidth=0.8)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_loaded_cdf.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def generate_markdown(scale, lat, loaded, perf, sysinfo):
  """Generate the bare-metal comparison markdown report."""
  lines = []
  lines.append('# Hyper-DERP vs Tailscale derper: '
               'Bare Metal Comparison')
  lines.append('')
  lines.append('## Test Environment')
  lines.append('')
  lines.append(f'- **Date**: {sysinfo.get("date", "N/A")}')
  lines.append(f'- **CPU**: {sysinfo.get("cpu", "N/A")}')
  lines.append(f'- **Kernel**: {sysinfo.get("kernel", "N/A")}')
  lines.append(f'- **Cores**: {sysinfo.get("cores", "N/A")}')
  lines.append(f'- **Governor**: {sysinfo.get("governor", "N/A")}')
  lines.append(f'- **Relay pinned**: cores '
               f'{sysinfo.get("relay_cores", "4,5")}')
  lines.append(f'- **Client pinned**: cores '
               f'{sysinfo.get("client_cores", "12-15")}')
  lines.append(f'- **Workers**: '
               f'{sysinfo.get("workers", "2")} (Hyper-DERP)')
  lines.append('- **Network**: localhost loopback (TCP)')
  lines.append(f'- **Payload**: '
               f'{sysinfo.get("size", "1400B")} (WireGuard MTU)')
  lines.append(f'- **Topology**: '
               f'{sysinfo.get("peers", "20")} peers, '
               f'{sysinfo.get("pairs", "10")} active pairs')
  lines.append(f'- **Duration**: '
               f'{sysinfo.get("duration", "10s")} per rate point')
  lines.append(f'- **TCP tuning**: wmem_max='
               f'{sysinfo.get("wmem_max", "16MB")}, '
               f'lo_mtu={sysinfo.get("lo_mtu", "65536")}')
  lines.append('')

  # Throughput scaling table.
  lines.append('## Throughput Scaling')
  lines.append('')
  lines.append('Delivered relay throughput as offered send rate '
               'increases. Rate is token-bucket paced across all '
               '10 sender threads.')
  lines.append('')
  lines.append('| Rate (Mbps) | TS Sent | TS Recv | TS Loss | '
               'TS Mbps | HD Sent | HD Recv | HD Loss | '
               'HD Mbps | HD/TS |')
  lines.append('|-------------|---------|---------|---------|'
               '---------|---------|---------|---------|'
               '---------|-------|')

  for rate in RATES:
    ts = scale['Tailscale'].get(rate, {})
    hd = scale['Hyper-DERP'].get(rate, {})
    if not ts and not hd:
      continue

    def fmt(d, key, is_pct=False):
      v = d.get(key, 0) if d else 0
      if is_pct:
        return f'{v:.2f}%'
      if isinstance(v, float):
        return f'{v:.1f}'
      return f'{v:,}'

    ts_tp = ts.get('throughput_mbps', 0) if ts else 0
    hd_tp = hd.get('throughput_mbps', 0) if hd else 0
    ratio = f'{hd_tp/ts_tp:.1f}x' if ts_tp > 0 else 'N/A'

    lines.append(
        f'| {rate:,} | '
        f'{fmt(ts, "messages_sent")} | '
        f'{fmt(ts, "messages_recv")} | '
        f'{fmt(ts, "message_loss_pct", True)} | '
        f'{fmt(ts, "throughput_mbps")} | '
        f'{fmt(hd, "messages_sent")} | '
        f'{fmt(hd, "messages_recv")} | '
        f'{fmt(hd, "message_loss_pct", True)} | '
        f'{fmt(hd, "throughput_mbps")} | '
        f'{ratio} |')

  lines.append('')
  lines.append('![Throughput Scaling](throughput_scaling.png)')
  lines.append('')
  lines.append('![Loss Rate](loss_rate.png)')
  lines.append('')

  # Saturation analysis.
  lines.append('## Saturation Analysis')
  lines.append('')

  # Find TS ceiling (it plateaus early).
  ts_peak = 0
  ts_peak_rate = 0
  hd_peak = 0
  hd_peak_rate = 0
  for rate in RATES:
    for server, vals in [('Tailscale', None), ('Hyper-DERP', None)]:
      d = scale[server].get(rate, {})
      tp = d.get('throughput_mbps', 0)
      if server == 'Tailscale' and tp > ts_peak:
        ts_peak = tp
        ts_peak_rate = rate
      if server == 'Hyper-DERP' and tp > hd_peak:
        hd_peak = tp
        hd_peak_rate = rate

  lines.append(
      f'- **TS ceiling**: {ts_peak:.0f} Mbps '
      f'(reached at {ts_peak_rate:,} Mbps offered) — '
      f'plateaus and cannot push further')
  lines.append(
      f'- **HD ceiling**: {hd_peak:.0f} Mbps '
      f'(reached at {hd_peak_rate:,} Mbps offered)')
  if ts_peak > 0:
    lines.append(
        f'- **HD/TS peak ratio**: '
        f'**{hd_peak/ts_peak:.1f}x**')
  lines.append('')

  # Find where each starts losing.
  for server, prefix in [('Tailscale', 'TS'), ('Hyper-DERP', 'HD')]:
    first_loss_rate = None
    for rate in RATES:
      d = scale[server].get(rate, {})
      if d and d.get('message_loss_pct', 0) > 0.01:
        first_loss_rate = rate
        break
    if first_loss_rate:
      d = scale[server][first_loss_rate]
      lines.append(
          f'- **{prefix}** first loss at '
          f'{first_loss_rate:,} Mbps '
          f'({d["message_loss_pct"]:.2f}%)')
  lines.append('')

  # Idle latency section.
  if lat:
    lines.append('## Idle Round-Trip Latency (1400B)')
    lines.append('')
    lines.append('Measured via ping/echo over loopback '
                 '(5000 round-trips).')
    lines.append('')
    lines.append('| Metric | Tailscale | Hyper-DERP | '
                 'Speedup |')
    lines.append('|--------|-----------|------------|'
                 '---------|')

    for m in ['p50', 'p90', 'p99', 'p999', 'max']:
      ts_v = lat.get('Tailscale', {}).get(m, 0)
      hd_v = lat.get('Hyper-DERP', {}).get(m, 0)
      if ts_v and hd_v:
        ratio = ts_v / hd_v
        bold = '**' if ratio > 1.0 else ''
        lines.append(
            f'| {m} | {ts_v:.0f} us | '
            f'{hd_v:.0f} us | {bold}{ratio:.2f}x{bold} |')

    ts_pps = lat.get('Tailscale', {}).get('pps', 0)
    hd_pps = lat.get('Hyper-DERP', {}).get('pps', 0)
    if ts_pps and hd_pps:
      lines.append('')
      lines.append(
          f'Ping throughput: TS {ts_pps:,.0f} pps, '
          f'HD {hd_pps:,.0f} pps '
          f'({hd_pps/ts_pps:.2f}x)')

    lines.append('')
    lines.append('![Idle Latency](latency_idle_1400B.png)')
    lines.append('')
    lines.append('![Idle Latency CDF](latency_cdf_1400B.png)')
    lines.append('')

  # Loaded latency section.
  if loaded:
    lines.append('## Loaded Latency (1400B)')
    lines.append('')
    lines.append('Ping/echo latency while background throughput '
                 'traffic is running.')
    lines.append('')

    for bg_rate in [500, 2000]:
      ts_data = loaded.get('Tailscale', {}).get(bg_rate, {})
      hd_data = loaded.get('Hyper-DERP', {}).get(bg_rate, {})
      if not ts_data or not hd_data:
        continue

      lines.append(f'### {bg_rate} Mbps background')
      lines.append('')
      lines.append('| Metric | Tailscale | Hyper-DERP | '
                   'Ratio |')
      lines.append('|--------|-----------|------------|'
                   '-------|')

      for m in ['p50', 'p90', 'p99', 'p999']:
        ts_v = ts_data.get(m, 0)
        hd_v = hd_data.get(m, 0)
        if ts_v and hd_v:
          ratio = hd_v / ts_v
          lines.append(
              f'| {m} | {ts_v:.0f} us | '
              f'{hd_v:.0f} us | {ratio:.1f}x |')
      lines.append('')

    lines.append('![Loaded Latency](latency_loaded.png)')
    lines.append('')
    lines.append('![Loaded Latency CDF](latency_loaded_cdf.png)')
    lines.append('')

  # Perf counters section.
  if perf:
    lines.append('## CPU Performance Counters (5000 Mbps, 10s)')
    lines.append('')
    lines.append('`perf stat` during 5000 Mbps throughput test.')
    lines.append('')

    for server in ['Tailscale', 'Hyper-DERP']:
      txt = perf.get(server, '')
      if not txt:
        continue
      label = 'Tailscale derper (Go)' if server == 'Tailscale' \
          else 'Hyper-DERP (C++/io_uring)'
      lines.append(f'### {label}')
      lines.append('')
      lines.append('```')
      # Extract just the counter lines.
      for line in txt.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith('#'):
          lines.append(stripped)
      lines.append('```')
      lines.append('')

    # Comparison table.
    ts_txt = perf.get('Tailscale', '')
    hd_txt = perf.get('Hyper-DERP', '')
    if ts_txt and hd_txt:
      lines.append('### Key Metrics')
      lines.append('')
      lines.append('| Metric | Tailscale | Hyper-DERP |')
      lines.append('|--------|-----------|------------|')

      def extract_metric(txt, name):
        for line in txt.splitlines():
          if name in line:
            parts = line.strip().split()
            if parts:
              return parts[0].replace(',', '')
        return 'N/A'

      for metric in ['task-clock', 'context-switches',
                     'cpu-migrations']:
        ts_v = extract_metric(ts_txt, metric)
        hd_v = extract_metric(hd_txt, metric)
        lines.append(f'| {metric} | {ts_v} | {hd_v} |')
      lines.append('')

  # Summary.
  lines.append('## Summary')
  lines.append('')
  lines.append('Hyper-DERP (io_uring, C++) vs Tailscale derper '
               '(Go) on bare metal:')
  lines.append('')

  # Throughput.
  lines.append('### Throughput')
  lines.append('')
  lines.append(f'- **TS peak**: {ts_peak:.0f} Mbps')
  lines.append(f'- **HD peak**: {hd_peak:.0f} Mbps')
  if ts_peak > 0:
    lines.append(f'- HD delivers **{hd_peak/ts_peak:.1f}x** peak '
                 f'throughput')
  lines.append('')

  # Latency.
  if lat:
    lines.append('### Idle Latency')
    lines.append('')
    ts_lat = lat.get('Tailscale', {})
    hd_lat = lat.get('Hyper-DERP', {})
    for m in ['p50', 'p90', 'p99', 'p999']:
      ts_v = ts_lat.get(m, 0)
      hd_v = hd_lat.get(m, 0)
      if ts_v and hd_v:
        if hd_v < ts_v:
          lines.append(
              f'- **{m}**: HD {hd_v:.0f} us vs TS {ts_v:.0f} us '
              f'(**HD {ts_v/hd_v:.1f}x faster**)')
        else:
          lines.append(
              f'- **{m}**: HD {hd_v:.0f} us vs TS {ts_v:.0f} us '
              f'(TS {hd_v/ts_v:.1f}x faster)')
    lines.append('')

  # Loaded latency.
  if loaded:
    lines.append('### Loaded Latency')
    lines.append('')
    for bg_rate in [500, 2000]:
      ts_data = loaded.get('Tailscale', {}).get(bg_rate, {})
      hd_data = loaded.get('Hyper-DERP', {}).get(bg_rate, {})
      if ts_data and hd_data:
        ts_p50 = ts_data.get('p50', 0)
        hd_p50 = hd_data.get('p50', 0)
        if ts_p50 and hd_p50:
          lines.append(
              f'- **{bg_rate} Mbps p50**: HD {hd_p50:.0f} us '
              f'vs TS {ts_p50:.0f} us '
              f'({hd_p50/ts_p50:.1f}x)')
    lines.append('')

  # Loss comparison.
  lines.append('### Loss')
  lines.append('')
  for rate in RATES:
    ts_d = scale['Tailscale'].get(rate, {})
    hd_d = scale['Hyper-DERP'].get(rate, {})
    ts_loss = ts_d.get('message_loss_pct', 0)
    hd_loss = hd_d.get('message_loss_pct', 0)
    if ts_loss > 0.01 or hd_loss > 0.01:
      lines.append(
          f'- **{rate:,} Mbps**: TS {ts_loss:.2f}%, '
          f'HD {hd_loss:.2f}%')
  lines.append('')

  # Efficiency.
  if perf:
    ts_txt = perf.get('Tailscale', '')
    hd_txt = perf.get('Hyper-DERP', '')
    if ts_txt and hd_txt:
      lines.append('### CPU Efficiency')
      lines.append('')

      def extract_float(txt, name):
        for line in txt.splitlines():
          if name in line:
            parts = line.strip().split()
            if parts:
              try:
                return float(parts[0].replace(',', ''))
              except ValueError:
                pass
        return 0

      ts_clock = extract_float(ts_txt, 'task-clock')
      hd_clock = extract_float(hd_txt, 'task-clock')
      ts_ctx = extract_float(ts_txt, 'context-switches')
      hd_ctx = extract_float(hd_txt, 'context-switches')
      if ts_clock and hd_clock:
        lines.append(
            f'- **CPU time**: HD {hd_clock:.0f} ms vs '
            f'TS {ts_clock:.0f} ms '
            f'({ts_clock/hd_clock:.1f}x less CPU)')
      if ts_ctx and hd_ctx:
        lines.append(
            f'- **Context switches**: HD {hd_ctx:,.0f} vs '
            f'TS {ts_ctx:,.0f} '
            f'({ts_ctx/hd_ctx:.0f}x fewer)')
      lines.append('')

  return '\n'.join(lines)


def main():
  sysinfo = load_system_info()
  scale = load_scale_data()
  lat = load_latency_data()
  loaded = load_loaded_latency()
  perf = load_perf_data()

  print('Generating throughput scaling plot...')
  plot_throughput_scaling(scale)

  print('Generating loss rate plot...')
  plot_loss_rate(scale)

  if lat:
    print('Generating idle latency bar chart...')
    plot_latency_bars(lat)
    print('Generating idle latency CDF...')
    plot_latency_cdf(lat)

  if loaded:
    print('Generating loaded latency plots...')
    plot_loaded_latency(loaded)
    plot_loaded_latency_cdf(loaded)

  print('Generating markdown...')
  md = generate_markdown(scale, lat, loaded, perf, sysinfo)
  md_path = os.path.join(OUT_DIR, 'BARE_METAL_COMPARISON.md')
  with open(md_path, 'w') as f:
    f.write(md)

  print(f'Report: {md_path}')
  print('Done.')


if __name__ == '__main__':
  main()
