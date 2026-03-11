#!/usr/bin/env python3
"""Generate VM-based comparison report: Hyper-DERP vs Tailscale derper.

Reads scale test JSON (throughput vs rate) and latency JSON (1400B
ping/echo) from the vm_scale/ directory.
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
    'bench_results/comparison-20260311/vm_scale'
OUT_DIR = RESULT_DIR

COLORS = {'Tailscale': '#4A90D9', 'Hyper-DERP': '#E5553B'}
RATES = [10, 50, 100, 250, 500, 1000, 2000, 5000]


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
      label = f'{rate}mbps' if rate > 0 else 'unlimitedmbps'
      d = load_json(os.path.join(RESULT_DIR, f'{prefix}_{label}.json'))
      if d:
        data[server][rate] = d
  return data


def load_latency_data():
  """Load 1400B latency ping/echo results."""
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


def plot_throughput_scaling(scale):
  """Line chart: delivered throughput (Mbps) vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  fig.suptitle(
      'Delivered Throughput vs Offered Rate\n'
      '(20 peers, 10 active pairs, 1400B, 5s, VM-to-VM)',
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

  # Ideal line (y=x scaled by actual delivery at low rates).
  ax.plot([0, max(RATES)], [0, max(RATES) / 2.3],
          '--', color='gray', alpha=0.4, linewidth=1,
          label='Ideal (rate/2.3)')

  ax.set_xlabel('Offered Rate (Mbps aggregate)')
  ax.set_ylabel('Delivered Throughput (Mbps)')
  ax.set_xscale('log')
  ax.set_yscale('log')
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3, which='both')
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:.0f}'))
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:.0f}'))

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
      '(20 peers, 10 active pairs, 1400B, 5s, VM-to-VM)',
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
  ax.set_ylim(-2, 100)
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:.0f}'))
  ax.axhline(y=1, color='orange', linestyle='--', alpha=0.5,
             linewidth=1)
  ax.text(RATES[0], 2, '1% loss threshold', fontsize=8,
          color='orange')

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'loss_rate.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_bars_1400(lat):
  """Bar chart: p50/p90/p99 latency at 1400B."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle(
      'Round-Trip Latency at 1400B (WireGuard MTU)\n'
      '(VM-to-VM via tap bridge)',
      fontsize=13, fontweight='bold')

  metrics = ['p50', 'p90', 'p99']
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
  path = os.path.join(OUT_DIR, 'latency_bars_1400B.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_cdf_1400(lat):
  """CDF of raw latency samples at 1400B."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle(
      'Latency CDF at 1400B (WireGuard MTU)\n'
      '(VM-to-VM via tap bridge)',
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


def plot_send_errors(scale):
  """Bar chart: send errors at high rates."""
  fig, ax = plt.subplots(figsize=(8, 5))
  fig.suptitle(
      'Send Errors vs Offered Rate\n'
      '(non-zero only)',
      fontsize=13, fontweight='bold')

  for server in ['Tailscale', 'Hyper-DERP']:
    rates_plot = []
    errs_plot = []
    for rate in RATES:
      d = scale[server].get(rate)
      if d and d.get('send_errors', 0) > 0:
        rates_plot.append(rate)
        errs_plot.append(d['send_errors'])
    if rates_plot:
      label = 'Tailscale derper' if server == 'Tailscale' \
          else 'Hyper-DERP'
      ax.bar([str(r) for r in rates_plot], errs_plot,
             label=label, color=COLORS[server],
             edgecolor='white', alpha=0.8)

  ax.set_xlabel('Offered Rate (Mbps)')
  ax.set_ylabel('Send Errors')
  ax.legend(fontsize=10)
  ax.grid(axis='y', alpha=0.3)
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'send_errors.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def generate_markdown(scale, lat):
  """Generate the VM comparison markdown report."""
  lines = []
  lines.append('# Hyper-DERP vs Tailscale derper: '
               'VM Relay Forwarding Test')
  lines.append('')
  lines.append('## Test Environment')
  lines.append('')
  lines.append('- **Date**: 2026-03-11')
  lines.append('- **Host CPU**: 13th Gen Intel Core i5-13600KF')
  lines.append('- **Host Kernel**: 6.12.73+deb13-amd64')
  lines.append('- **Relay VM**: 2 vCPU (pinned to cores 4-5), '
               '2GB RAM')
  lines.append('- **Client VM**: 2 vCPU (pinned to cores 6-7), '
               '2GB RAM')
  lines.append('- **Network**: tap bridge (virbr-targets), '
               '10.101.0.0/20')
  lines.append('- **Payload**: 1400B (WireGuard MTU)')
  lines.append('- **Topology**: 20 peers, 10 active '
               'sender/receiver pairs')
  lines.append('- **Duration**: 5 seconds per rate point')
  lines.append('- **Workers**: 2 (Hyper-DERP)')
  lines.append('')

  # Throughput scaling table.
  lines.append('## Throughput Scaling')
  lines.append('')
  lines.append('Delivered relay throughput (received at client) '
               'as offered send rate increases. Rate is token-bucket '
               'paced across all 10 sender threads.')
  lines.append('')
  lines.append('| Rate (Mbps) | TS Sent | TS Recv | TS Loss | '
               'TS Mbps | HD Sent | HD Recv | HD Loss | '
               'HD Mbps |')
  lines.append('|-------------|---------|---------|---------|'
               '---------|---------|---------|---------|'
               '---------|')

  for rate in RATES + [0]:
    label = f'{rate}' if rate > 0 else 'unlimited'
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

    lines.append(
        f'| {label} | '
        f'{fmt(ts, "messages_sent")} | '
        f'{fmt(ts, "messages_recv")} | '
        f'{fmt(ts, "message_loss_pct", True)} | '
        f'{fmt(ts, "throughput_mbps")} | '
        f'{fmt(hd, "messages_sent")} | '
        f'{fmt(hd, "messages_recv")} | '
        f'{fmt(hd, "message_loss_pct", True)} | '
        f'{fmt(hd, "throughput_mbps")} |')

  lines.append('')
  lines.append('![Throughput Scaling](throughput_scaling.png)')
  lines.append('')
  lines.append('![Loss Rate](loss_rate.png)')
  lines.append('')

  # Saturation analysis.
  lines.append('## Saturation Analysis')
  lines.append('')
  lines.append('Both relays deliver identical throughput up to '
               '~500 Mbps offered rate (perfect delivery). '
               'Beyond that:')
  lines.append('')

  # Find where each starts losing.
  for server, prefix in [('Tailscale', 'TS'), ('Hyper-DERP', 'HD')]:
    first_loss_rate = None
    for rate in RATES:
      d = scale[server].get(rate, {})
      if d and d.get('message_loss_pct', 0) > 0.1:
        first_loss_rate = rate
        break
    if first_loss_rate:
      d = scale[server][first_loss_rate]
      lines.append(
          f'- **{prefix}** first loses packets at '
          f'{first_loss_rate} Mbps '
          f'({d["message_loss_pct"]:.2f}% loss, '
          f'{d["throughput_mbps"]:.0f} Mbps delivered)')

  lines.append('')

  # Peak throughput.
  for server, prefix in [('Tailscale', 'TS'), ('Hyper-DERP', 'HD')]:
    peak_rate = 0
    peak_tp = 0
    for rate in RATES:
      d = scale[server].get(rate, {})
      if d and d.get('throughput_mbps', 0) > peak_tp:
        peak_tp = d['throughput_mbps']
        peak_rate = rate
    if peak_tp > 0:
      lines.append(
          f'- **{prefix}** peak: {peak_tp:.0f} Mbps '
          f'(at {peak_rate} Mbps offered)')

  lines.append('')

  # HD collapse.
  hd_5k = scale['Hyper-DERP'].get(5000, {})
  if hd_5k and hd_5k.get('send_errors', 0) > 0:
    lines.append(
        f'**Critical finding**: Hyper-DERP collapses at '
        f'5000 Mbps with {hd_5k["send_errors"]:,} send errors, '
        f'{hd_5k["message_loss_pct"]:.1f}% loss, '
        f'and only {hd_5k["throughput_mbps"]:.1f} Mbps delivered. '
        f'This indicates backpressure failure in the io_uring '
        f'data plane — likely SQ/CQ overflow or provided buffer '
        f'ring exhaustion causing send() to return errors '
        f'instead of blocking.')
    lines.append('')

  hd_unlim = scale['Hyper-DERP'].get(0, {})
  if hd_unlim and hd_unlim.get('connected_peers', 20) == 0:
    lines.append(
        'At unlimited rate, Hyper-DERP failed to accept '
        'connections entirely (0/20 connected), suggesting '
        'the relay process hung or crashed under extreme load.')
    lines.append('')

  if hd_5k or hd_unlim:
    lines.append('![Send Errors](send_errors.png)')
    lines.append('')

  # Latency section.
  if lat:
    lines.append('## Round-Trip Latency (1400B)')
    lines.append('')
    lines.append('Measured via ping/echo over tap bridge '
                 '(2000 round-trips, 200 warmup discarded).')
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
        lines.append(
            f'| {m} | {ts_v:.0f} us | '
            f'{hd_v:.0f} us | **{ratio:.1f}x** |')

    ts_pps = lat.get('Tailscale', {}).get('pps', 0)
    hd_pps = lat.get('Hyper-DERP', {}).get('pps', 0)
    if ts_pps and hd_pps:
      lines.append('')
      lines.append(
          f'Ping throughput: Hyper-DERP {hd_pps:,.0f} pps vs '
          f'Tailscale {ts_pps:,.0f} pps '
          f'(**{hd_pps/ts_pps:.1f}x**)')

    lines.append('')
    lines.append('![Latency Bars 1400B](latency_bars_1400B.png)')
    lines.append('')
    lines.append('![Latency CDF 1400B](latency_cdf_1400B.png)')
    lines.append('')

  # Summary — dynamically generated from results.
  lines.append('## Summary')
  lines.append('')
  lines.append('Hyper-DERP (io_uring, C++) vs Tailscale derper '
               '(Go):')
  lines.append('')

  # Latency comparison.
  if lat:
    ts_lat = lat.get('Tailscale', {})
    hd_lat = lat.get('Hyper-DERP', {})
    for m in ['p50', 'p90', 'p99', 'p999']:
      ts_v = ts_lat.get(m, 0)
      hd_v = hd_lat.get(m, 0)
      if ts_v and hd_v:
        if hd_v < ts_v:
          lines.append(
              f'- **{m} latency**: {hd_v:.0f} us vs '
              f'{ts_v:.0f} us '
              f'(**HD {ts_v/hd_v:.1f}x faster**)')
        elif hd_v > ts_v:
          lines.append(
              f'- **{m} latency**: {hd_v:.0f} us vs '
              f'{ts_v:.0f} us '
              f'(TS {hd_v/ts_v:.1f}x faster)')
        else:
          lines.append(
              f'- **{m} latency**: {hd_v:.0f} us '
              f'(equal)')

  # Throughput comparison at each rate point.
  lines.append('')
  lines.append('### Throughput')
  lines.append('')
  hd_wins = 0
  ts_wins = 0
  for rate in RATES:
    ts_d = scale['Tailscale'].get(rate, {})
    hd_d = scale['Hyper-DERP'].get(rate, {})
    ts_tp = ts_d.get('throughput_mbps', 0)
    hd_tp = hd_d.get('throughput_mbps', 0)
    if ts_tp and hd_tp:
      if hd_tp > ts_tp * 1.02:
        hd_wins += 1
      elif ts_tp > hd_tp * 1.02:
        ts_wins += 1

  # Find peak throughput for each.
  ts_peak = max(
      (scale['Tailscale'].get(r, {}).get(
          'throughput_mbps', 0) for r in RATES), default=0)
  hd_peak = max(
      (scale['Hyper-DERP'].get(r, {}).get(
          'throughput_mbps', 0) for r in RATES), default=0)
  lines.append(f'- **TS peak**: {ts_peak:.0f} Mbps')
  lines.append(f'- **HD peak**: {hd_peak:.0f} Mbps')
  if ts_peak and hd_peak:
    ratio = hd_peak / ts_peak
    if ratio > 1:
      lines.append(
          f'- HD delivers **{ratio:.2f}x** peak throughput')
    else:
      lines.append(
          f'- TS delivers **{1/ratio:.2f}x** peak throughput')

  # Loss comparison.
  lines.append('')
  lines.append('### Loss Behavior')
  lines.append('')
  for rate in RATES:
    ts_d = scale['Tailscale'].get(rate, {})
    hd_d = scale['Hyper-DERP'].get(rate, {})
    ts_loss = ts_d.get('message_loss_pct', 0)
    hd_loss = hd_d.get('message_loss_pct', 0)
    if ts_loss > 0.1 or hd_loss > 0.1:
      lines.append(
          f'- **{rate} Mbps**: TS {ts_loss:.2f}% loss, '
          f'HD {hd_loss:.2f}% loss')
  lines.append('')

  return '\n'.join(lines)


def main():
  scale = load_scale_data()
  lat = load_latency_data()

  print('Generating throughput scaling plot...')
  plot_throughput_scaling(scale)

  print('Generating loss rate plot...')
  plot_loss_rate(scale)

  if lat:
    print('Generating latency bar chart (1400B)...')
    plot_latency_bars_1400(lat)

    print('Generating latency CDF (1400B)...')
    plot_latency_cdf_1400(lat)

  # Check if any send errors exist.
  has_errors = False
  for server in scale:
    for rate in scale[server]:
      if scale[server][rate].get('send_errors', 0) > 0:
        has_errors = True
        break
  if has_errors:
    print('Generating send errors plot...')
    plot_send_errors(scale)

  print('Generating markdown...')
  md = generate_markdown(scale, lat)
  md_path = os.path.join(OUT_DIR, 'VM_COMPARISON.md')
  with open(md_path, 'w') as f:
    f.write(md)

  print(f'Report: {md_path}')
  print('Done.')


if __name__ == '__main__':
  main()
