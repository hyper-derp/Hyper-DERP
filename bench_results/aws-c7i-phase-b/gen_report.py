#!/usr/bin/env python3
"""Generate AWS Phase B (kTLS) sweep report with plots."""

import json
import glob
import math
import os
import re

BASE = os.path.dirname(os.path.abspath(__file__))
PLOTS = os.path.join(BASE, "plots")
os.makedirs(PLOTS, exist_ok=True)

try:
  import matplotlib
  matplotlib.use("Agg")
  import matplotlib.pyplot as plt
  HAS_MPL = True
except ImportError:
  HAS_MPL = False
  print("WARNING: matplotlib not available, skipping plots")

# --- Stats ---

T_TABLE = {
  2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571,
  7: 2.447, 8: 2.365, 9: 2.306, 10: 2.262, 11: 2.228,
  12: 2.201, 13: 2.179, 14: 2.160, 15: 2.145, 16: 2.131,
  17: 2.120, 18: 2.110, 19: 2.101, 20: 2.093, 21: 2.086,
  22: 2.080, 23: 2.074, 24: 2.069, 25: 2.064, 26: 2.060,
  30: 2.045, 50: 2.009, 100: 1.984
}

def t_crit(n):
  if n in T_TABLE:
    return T_TABLE[n]
  if n > 100:
    return 1.96
  keys = sorted(T_TABLE.keys())
  for i in range(len(keys) - 1):
    if keys[i] <= n <= keys[i + 1]:
      f = (n - keys[i]) / (keys[i + 1] - keys[i])
      return T_TABLE[keys[i]] * (1 - f) + T_TABLE[keys[i + 1]] * f
  return 2.0

def stats(vals):
  n = len(vals)
  if n == 0:
    return None
  mean = sum(vals) / n
  if n > 1:
    var = sum((x - mean) ** 2 for x in vals) / (n - 1)
    sd = math.sqrt(var)
    ci = t_crit(n) * sd / math.sqrt(n)
    cv = (sd / mean * 100) if mean != 0 else 0
  else:
    sd = ci = cv = 0
  return {
    "n": n, "mean": mean, "sd": sd, "ci": ci, "cv": cv,
    "min": min(vals), "max": max(vals)
  }

# --- Data loading ---

CONFIGS = ["16vcpu", "8vcpu"]
CONFIG_LABELS = {
  "16vcpu": "16 vCPU (8w)",
  "8vcpu": "8 vCPU (4w)",
}
COLORS = {"hd": "#e74c3c", "ts": "#3498db"}
LABELS = {"hd": "Hyper-DERP (kTLS)", "ts": "Go derper (TLS)"}

def load_rate_data(config):
  rate_dir = os.path.join(BASE, config, "rate")
  data = {}
  for f in glob.glob(os.path.join(rate_dir, "*.json")):
    fname = os.path.basename(f).replace(".json", "")
    m = re.match(r'(.+)_r(\d+)$', fname)
    if not m:
      continue
    prefix = m.group(1)
    parts = prefix.split("_")
    try:
      rate = int(parts[-1])
      label = "_".join(parts[:-1])
    except ValueError:
      continue
    try:
      with open(f) as fh:
        d = json.load(fh)
      tp = d.get("throughput_mbps", 0)
      loss = d.get("message_loss_pct", 0) or 0
    except Exception:
      continue
    key = (label, rate)
    if key not in data:
      data[key] = {"tp": [], "loss": []}
    data[key]["tp"].append(tp)
    data[key]["loss"].append(loss)
  return data

def load_latency_data(config):
  lat_dir = os.path.join(BASE, config, "latency")
  data = {}
  for f in glob.glob(os.path.join(lat_dir, "*.json")):
    fname = os.path.basename(f).replace(".json", "")
    m = re.match(r'(.+)_r(\d+)$', fname)
    if not m:
      continue
    parts = m.group(1).split("_")
    server = parts[0]
    load = parts[1]
    key = (server, load)
    try:
      with open(f) as fh:
        d = json.load(fh)
      lat = d.get("latency_ns", {})
      if not lat:
        continue
      if key not in data:
        data[key] = {"p50": [], "p90": [], "p99": [],
                     "p999": [], "max": []}
      for p in ["p50", "p90", "p99", "p999", "max"]:
        v = lat.get(p, 0)
        if v:
          data[key][p].append(v / 1000)
    except Exception:
      continue
  return data

def load_system_info(config):
  path = os.path.join(BASE, config, "system_info.txt")
  info = {}
  try:
    with open(path) as f:
      for line in f:
        if ":" in line:
          k, v = line.split(":", 1)
          info[k.strip()] = v.strip()
  except Exception:
    pass
  return info

# --- Plots ---

def plot_throughput_all():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  fig, axes = plt.subplots(1, 2, figsize=(14, 5))
  for idx, config in enumerate(configs):
    ax = axes[idx]
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    for server in ["ts", "hd"]:
      x, y, err = [], [], []
      for rate in rates:
        key = (server, rate)
        if key not in data:
          continue
        s = stats(data[key]["tp"])
        x.append(rate / 1000)
        y.append(s["mean"])
        err.append(s["ci"])
      ax.errorbar(x, y, yerr=err, marker="o", markersize=4,
                  color=COLORS[server], label=LABELS[server],
                  capsize=3, linewidth=1.5)
    max_rate = max(rates) / 1000
    ax.plot([0, max_rate], [0, max_rate * 1000], ":",
            color="gray", alpha=0.5, label="Wire rate")
    ax.set_title(CONFIG_LABELS[config], fontsize=12,
                 fontweight="bold")
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Delivered Throughput (Mbps)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
  fig.suptitle(
    "Throughput with TLS — AWS c7i (n=25)",
    fontsize=14, fontweight="bold")
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "throughput_tls.png"), dpi=150)
  plt.close()

def plot_loss_all():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  fig, axes = plt.subplots(1, 2, figsize=(14, 5))
  for idx, config in enumerate(configs):
    ax = axes[idx]
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    for server in ["ts", "hd"]:
      x, y, err = [], [], []
      for rate in rates:
        key = (server, rate)
        if key not in data:
          continue
        s = stats(data[key]["loss"])
        x.append(rate / 1000)
        y.append(s["mean"])
        err.append(s["ci"])
      ax.errorbar(x, y, yerr=err, marker="o", markersize=4,
                  color=COLORS[server], label=LABELS[server],
                  capsize=3, linewidth=1.5)
    ax.set_title(CONFIG_LABELS[config], fontsize=12,
                 fontweight="bold")
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Message Loss (%)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=-2, top=100)
  fig.suptitle(
    "Message Loss with TLS — AWS c7i (n=25)",
    fontsize=14, fontweight="bold")
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "loss_tls.png"), dpi=150)
  plt.close()

def plot_ratio():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  config_colors = {
    "2vcpu": "#e74c3c", "4vcpu": "#e67e22",
    "8vcpu": "#2ecc71", "16vcpu": "#3498db",
  }
  fig, ax = plt.subplots(figsize=(12, 6))
  bar_width = 0.18
  all_rates = set()
  ratios = {}
  for config in configs:
    data = load_rate_data(config)
    for (label, rate) in data.keys():
      if label == "hd":
        ts_key = ("ts", rate)
        if ts_key in data:
          hd_s = stats(data[("hd", rate)]["tp"])
          ts_s = stats(data[ts_key]["tp"])
          if hd_s and ts_s and ts_s["mean"] > 100:
            all_rates.add(rate)
            if config not in ratios:
              ratios[config] = {}
            ratios[config][rate] = hd_s["mean"] / ts_s["mean"]
  show_rates = sorted([r for r in all_rates if r >= 2000])
  x = range(len(show_rates))
  for i, config in enumerate(configs):
    vals = [ratios.get(config, {}).get(r, 0) for r in show_rates]
    offset = (i - len(configs) / 2 + 0.5) * bar_width
    bars = ax.bar([xi + offset for xi in x], vals, bar_width,
                  label=CONFIG_LABELS[config],
                  color=config_colors[config], alpha=0.85)
    for bar, v in zip(bars, vals):
      if v > 1.05:
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height(), f"{v:.1f}x",
                ha="center", va="bottom", fontsize=7,
                fontweight="bold")
  ax.axhline(y=1.0, color="gray", linestyle="--", alpha=0.5)
  ax.set_xticks(list(x))
  ax.set_xticklabels([f"{r/1000:.1f}G" for r in show_rates])
  ax.set_xlabel("Offered Rate")
  ax.set_ylabel("HD kTLS / TS TLS Throughput Ratio")
  ax.set_title("Hyper-DERP Advantage with TLS by vCPU Count",
               fontsize=14, fontweight="bold")
  ax.legend()
  ax.grid(True, alpha=0.3, axis="y")
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "ratio_tls.png"), dpi=150)
  plt.close()

def plot_cost_story():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  vcpus = [8, 16]
  hd_peak, ts_peak, hd_lossless = [], [], []
  for config in configs:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    best_hd, best_ts, best_ll = 0, 0, 0
    for rate in rates:
      hd_key, ts_key = ("hd", rate), ("ts", rate)
      if hd_key in data:
        s = stats(data[hd_key]["tp"])
        ls = stats(data[hd_key]["loss"])
        if s["mean"] > best_hd:
          best_hd = s["mean"]
        if ls["mean"] < 1.0 and s["mean"] > best_ll:
          best_ll = s["mean"]
      if ts_key in data:
        s = stats(data[ts_key]["tp"])
        if s["mean"] > best_ts:
          best_ts = s["mean"]
    hd_peak.append(best_hd)
    ts_peak.append(best_ts)
    hd_lossless.append(best_ll)

  fig, ax = plt.subplots(figsize=(10, 6))
  x = range(len(vcpus))
  w = 0.25
  ax.bar([xi - w for xi in x], [v / 1000 for v in ts_peak], w,
         label="TS TLS Peak", color=COLORS["ts"], alpha=0.85)
  ax.bar([xi for xi in x], [v / 1000 for v in hd_lossless], w,
         label="HD kTLS Lossless (<1%)", color="#e74c3c", alpha=0.6)
  ax.bar([xi + w for xi in x], [v / 1000 for v in hd_peak], w,
         label="HD kTLS Peak", color=COLORS["hd"], alpha=0.85)
  for i in x:
    ratio = hd_peak[i] / ts_peak[i] if ts_peak[i] > 0 else 0
    ax.text(i + w, hd_peak[i] / 1000 + 0.1, f"{ratio:.1f}x",
            ha="center", va="bottom", fontweight="bold",
            fontsize=10)
  ax.set_xticks(list(x))
  ax.set_xticklabels([f"{v} vCPU" for v in vcpus])
  ax.set_ylabel("Throughput (Gbps)")
  ax.set_title("Peak Throughput with TLS by vCPU Count",
               fontsize=14, fontweight="bold")
  ax.legend()
  ax.grid(True, alpha=0.3, axis="y")
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "cost_story_tls.png"), dpi=150)
  plt.close()

def plot_latency_ratio():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  load_order = ["idle", "ceil25", "ceil50", "ceil75", "ceil100",
                "ceil150"]
  load_labels = ["Idle", "25%", "50%", "75%", "100%", "150%"]
  config_colors = {
    "2vcpu": "#e74c3c", "4vcpu": "#e67e22",
    "8vcpu": "#2ecc71", "16vcpu": "#3498db",
  }
  fig, axes = plt.subplots(1, 2, figsize=(14, 6))
  for ax, pctl in zip(axes, ["p50", "p99"]):
    for config in configs:
      data = load_latency_data(config)
      ratios = []
      for load in load_order:
        ts_key = ("ts", load)
        hd_key = ("hd", load)
        if (ts_key in data and hd_key in data and
            data[ts_key][pctl] and data[hd_key][pctl]):
          ts_m = sum(data[ts_key][pctl]) / len(data[ts_key][pctl])
          hd_m = sum(data[hd_key][pctl]) / len(data[hd_key][pctl])
          ratios.append(ts_m / hd_m if hd_m > 0 else 0)
        else:
          ratios.append(0)
      ax.plot(range(len(load_order)), ratios, marker="o",
              label=CONFIG_LABELS[config],
              color=config_colors[config], linewidth=2)
    ax.axhline(y=1.0, color="gray", linestyle="--", alpha=0.5)
    ax.set_xticks(range(len(load_order)))
    ax.set_xticklabels(load_labels)
    ax.set_xlabel("Background Load (% of TS TLS Ceiling)")
    ax.set_ylabel(f"TS/HD {pctl} Ratio (higher = HD wins)")
    ax.set_title(f"{pctl} Latency Ratio with TLS",
                 fontsize=12, fontweight="bold")
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)
  fig.suptitle("Latency Advantage with TLS — TS/HD Ratio",
               fontsize=14, fontweight="bold")
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "latency_ratio_tls.png"),
              dpi=150)
  plt.close()

def plot_latency_all():
  if not HAS_MPL:
    return
  configs = ["8vcpu", "16vcpu"]
  load_order = ["idle", "ceil25", "ceil50", "ceil75", "ceil100",
                "ceil150"]
  load_labels = ["Idle", "25%", "50%", "75%", "100%", "150%"]
  fig, axes = plt.subplots(2, 2, figsize=(12, 10))
  for col, config in enumerate(configs):
    data = load_latency_data(config)
    for row, pctl in enumerate(["p50", "p99"]):
      ax = axes[row][col]
      x_pos = range(len(load_order))
      for server in ["ts", "hd"]:
        vals, errs = [], []
        for load in load_order:
          key = (server, load)
          if key in data and data[key][pctl]:
            s = stats(data[key][pctl])
            vals.append(s["mean"])
            errs.append(s["ci"])
          else:
            vals.append(0)
            errs.append(0)
        offset = -0.15 if server == "ts" else 0.15
        ax.bar([xi + offset for xi in x_pos], vals, 0.28,
               yerr=errs, color=COLORS[server],
               label=LABELS[server], capsize=2, alpha=0.85)
      ax.set_xticks(list(x_pos))
      ax.set_xticklabels(load_labels, fontsize=8)
      ax.set_ylabel(f"{pctl} (us)")
      if row == 0:
        ax.set_title(CONFIG_LABELS[config], fontsize=11,
                     fontweight="bold")
      ax.legend(fontsize=6)
      ax.grid(True, alpha=0.3, axis="y")
  fig.suptitle(
    "Latency Under Load with TLS (% of TS Ceiling)",
    fontsize=14, fontweight="bold")
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "latency_tls.png"), dpi=150)
  plt.close()

def plot_phase_a_vs_b():
  """Compare Phase A (TCP) vs Phase B (TLS) TS ceilings."""
  if not HAS_MPL:
    return
  phase_a_dir = os.path.join(os.path.dirname(BASE),
                             "gcp-c4-full-sweep")
  configs = ["8vcpu", "16vcpu"]
  # Map Phase A names (2vcpu_1w -> 2vcpu)
  phase_a_map = {
    "2vcpu": "2vcpu_1w", "4vcpu": "4vcpu",
    "8vcpu": "8vcpu", "16vcpu": "16vcpu"
  }
  vcpus = [8, 16]
  ts_tcp, ts_tls, hd_tcp, hd_tls = [], [], [], []

  for config in configs:
    # Phase B
    data_b = load_rate_data(config)
    best_ts_b, best_hd_b = 0, 0
    for (label, rate) in data_b:
      s = stats(data_b[(label, rate)]["tp"])
      if label == "ts" and s["mean"] > best_ts_b:
        best_ts_b = s["mean"]
      if label == "hd" and s["mean"] > best_hd_b:
        best_hd_b = s["mean"]
    ts_tls.append(best_ts_b)
    hd_tls.append(best_hd_b)

    # Phase A
    pa_config = phase_a_map[config]
    pa_rate_dir = os.path.join(phase_a_dir, pa_config, "rate")
    pa_data = {}
    if os.path.exists(pa_rate_dir):
      for f in glob.glob(os.path.join(pa_rate_dir, "*.json")):
        fname = os.path.basename(f).replace(".json", "")
        m2 = re.match(r'(.+)_r(\d+)$', fname)
        if not m2:
          continue
        parts = m2.group(1).split("_")
        try:
          rate = int(parts[-1])
          label = "_".join(parts[:-1])
        except ValueError:
          continue
        try:
          d = json.load(open(f))
          tp = d.get("throughput_mbps", 0)
        except Exception:
          continue
        if label not in pa_data:
          pa_data[label] = {}
        if rate not in pa_data[label]:
          pa_data[label][rate] = []
        pa_data[label][rate].append(tp)

    best_ts_a, best_hd_a = 0, 0
    for label in pa_data:
      for rate in pa_data[label]:
        s = stats(pa_data[label][rate])
        if "ts" in label and s["mean"] > best_ts_a:
          best_ts_a = s["mean"]
        if "hd" in label and s["mean"] > best_hd_a:
          best_hd_a = s["mean"]
    ts_tcp.append(best_ts_a)
    hd_tcp.append(best_hd_a)

  fig, ax = plt.subplots(figsize=(12, 6))
  x = range(len(vcpus))
  w = 0.2
  ax.bar([xi - 1.5 * w for xi in x],
         [v / 1000 for v in ts_tcp], w,
         label="TS Plain TCP", color="#3498db", alpha=0.5)
  ax.bar([xi - 0.5 * w for xi in x],
         [v / 1000 for v in ts_tls], w,
         label="TS TLS", color="#3498db", alpha=0.85)
  ax.bar([xi + 0.5 * w for xi in x],
         [v / 1000 for v in hd_tcp], w,
         label="HD Plain TCP", color="#e74c3c", alpha=0.5)
  ax.bar([xi + 1.5 * w for xi in x],
         [v / 1000 for v in hd_tls], w,
         label="HD kTLS", color="#e74c3c", alpha=0.85)
  ax.set_xticks(list(x))
  ax.set_xticklabels([f"{v} vCPU" for v in vcpus])
  ax.set_ylabel("Peak Throughput (Gbps)")
  ax.set_title("Phase A (TCP) vs Phase B (TLS) — Peak Throughput",
               fontsize=14, fontweight="bold")
  ax.legend()
  ax.grid(True, alpha=0.3, axis="y")
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "phase_a_vs_b.png"), dpi=150)
  plt.close()

# --- Report ---

def generate_report():
  lines = []
  w = lines.append

  w("# AWS Phase B Report — kTLS Sweep (c7i)")
  w("")
  w("**Date**: 2026-03-15")
  w("**Platform**: AWS c7i (Intel Xeon Platinum 8488C)")
  w("**Region**: eu-central-1 (Frankfurt)")
  w("**Payload**: 1400 bytes (WireGuard MTU)")
  w("**Protocol**: DERP over kTLS (HD) / TLS (TS)")
  w("**HD build**: P3 bitmask, ring size 4096, kTLS")
  w("**TS build**: v1.96.1 release, TLS")
  w("**Configs**: 16 vCPU (c7i.4xlarge) and 8 vCPU (c7i.2xlarge)")
  w("only. Smaller instances hit ENA bandwidth shaping.")
  w("")

  w("## Test Configuration")
  w("")
  w("| Config | vCPU | Workers | TS TLS Ceiling |")
  w("|--------|-----:|--------:|---------------:|")
  ts_ceilings = {}
  for config in CONFIGS:
    info = load_system_info(config)
    ceiling = info.get("ts_tls_ceiling", "?")
    ts_ceilings[config] = ceiling
    w(f"| {CONFIG_LABELS[config]} | {info.get('vcpu', '?')} "
      f"| {info.get('workers', '?')} | {ceiling}M |")
  w("")

  # Rate sweep
  w("## Throughput — Rate Sweep")
  w("")

  for config in CONFIGS:
    data = load_rate_data(config)
    rates = sorted(set(r for (l, r) in data.keys()))
    w(f"### {CONFIG_LABELS[config]}")
    w("")
    w("| Rate | HD kTLS | +/-CI | CV% | HD Loss"
      " | TS TLS | +/-CI | TS Loss | Ratio |")
    w("|-----:|-------:|------:|----:|-------:"
      "|------:|------:|-------:|------:|")
    for rate in rates:
      hd = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts = stats(data.get(("ts", rate), {"tp": []})["tp"])
      hd_l = stats(data.get(("hd", rate), {"loss": []})["loss"])
      ts_l = stats(data.get(("ts", rate), {"loss": []})["loss"])
      if not hd or not ts:
        continue
      ratio = hd["mean"] / ts["mean"] if ts["mean"] > 0 else 0
      cv_flag = " !" if hd["cv"] > 10 else ""
      w(f"| {rate} | {hd['mean']:.0f} | {hd['ci']:.0f} "
        f"| {hd['cv']:.1f}{cv_flag} | {hd_l['mean']:.2f}% "
        f"| {ts['mean']:.0f} | {ts['ci']:.0f} "
        f"| {ts_l['mean']:.2f}% | **{ratio:.2f}x** |")
    w("")

  w("![Throughput](plots/throughput_tls.png)")
  w("")
  w("![Loss](plots/loss_tls.png)")
  w("")
  w("![Ratio](plots/ratio_tls.png)")
  w("")

  # Summary
  w("## Summary: HD kTLS vs TS TLS")
  w("")
  w("| Config | TS TLS Ceiling | HD Lossless | HD Peak | Ratio |")
  w("|--------|---------------:|------------:|--------:|------:|")
  for config in CONFIGS:
    data = load_rate_data(config)
    rates = sorted(set(r for (l, r) in data.keys()))
    best_hd, best_ts, best_ll, ts_ceil = 0, 0, 0, 0
    for rate in rates:
      hd_key, ts_key = ("hd", rate), ("ts", rate)
      if hd_key in data:
        s = stats(data[hd_key]["tp"])
        ls = stats(data[hd_key]["loss"])
        if s["mean"] > best_hd:
          best_hd = s["mean"]
        if ls["mean"] < 1.0 and s["mean"] > best_ll:
          best_ll = s["mean"]
      if ts_key in data:
        s = stats(data[ts_key]["tp"])
        ls = stats(data[ts_key]["loss"])
        if s["mean"] > best_ts:
          best_ts = s["mean"]
        if ls["mean"] < 5.0 and s["mean"] > ts_ceil:
          ts_ceil = s["mean"]
    ratio = best_hd / best_ts if best_ts > 0 else 0
    w(f"| {CONFIG_LABELS[config]} | {ts_ceil/1000:.1f} Gbps "
      f"| {best_ll/1000:.1f} Gbps | {best_hd/1000:.1f} Gbps "
      f"| **{ratio:.1f}x** |")
  w("")
  w("![Cost Story](plots/cost_story_tls.png)")
  w("")

  # Cost comparison
  w("## The Cost Story")
  w("")
  w("| What you have | What you need with HD |")
  w("|:--------------|:----------------------|")

  # Find HD configs that match or exceed TS configs
  ts_peaks = {}
  hd_peaks = {}
  for config in CONFIGS:
    data = load_rate_data(config)
    for (label, rate) in data:
      s = stats(data[(label, rate)]["tp"])
      if label == "ts":
        if config not in ts_peaks or s["mean"] > ts_peaks[config]:
          ts_peaks[config] = s["mean"]
      if label == "hd":
        if config not in hd_peaks or s["mean"] > hd_peaks[config]:
          hd_peaks[config] = s["mean"]

  w(f"| TS on 16 vCPU: {ts_peaks.get('16vcpu',0)/1000:.1f} Gbps "
    f"| HD on 8 vCPU: {hd_peaks.get('8vcpu',0)/1000:.1f} Gbps "
    f"(2x smaller) |")
  w(f"| TS on 8 vCPU: {ts_peaks.get('8vcpu',0)/1000:.1f} Gbps "
    f"| HD on 4 vCPU: {hd_peaks.get('4vcpu',0)/1000:.1f} Gbps "
    f"(2x smaller) |")
  w(f"| TS on 4 vCPU: {ts_peaks.get('4vcpu',0)/1000:.1f} Gbps "
    f"| HD on 2 vCPU: {hd_peaks.get('2vcpu',0)/1000:.1f} Gbps "
    f"(2x smaller) |")
  w("")

  # Latency
  w("## Latency Under Load (with TLS)")
  w("")
  w("Background loads scaled to % of each config's TS TLS ceiling.")
  w("")

  load_order = ["idle", "ceil25", "ceil50", "ceil75",
                "ceil100", "ceil150"]
  load_labels = {
    "idle": "Idle", "ceil25": "25%", "ceil50": "50%",
    "ceil75": "75%", "ceil100": "100%", "ceil150": "150%"
  }

  for config in CONFIGS:
    data = load_latency_data(config)
    w(f"### {CONFIG_LABELS[config]}")
    w("")
    w("| Load | Srv | N | p50 (us) | p99 (us)"
      " | p999 (us) | max (us) |")
    w("|:-----|:---:|--:|---------:|--------:"
      "|---------:|---------:|")
    for load in load_order:
      for server in ["ts", "hd"]:
        key = (server, load)
        if key not in data or not data[key]["p50"]:
          continue
        p50 = stats(data[key]["p50"])
        p99 = stats(data[key]["p99"])
        p999 = stats(data[key]["p999"])
        mx = stats(data[key]["max"])
        w(f"| {load_labels.get(load, load)} "
          f"| {server.upper()} | {p50['n']} "
          f"| {p50['mean']:.0f} | {p99['mean']:.0f} "
          f"| {p999['mean']:.0f} | {mx['max']:.0f} |")

    w("")
    w("**Latency ratio (TS/HD):**")
    w("")
    w("| Load | p50 | p99 |")
    w("|:-----|----:|----:|")
    for load in load_order:
      ts_key = ("ts", load)
      hd_key = ("hd", load)
      if (ts_key in data and hd_key in data and
          data[ts_key]["p50"] and data[hd_key]["p50"]):
        ts_p50 = sum(data[ts_key]["p50"]) / len(data[ts_key]["p50"])
        hd_p50 = sum(data[hd_key]["p50"]) / len(data[hd_key]["p50"])
        ts_p99 = sum(data[ts_key]["p99"]) / len(data[ts_key]["p99"])
        hd_p99 = sum(data[hd_key]["p99"]) / len(data[hd_key]["p99"])
        r50 = ts_p50 / hd_p50 if hd_p50 > 0 else 0
        r99 = ts_p99 / hd_p99 if hd_p99 > 0 else 0
        w(f"| {load_labels.get(load, load)} "
          f"| {r50:.2f}x | {r99:.2f}x |")
    w("")

  w("![Latency](plots/latency_tls.png)")
  w("")
  w("![Latency Ratio](plots/latency_ratio_tls.png)")
  w("")

  # Key findings
  w("## Key Findings")
  w("")
  w("### 1. The advantage grows as resources shrink")
  w("")
  w("| Config | Throughput Ratio | p99 Ratio at TS Ceiling |")
  w("|--------|----------------:|-----------------------:|")
  for config in CONFIGS:
    data = load_rate_data(config)
    lat = load_latency_data(config)
    best_hd, best_ts = 0, 0
    for (label, rate) in data:
      s = stats(data[(label, rate)]["tp"])
      if label == "hd" and s["mean"] > best_hd:
        best_hd = s["mean"]
      if label == "ts" and s["mean"] > best_ts:
        best_ts = s["mean"]
    tp_r = best_hd / best_ts if best_ts > 0 else 0
    ts_p99 = lat.get(("ts", "ceil100"), {}).get("p99", [])
    hd_p99 = lat.get(("hd", "ceil100"), {}).get("p99", [])
    if ts_p99 and hd_p99:
      lat_r = (sum(ts_p99)/len(ts_p99)) / (sum(hd_p99)/len(hd_p99))
    else:
      lat_r = 0
    w(f"| {CONFIG_LABELS[config]} | {tp_r:.1f}x | {lat_r:.1f}x |")
  w("")

  w("### 2. Cross-cloud consistency")
  w("")
  w("Comparing HD/TS ratios at matched rates and vCPU counts:")
  w("")
  w("| Config | Rate | GCP ratio | AWS ratio |")
  w("|--------|-----:|----------:|----------:|")
  w("| 8 vCPU | 7.5G | 1.71x | 1.72x |")
  w("| 8 vCPU | 10G | 1.94x | 2.00x |")
  w("| 8 vCPU | 15G | 1.99x | 2.19x |")
  w("| 16 vCPU | 10G | 1.17x | 1.12x |")
  w("| 16 vCPU | 15G | 1.44x | 1.27x |")
  w("| 16 vCPU | 20G | 1.56x | 1.27x |")
  w("")
  w("**8 vCPU ratios are nearly identical across clouds.** The")
  w("advantage is architectural, not platform-specific.")
  w("")
  w("**16 vCPU ratios are lower on AWS** because HD plateaus at")
  w("~10.3 Gbps on AWS (vs ~12 Gbps on GCP). TS also performs")
  w("slightly better on AWS at 16 vCPU (8.1 Gbps vs 7.7 Gbps).")
  w("The Xeon 8488C and ENA may have different scheduling or")
  w("NIC offload characteristics.")
  w("")

  w("### 3. HD peaks at 10.3 Gbps on AWS 16 vCPU")
  w("")
  w("HD on AWS c7i.4xlarge plateaus at 10,304 Mbps from 15G to")
  w("20G offered — with CV of 0.2% and near-zero loss. This is")
  w("remarkably stable but lower than GCP's 12 Gbps peak.")
  w("Possible causes: ENA bandwidth characteristics, different")
  w("kernel TCP tuning defaults, or Xeon 8488C vs 8581C kTLS")
  w("throughput difference.")
  w("")

  w("### 4. AWS bandwidth shaping limits smaller instances")
  w("")
  w("c7i.2xlarge (8 vCPU) has a sustained baseline of ~2.5 Gbps")
  w("with burst to 12.5 Gbps. The 25-run high-rate sweeps can")
  w("deplete burst credits. Configs below 8 vCPU were not tested")
  w("because the bandwidth shaper — not the relay — would be the")
  w("bottleneck. For relay deployments on AWS, use c7i.4xlarge or")
  w("larger to avoid bandwidth constraints.")
  w("")

  # Methodology
  w("## Methodology")
  w("")
  w("- 25 runs at high rates, 5 at low rates")
  w("- Latency: 10-15 runs per load level, 4500 samples per run")
  w("- Background loads scaled to % of TS TLS ceiling per config")
  w("- TS ceiling determined by probe phase (3 runs x 5 rates)")
  w("- Strict isolation: one server at a time, cache drops between")
  w("- Go derper: v1.96.1, -trimpath -ldflags=\"-s -w\", TLS")
  w("- HD: P3 bitmask, kTLS, ring 4096, --metrics-port 9090")
  w("- `modprobe tls` verified on relay VM")
  w("- /proc/net/tls_stat checked for kTLS activation")
  w("")

  return "\n".join(lines)


if __name__ == "__main__":
  print("Generating plots...")
  plot_throughput_all()
  plot_loss_all()
  plot_ratio()
  plot_cost_story()
  plot_latency_all()
  plot_latency_ratio()
  # No phase_a_vs_b on AWS (no Phase A data)

  print("Generating report...")
  report = generate_report()
  report_path = os.path.join(BASE, "REPORT.md")
  with open(report_path, "w") as f:
    f.write(report)
  print(f"Report: {report_path}")
  print(f"Plots:  {PLOTS}/")
