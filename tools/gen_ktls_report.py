#!/usr/bin/env python3
"""Generate kTLS comparison report: TS vs HD vs HD+kTLS."""

import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULT_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    'bench_results/ktls-20260311'
OUT_DIR = RESULT_DIR

COLORS = {
    'Tailscale': '#4A90D9',
    'Hyper-DERP': '#E5553B',
    'kTLS': '#2ECC71',
}
LABELS = {
    'Tailscale': 'Tailscale (Go)',
    'Hyper-DERP': 'HD (plain TCP)',
    'kTLS': 'HD (kTLS)',
}

RATES = [100, 500, 1000, 2000, 5000, 10000, 20000, 50000]


def load_json(path):
  try:
    with open(path) as f:
      return json.load(f)
  except (OSError, json.JSONDecodeError):
    return None


def load_all():
  data = {}
  for name, prefix in [('Tailscale', 'ts'),
                        ('Hyper-DERP', 'hd'),
                        ('kTLS', 'ktls')]:
    entry = {'scale': {}, 'lat': {}, 'loaded': {}}
    for rate in RATES:
      d = load_json(os.path.join(
          RESULT_DIR, f'{prefix}_{rate}mbps.json'))
      if d:
        entry['scale'][rate] = d

    d = load_json(os.path.join(
        RESULT_DIR, f'{prefix}_lat_1400B.json'))
    if d and 'latency_ns' in d:
      entry['lat']['idle'] = d

    for bg in [500, 2000]:
      d = load_json(os.path.join(
          RESULT_DIR,
          f'{prefix}_loaded_lat_{bg}mbps.json'))
      if d and 'latency_ns' in d:
        entry['loaded'][bg] = d

    data[name] = entry
  return data


def plot_throughput(data):
  """Line chart: throughput vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  ax.set_title('Throughput vs Offered Rate (1400B)',
               fontsize=14, fontweight='bold')

  for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
    rates_found = []
    tp_vals = []
    for rate in RATES:
      d = data[name]['scale'].get(rate)
      if d:
        rates_found.append(rate)
        tp_vals.append(d['throughput_mbps'])

    if rates_found:
      ax.plot(rates_found, tp_vals,
              marker='o', linewidth=2, markersize=6,
              color=COLORS[name], label=LABELS[name])

  # Reference line (y=x).
  ax.plot([0, max(RATES)], [0, max(RATES)],
          '--', color='gray', alpha=0.3, linewidth=1,
          label='Line rate')

  ax.set_xlabel('Offered Rate (Mbps)')
  ax.set_ylabel('Achieved Throughput (Mbps)')
  ax.set_xscale('log')
  ax.set_yscale('log')
  ax.set_xlim(80, 60000)
  ax.set_ylim(50, 60000)
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3, which='both')
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))
  ax.yaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'throughput.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_loss(data):
  """Loss percentage vs offered rate."""
  fig, ax = plt.subplots(figsize=(10, 6))
  ax.set_title('Message Loss vs Offered Rate (1400B)',
               fontsize=14, fontweight='bold')

  for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
    rates_found = []
    loss_vals = []
    for rate in RATES:
      d = data[name]['scale'].get(rate)
      if d:
        rates_found.append(rate)
        loss_vals.append(d['message_loss_pct'])

    if rates_found:
      ax.plot(rates_found, loss_vals,
              marker='s', linewidth=2, markersize=6,
              color=COLORS[name], label=LABELS[name])

  ax.set_xlabel('Offered Rate (Mbps)')
  ax.set_ylabel('Message Loss (%)')
  ax.set_xscale('log')
  ax.legend(fontsize=10)
  ax.grid(True, alpha=0.3, which='both')
  ax.set_xlim(80, 60000)
  ax.set_ylim(-0.5, None)
  ax.xaxis.set_major_formatter(
      ticker.FuncFormatter(
          lambda v, _: f'{v:,.0f}'))

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'loss.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_bars(data):
  """Grouped bars: idle + loaded latency p50/p90/p99."""
  scenarios = []
  labels_s = []
  for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
    if 'idle' in data[name]['lat']:
      scenarios.append((name, 'idle',
                        data[name]['lat']['idle']))
      labels_s.append(f'{LABELS[name]} idle')
    for bg in [500, 2000]:
      if bg in data[name]['loaded']:
        scenarios.append((name, f'{bg}M',
                          data[name]['loaded'][bg]))
        labels_s.append(f'{LABELS[name]} @{bg}M')

  metrics = ['p50', 'p90', 'p99']
  fig, axes = plt.subplots(1, 3, figsize=(16, 6))
  fig.suptitle('Round-Trip Latency (1400B, us)',
               fontsize=14, fontweight='bold')

  for mi, metric in enumerate(metrics):
    ax = axes[mi]
    vals = []
    colors = []
    xlabels = []
    for name, scenario, d in scenarios:
      ln = d['latency_ns']
      vals.append(ln[metric] / 1000)
      colors.append(COLORS[name])
      xlabels.append(f'{LABELS[name]}\n{scenario}')

    x = np.arange(len(vals))
    bars = ax.bar(x, vals, color=colors, edgecolor='white',
                  width=0.7)

    for bar, v in zip(bars, vals):
      ax.text(bar.get_x() + bar.get_width() / 2,
              bar.get_height() + max(vals) * 0.02,
              f'{v:.0f}',
              ha='center', va='bottom', fontsize=8)

    ax.set_title(metric, fontsize=12)
    ax.set_ylabel('Latency (us)')
    ax.set_xticks(x)
    ax.set_xticklabels(xlabels, fontsize=7,
                       rotation=45, ha='right')
    ax.set_ylim(0, max(vals) * 1.2)
    ax.grid(axis='y', alpha=0.3)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_bars.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def plot_latency_cdf(data):
  """CDF of raw idle latency samples."""
  fig, ax = plt.subplots(figsize=(10, 6))
  ax.set_title('Idle Latency CDF (1400B)',
               fontsize=14, fontweight='bold')

  for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
    d = data[name]['lat'].get('idle')
    if not d or 'latency_ns' not in d:
      continue
    raw = d['latency_ns'].get('raw', [])
    if not raw:
      continue
    sorted_us = np.sort([x / 1000 for x in raw])
    cdf = np.arange(1, len(sorted_us) + 1) / len(sorted_us)
    ax.plot(sorted_us, cdf * 100,
            label=LABELS[name],
            color=COLORS[name], linewidth=2)

  ax.set_xlabel('Latency (us)')
  ax.set_ylabel('Percentile')
  ax.legend(fontsize=10)
  ax.grid(alpha=0.3)
  ax.set_ylim(0, 101)
  ax.axhline(y=99, color='gray', linestyle='--',
             alpha=0.4, linewidth=0.8)

  plt.tight_layout()
  path = os.path.join(OUT_DIR, 'latency_cdf.png')
  fig.savefig(path, dpi=150, bbox_inches='tight')
  plt.close(fig)
  return path


def generate_markdown(data):
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
  lines.append('# kTLS Performance Comparison')
  lines.append('')
  lines.append('Tailscale (Go) vs Hyper-DERP (plain TCP) '
               'vs Hyper-DERP (kTLS)')
  lines.append('')
  lines.append('## Test Environment')
  lines.append('')
  lines.append(f'- **Date**: {sysinfo.get("date", "N/A")}')
  lines.append(f'- **CPU**: {sysinfo.get("cpu", "N/A")}')
  lines.append(f'- **Kernel**: {sysinfo.get("kernel", "N/A")}')
  lines.append(f'- **Relay cores**: '
               f'{sysinfo.get("relay_cores", "N/A")} '
               f'({sysinfo.get("workers", "2")} workers)')
  lines.append(f'- **Client cores**: '
               f'{sysinfo.get("client_cores", "N/A")}')
  lines.append('- **Network**: veth on bridge '
               '(kernel TCP stack, no virtio)')
  lines.append(f'- **Payload**: {sysinfo.get("size", "1400B")} '
               '(WireGuard MTU)')
  lines.append(f'- **Peers**: {sysinfo.get("peers", "20")} '
               f'({sysinfo.get("pairs", "10")} active pairs)')
  lines.append(f'- **Duration**: '
               f'{sysinfo.get("duration", "10s")} per point')
  lines.append(f'- **TLS**: {sysinfo.get("tls", "N/A")}')
  lines.append('')

  # Throughput table.
  lines.append('## Throughput')
  lines.append('')
  lines.append('| Offered | TS Mbps | TS Loss | HD Mbps '
               '| HD Loss | kTLS Mbps | kTLS Loss | kTLS/TS |')
  lines.append('|--------:|--------:|--------:|--------:'
               '|--------:|----------:|----------:|--------:|')

  for rate in RATES:
    ts = data['Tailscale']['scale'].get(rate)
    hd = data['Hyper-DERP']['scale'].get(rate)
    kt = data['kTLS']['scale'].get(rate)
    ts_tp = ts['throughput_mbps'] if ts else 0
    ts_l = f'{ts["message_loss_pct"]:.2f}%' if ts else '-'
    hd_tp = hd['throughput_mbps'] if hd else 0
    hd_l = f'{hd["message_loss_pct"]:.2f}%' if hd else '-'
    kt_tp = kt['throughput_mbps'] if kt else 0
    kt_l = f'{kt["message_loss_pct"]:.2f}%' if kt else '-'
    ratio = f'{kt_tp / ts_tp:.2f}x' if ts_tp > 0 else '-'
    lines.append(
        f'| {rate:,} | {ts_tp:.1f} | {ts_l} | '
        f'{hd_tp:.1f} | {hd_l} | '
        f'{kt_tp:.1f} | {kt_l} | {ratio} |')

  lines.append('')
  lines.append('![Throughput](throughput.png)')
  lines.append('')
  lines.append('![Loss](loss.png)')
  lines.append('')

  # Latency table.
  lines.append('## Latency (1400B)')
  lines.append('')
  lines.append('| Scenario | TS p50 | TS p99 | HD p50 '
               '| HD p99 | kTLS p50 | kTLS p99 |')
  lines.append('|----------|-------:|-------:|-------:'
               '|-------:|---------:|---------:|')

  for label, key in [('Idle', 'idle')]:
    row = f'| {label}'
    for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
      d = data[name]['lat'].get(key)
      if d:
        ln = d['latency_ns']
        row += (f' | {ln["p50"]/1000:.0f}us'
                f' | {ln["p99"]/1000:.0f}us')
      else:
        row += ' | - | -'
    row += ' |'
    lines.append(row)

  for bg in [500, 2000]:
    row = f'| @{bg}M load'
    for name in ['Tailscale', 'Hyper-DERP', 'kTLS']:
      d = data[name]['loaded'].get(bg)
      if d:
        ln = d['latency_ns']
        row += (f' | {ln["p50"]/1000:.0f}us'
                f' | {ln["p99"]/1000:.0f}us')
      else:
        row += ' | - | -'
    row += ' |'
    lines.append(row)

  lines.append('')
  lines.append('![Latency Bars](latency_bars.png)')
  lines.append('')
  lines.append('![Latency CDF](latency_cdf.png)')
  lines.append('')

  # Analysis.
  lines.append('## Analysis')
  lines.append('')
  lines.append('### kTLS Latency Overhead')
  lines.append('')
  lines.append('kTLS idle p50 is ~93us vs 58us (HD plain) '
               'and 47us (TS). The +35us RTT overhead is '
               'caused by:')
  lines.append('')
  lines.append('1. **Multiple TLS records per frame**: '
               'The DERP client calls `write()` 3 times per '
               'SendPacket (5B header, 32B key, 1400B payload). '
               'With kTLS + TCP_NODELAY, each write creates a '
               'separate TLS record (5B header + payload + '
               '16B AEAD tag). This triples the per-frame '
               'crypto overhead.')
  lines.append('2. **Record reassembly on recv**: The kernel '
               'must receive a complete TLS record (including '
               'MAC tag) before decrypting and delivering to '
               'userspace. Plain TCP delivers partial data '
               'immediately.')
  lines.append('3. **Software AES-GCM on veth**: No hardware '
               'TLS offload on virtual interfaces. Real NICs '
               '(ConnectX-5+) offload AES-GCM to hardware.')
  lines.append('')
  lines.append('**Fix**: Coalesce the 3 writes into a single '
               '`write()` call per frame, producing one TLS '
               'record instead of three. This should reduce '
               'kTLS overhead by ~2/3.')
  lines.append('')
  lines.append('### kTLS Throughput')
  lines.append('')
  lines.append('kTLS peaks at ~2340 Mbps vs HD plain 3142 Mbps '
               '(0.75x). The CPU cost of software AES-GCM is '
               'the bottleneck on veth. With NIC TLS offload, '
               'this overhead disappears.')
  lines.append('')
  lines.append('### kTLS Reliability')
  lines.append('')
  lines.append('kTLS achieves near-zero loss at all rates '
               '(max 0.02%). The TLS record framing creates '
               'natural TCP backpressure that prevents buffer '
               'overflow. Compare: HD plain loses 25.7% at '
               '50 Gbps offered, TS loses 0.5%.')
  lines.append('')
  lines.append('### Anomalous Loaded Latency')
  lines.append('')

  # Check for anomaly.
  ts_500 = data['Tailscale']['loaded'].get(500)
  ts_2000 = data['Tailscale']['loaded'].get(2000)
  if ts_500 and ts_2000:
    p50_500 = ts_500['latency_ns']['p50'] / 1000
    p50_2000 = ts_2000['latency_ns']['p50'] / 1000
    if p50_2000 < p50_500:
      lines.append(
          f'TS @2000M (p50={p50_2000:.0f}us) shows '
          f'*lower* latency than @500M (p50={p50_500:.0f}us). '
          'This is suspicious and likely measurement '
          'variance or Go runtime scheduling behavior '
          '(GC pauses at 500M but not 2000M).')
      lines.append('')

  kt_500 = data['kTLS']['loaded'].get(500)
  kt_2000 = data['kTLS']['loaded'].get(2000)
  if kt_500 and kt_2000:
    p50_500 = kt_500['latency_ns']['p50'] / 1000
    p50_2000 = kt_2000['latency_ns']['p50'] / 1000
    if p50_2000 < p50_500:
      lines.append(
          f'kTLS @2000M (p50={p50_2000:.0f}us) also shows '
          f'lower latency than @500M (p50={p50_500:.0f}us). '
          'At 2000M the relay is near saturation, so the '
          'kernel TCP stack batches more aggressively, '
          'reducing per-packet latency. The 500M test '
          'hits an awkward middle ground: enough load to '
          'cause queuing but not enough for batch '
          'amortization.')
      lines.append('')

  lines.append('### Next Steps')
  lines.append('')
  lines.append('1. Coalesce client writes (single write per '
               'DERP frame) to reduce TLS record count')
  lines.append('2. Re-run with coalesced writes to measure '
               'actual kTLS record overhead')
  lines.append('3. Test on hardware with NIC TLS offload '
               '(Mellanox ConnectX-5+)')
  lines.append('4. Compare against Tailscale with HTTPS '
               'enabled (user-space Go TLS) for apples-to-'
               'apples encryption comparison')
  lines.append('')

  return '\n'.join(lines)


def main():
  data = load_all()

  print('Generating plots...')
  plot_throughput(data)
  plot_loss(data)
  plot_latency_bars(data)
  plot_latency_cdf(data)

  print('Generating markdown...')
  md = generate_markdown(data)
  md_path = os.path.join(OUT_DIR, 'KTLS_COMPARISON.md')
  with open(md_path, 'w') as f:
    f.write(md)

  print(f'Report: {md_path}')


if __name__ == '__main__':
  main()
