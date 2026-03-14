#!/usr/bin/env python3
"""Generate full sweep report with plots."""

import json
import glob
import math
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
PLOTS = os.path.join(BASE, "plots")
os.makedirs(PLOTS, exist_ok=True)

try:
  import matplotlib
  matplotlib.use("Agg")
  import matplotlib.pyplot as plt
  import matplotlib.ticker as ticker
  HAS_MPL = True
except ImportError:
  HAS_MPL = False
  print("WARNING: matplotlib not available, skipping plots")


# --- Stats helpers ---

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
    "min": min(vals), "max": max(vals), "raw": vals
  }

def welch_t(s1, s2):
  """Welch's t-test. Returns t-stat and approximate DoF."""
  n1, n2 = s1["n"], s2["n"]
  m1, m2 = s1["mean"], s2["mean"]
  v1 = s1["sd"] ** 2 / n1 if n1 > 0 else 0
  v2 = s2["sd"] ** 2 / n2 if n2 > 0 else 0
  se = math.sqrt(v1 + v2) if (v1 + v2) > 0 else 1e-9
  t = (m1 - m2) / se
  if v1 + v2 > 0:
    dof = (v1 + v2) ** 2 / (
      (v1 ** 2 / (n1 - 1) if n1 > 1 else 1e-9) +
      (v2 ** 2 / (n2 - 1) if n2 > 1 else 1e-9)
    )
  else:
    dof = min(n1, n2) - 1
  return t, dof


# --- Data loading ---

CONFIGS = ["16vcpu", "8vcpu", "4vcpu", "2vcpu_1w", "2vcpu_2w"]
CONFIG_LABELS = {
  "16vcpu": "16 vCPU (8w)",
  "8vcpu": "8 vCPU (4w)",
  "4vcpu": "4 vCPU (2w)",
  "2vcpu_1w": "2 vCPU (1w)",
  "2vcpu_2w": "2 vCPU (2w)",
}

def load_rate_data(config):
  rate_dir = os.path.join(BASE, config, "rate")
  data = {}
  for f in glob.glob(os.path.join(rate_dir, "*.json")):
    try:
      with open(f) as fh:
        d = json.load(fh)
      parts = os.path.basename(f).replace(".json", "").split("_")
      server = parts[0]
      rate = int(parts[1])
      key = (server, rate)
      if key not in data:
        data[key] = {"tp": [], "loss": []}
      data[key]["tp"].append(d.get("throughput_mbps", 0))
      loss = d.get("message_loss_pct", 0)
      data[key]["loss"].append(loss if loss else 0)
    except Exception:
      pass
  return data

def load_latency_data(config):
  lat_dir = os.path.join(BASE, config, "latency")
  data = {}
  for f in glob.glob(os.path.join(lat_dir, "*.json")):
    try:
      with open(f) as fh:
        d = json.load(fh)
      parts = os.path.basename(f).replace(".json", "").split("_")
      server = parts[0]
      load = parts[1]
      key = (server, load)
      if key not in data:
        data[key] = {"p50": [], "p90": [], "p95": [], "p99": [],
                     "p999": [], "max": [], "mean": []}
      lat = d.get("latency_ns", d.get("latency", {}))
      if lat:
        for p in ["p50", "p90", "p95", "p99", "p999", "max", "mean"]:
          v = lat.get(p, 0)
          if v:
            data[key][p].append(v / 1000)  # ns -> us
    except Exception:
      pass
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


# --- Plot generation ---

COLORS = {"hd": "#e74c3c", "ts": "#3498db"}
LABELS = {"hd": "Hyper-DERP", "ts": "Tailscale derper"}

def plot_throughput_all():
  """Single figure with 2x2 subplots for all configs."""
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  fig, axes = plt.subplots(2, 2, figsize=(14, 10))
  axes = axes.flatten()

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
    ax.plot([0, max_rate], [0, max_rate * 1000], ":", color="gray",
            alpha=0.5, label="Wire rate")
    ax.set_title(CONFIG_LABELS[config], fontsize=12, fontweight="bold")
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Delivered Throughput (Mbps)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

  fig.suptitle(
    "Throughput — GCP c4-highcpu Full Sweep (n=25)",
    fontsize=14, fontweight="bold"
  )
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "throughput_all.png"), dpi=150)
  plt.close()

def plot_loss_all():
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  fig, axes = plt.subplots(2, 2, figsize=(14, 10))
  axes = axes.flatten()

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

    ax.set_title(CONFIG_LABELS[config], fontsize=12, fontweight="bold")
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Message Loss (%)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=-2, top=100)

  fig.suptitle(
    "Message Loss — GCP c4-highcpu Full Sweep (n=25)",
    fontsize=14, fontweight="bold"
  )
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "loss_all.png"), dpi=150)
  plt.close()

def plot_ratio_all():
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  fig, ax = plt.subplots(figsize=(12, 6))

  bar_width = 0.18
  config_colors = {
    "2vcpu_1w": "#e74c3c",
    "4vcpu": "#e67e22",
    "8vcpu": "#2ecc71",
    "16vcpu": "#3498db",
  }

  all_rates = set()
  ratios = {}
  for config in configs:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    for rate in rates:
      hd = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts = stats(data.get(("ts", rate), {"tp": []})["tp"])
      if hd and ts and ts["mean"] > 0:
        all_rates.add(rate)
        if config not in ratios:
          ratios[config] = {}
        ratios[config][rate] = hd["mean"] / ts["mean"]

  all_rates = sorted(all_rates)
  # Only show rates where TS has measurable throughput
  show_rates = [r for r in all_rates if r >= 3000]
  x = range(len(show_rates))

  for i, config in enumerate(configs):
    vals = [ratios.get(config, {}).get(r, 0) for r in show_rates]
    offset = (i - len(configs) / 2 + 0.5) * bar_width
    bars = ax.bar(
      [xi + offset for xi in x], vals, bar_width,
      label=CONFIG_LABELS[config], color=config_colors[config],
      alpha=0.85
    )
    for bar, v in zip(bars, vals):
      if v > 0:
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                f"{v:.1f}x", ha="center", va="bottom", fontsize=7,
                fontweight="bold")

  ax.axhline(y=1.0, color="gray", linestyle="--", alpha=0.5)
  ax.set_xticks(list(x))
  ax.set_xticklabels([f"{r/1000:.0f}G" for r in show_rates])
  ax.set_xlabel("Offered Rate")
  ax.set_ylabel("HD / TS Throughput Ratio")
  ax.set_title(
    "Hyper-DERP Advantage by vCPU Count",
    fontsize=14, fontweight="bold"
  )
  ax.legend()
  ax.grid(True, alpha=0.3, axis="y")
  ax.set_ylim(bottom=0)

  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "ratio_all.png"), dpi=150)
  plt.close()

def plot_latency_all():
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  load_order = ["idle", "ceil25", "ceil50", "ceil75", "ceil100",
                "ceil150"]
  load_labels = ["Idle", "25%", "50%", "75%", "100%", "150%"]

  # p50 and p99 side by side for each config
  fig, axes = plt.subplots(2, 4, figsize=(20, 10))

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
        ax.bar(
          [xi + offset for xi in x_pos], vals, 0.28,
          yerr=errs, color=COLORS[server], label=LABELS[server],
          capsize=2, alpha=0.85
        )

      ax.set_xticks(list(x_pos))
      ax.set_xticklabels(load_labels, fontsize=8)
      ax.set_ylabel(f"{pctl} Latency (us)")
      if row == 0:
        ax.set_title(CONFIG_LABELS[config], fontsize=11,
                     fontweight="bold")
      ax.legend(fontsize=7)
      ax.grid(True, alpha=0.3, axis="y")

  axes[0][0].set_ylabel("p50 Latency (us)")
  axes[1][0].set_ylabel("p99 Latency (us)")

  fig.suptitle(
    "Latency Under Load (% of TS Ceiling) — GCP c4-highcpu",
    fontsize=14, fontweight="bold"
  )
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "latency_all.png"), dpi=150)
  plt.close()

def plot_latency_ratio():
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  load_order = ["idle", "ceil25", "ceil50", "ceil75", "ceil100",
                "ceil150"]
  load_labels = ["Idle", "25%", "50%", "75%", "100%", "150%"]
  config_colors = {
    "2vcpu_1w": "#e74c3c",
    "4vcpu": "#e67e22",
    "8vcpu": "#2ecc71",
    "16vcpu": "#3498db",
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

      ax.plot(
        range(len(load_order)), ratios, marker="o",
        label=CONFIG_LABELS[config],
        color=config_colors[config], linewidth=2
      )

    ax.axhline(y=1.0, color="gray", linestyle="--", alpha=0.5)
    ax.set_xticks(range(len(load_order)))
    ax.set_xticklabels(load_labels)
    ax.set_xlabel("Background Load (% of TS Ceiling)")
    ax.set_ylabel(f"TS/HD {pctl} Ratio (higher = HD wins)")
    ax.set_title(f"{pctl} Latency Ratio", fontsize=12,
                 fontweight="bold")
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)

  fig.suptitle(
    "Latency Advantage — TS / HD Ratio Under Load",
    fontsize=14, fontweight="bold"
  )
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "latency_ratio.png"), dpi=150)
  plt.close()

def plot_cost_story():
  """The headline plot: what vCPU does TS need to match HD?"""
  if not HAS_MPL:
    return
  configs = ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]
  vcpus = [2, 4, 8, 16]

  hd_peak = []
  ts_peak = []
  hd_lossless = []  # Highest rate with < 1% loss

  for config in configs:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))

    best_hd = 0
    best_ts = 0
    best_hd_lossless = 0
    for rate in rates:
      hd_key = ("hd", rate)
      ts_key = ("ts", rate)
      if hd_key in data:
        s = stats(data[hd_key]["tp"])
        if s["mean"] > best_hd:
          best_hd = s["mean"]
        ls = stats(data[hd_key]["loss"])
        if ls["mean"] < 1.0 and s["mean"] > best_hd_lossless:
          best_hd_lossless = s["mean"]
      if ts_key in data:
        s = stats(data[ts_key]["tp"])
        if s["mean"] > best_ts:
          best_ts = s["mean"]
    hd_peak.append(best_hd)
    ts_peak.append(best_ts)
    hd_lossless.append(best_hd_lossless)

  fig, ax = plt.subplots(figsize=(10, 6))
  x = range(len(vcpus))
  w = 0.25
  ax.bar([xi - w for xi in x], [v / 1000 for v in ts_peak], w,
         label="TS Peak", color=COLORS["ts"], alpha=0.85)
  ax.bar([xi for xi in x], [v / 1000 for v in hd_lossless], w,
         label="HD Lossless (<1%)", color="#e74c3c", alpha=0.6)
  ax.bar([xi + w for xi in x], [v / 1000 for v in hd_peak], w,
         label="HD Peak", color=COLORS["hd"], alpha=0.85)

  for i in x:
    ratio = hd_peak[i] / ts_peak[i] if ts_peak[i] > 0 else 0
    ax.text(i + w, hd_peak[i] / 1000 + 0.2, f"{ratio:.1f}x",
            ha="center", va="bottom", fontweight="bold", fontsize=10)

  ax.set_xticks(list(x))
  ax.set_xticklabels([f"{v} vCPU" for v in vcpus])
  ax.set_ylabel("Throughput (Gbps)")
  ax.set_title(
    "Peak Throughput by vCPU Count",
    fontsize=14, fontweight="bold"
  )
  ax.legend()
  ax.grid(True, alpha=0.3, axis="y")
  ax.set_ylim(bottom=0)

  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "cost_story.png"), dpi=150)
  plt.close()

def plot_2w_comparison():
  """2 vCPU: 1 worker vs 2 workers."""
  if not HAS_MPL:
    return
  fig, axes = plt.subplots(1, 2, figsize=(14, 6))

  for ax, metric, ylabel in [
    (axes[0], "tp", "Throughput (Mbps)"),
    (axes[1], "loss", "Loss (%)")
  ]:
    for config, color, label in [
      ("2vcpu_1w", "#e74c3c", "HD 1 worker"),
      ("2vcpu_2w", "#e67e22", "HD 2 workers"),
    ]:
      data = load_rate_data(config)
      rates = sorted(set(r for (s, r) in data.keys()))
      x, y, err = [], [], []
      for rate in rates:
        key = ("hd", rate)
        if key not in data:
          continue
        s = stats(data[key][metric])
        x.append(rate / 1000)
        y.append(s["mean"])
        err.append(s["ci"])
      ax.errorbar(x, y, yerr=err, marker="o", markersize=4,
                  color=color, label=label, capsize=3, linewidth=1.5)

    # Also show TS for reference
    data_1w = load_rate_data("2vcpu_1w")
    rates = sorted(set(r for (s, r) in data_1w.keys()))
    x, y, err = [], [], []
    for rate in rates:
      key = ("ts", rate)
      if key not in data_1w:
        continue
      s = stats(data_1w[key][metric])
      x.append(rate / 1000)
      y.append(s["mean"])
      err.append(s["ci"])
    ax.errorbar(x, y, yerr=err, marker="s", markersize=4,
                color=COLORS["ts"], label="Tailscale derper",
                capsize=3, linewidth=1.5, linestyle="--")

    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel(ylabel)
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

  fig.suptitle(
    "2 vCPU Worker Count Comparison",
    fontsize=14, fontweight="bold"
  )
  plt.tight_layout()
  plt.savefig(os.path.join(PLOTS, "2vcpu_workers.png"), dpi=150)
  plt.close()


# --- Report generation ---

def generate_report():
  lines = []
  w = lines.append

  w("# Hyper-DERP Full Sweep Report — GCP c4-highcpu")
  w("")
  w("**Date**: 2026-03-14")
  w("**Platform**: GCP c4-highcpu (Intel Xeon Platinum 8581C)")
  w("**Region**: europe-west3-b (Frankfurt)")
  w("**Payload**: 1400 bytes (WireGuard MTU)")
  w("**Protocol**: DERP over plain TCP (no TLS)")
  w("")

  # System info table
  w("## Test Configuration")
  w("")
  w("| Config | vCPU | Workers | Low Runs | High Runs"
    " | Lat Runs | TS Ceiling |")
  w("|--------|-----:|--------:|---------:|----------:"
    "|---------:|-----------:|")
  for config in CONFIGS:
    info = load_system_info(config)
    w(f"| {CONFIG_LABELS.get(config, config)} "
      f"| {info.get('relay_vcpus', '?')} "
      f"| {info.get('relay_workers', '?')} "
      f"| {info.get('low_rates_runs', '?')} "
      f"| {info.get('high_rates_runs', '?')} "
      f"| {info.get('lat_runs_low', '?')} "
      f"| {info.get('ts_ceiling_actual', '?')}M |")
  w("")

  # --- Rate sweep tables ---
  w("## Rate Sweep Results")
  w("")

  for config in ["16vcpu", "8vcpu", "4vcpu", "2vcpu_1w"]:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))

    w(f"### {CONFIG_LABELS[config]}")
    w("")
    w("| Rate | HD (Mbps) | +/-CI | CV% | HD Loss"
      " | TS (Mbps) | +/-CI | CV% | TS Loss | Ratio |")
    w("|-----:|----------:|------:|----:|--------:"
      "|----------:|------:|----:|--------:|------:|")

    for rate in rates:
      hd_tp = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts_tp = stats(data.get(("ts", rate), {"tp": []})["tp"])
      hd_lo = stats(data.get(("hd", rate), {"loss": []})["loss"])
      ts_lo = stats(data.get(("ts", rate), {"loss": []})["loss"])

      if not hd_tp or not ts_tp:
        continue

      ratio = hd_tp["mean"] / ts_tp["mean"] if ts_tp["mean"] > 0 else 0
      cv_flag = " !" if hd_tp["cv"] > 10 else ""

      w(f"| {rate} "
        f"| {hd_tp['mean']:.0f} | {hd_tp['ci']:.0f} "
        f"| {hd_tp['cv']:.1f}{cv_flag} "
        f"| {hd_lo['mean']:.2f}% "
        f"| {ts_tp['mean']:.0f} | {ts_tp['ci']:.0f} "
        f"| {ts_tp['cv']:.1f} "
        f"| {ts_lo['mean']:.2f}% "
        f"| **{ratio:.2f}x** |")
    w("")

  w("![Throughput](plots/throughput_all.png)")
  w("")
  w("![Loss](plots/loss_all.png)")
  w("")
  w("![Ratio](plots/ratio_all.png)")
  w("")

  # --- Summary comparison ---
  w("## Summary: HD vs TS by vCPU Count")
  w("")
  w("| Config | TS Ceiling | HD Lossless Ceiling"
    " | HD Peak | Peak Ratio |")
  w("|--------|----------:|-----------:|--------:|----------:|")

  for config in ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    best_hd = 0
    best_ts = 0
    best_hd_ll = 0
    ts_ceil = 0

    for rate in rates:
      hd_key = ("hd", rate)
      ts_key = ("ts", rate)
      if hd_key in data:
        s = stats(data[hd_key]["tp"])
        if s and s["mean"] > best_hd:
          best_hd = s["mean"]
        ls = stats(data[hd_key]["loss"])
        if ls and ls["mean"] < 1.0 and s["mean"] > best_hd_ll:
          best_hd_ll = s["mean"]
      if ts_key in data:
        s = stats(data[ts_key]["tp"])
        ls = stats(data[ts_key]["loss"])
        if s and s["mean"] > best_ts:
          best_ts = s["mean"]
        if ls and ls["mean"] < 5.0 and s["mean"] > ts_ceil:
          ts_ceil = s["mean"]

    ratio = best_hd / best_ts if best_ts > 0 else 0
    w(f"| {CONFIG_LABELS[config]} "
      f"| {ts_ceil/1000:.1f} Gbps "
      f"| {best_hd_ll/1000:.1f} Gbps "
      f"| {best_hd/1000:.1f} Gbps "
      f"| **{ratio:.1f}x** |")

  w("")
  w("![Cost Story](plots/cost_story.png)")
  w("")

  # --- Latency ---
  w("## Latency Under Load")
  w("")
  w("Background load levels scaled to percentage of each config's")
  w("TS ceiling (determined by probe phase).")
  w("")

  load_order = ["idle", "ceil25", "ceil50", "ceil75", "ceil100",
                "ceil150"]
  load_labels = {
    "idle": "Idle", "ceil25": "25%", "ceil50": "50%",
    "ceil75": "75%", "ceil100": "100%", "ceil150": "150%"
  }

  for config in ["16vcpu", "8vcpu", "4vcpu", "2vcpu_1w"]:
    data = load_latency_data(config)
    info = load_system_info(config)

    w(f"### {CONFIG_LABELS[config]} "
      f"(TS ceiling: {info.get('ts_ceiling_actual', '?')}M)")
    w("")
    w("| Load | Srv | N | p50 (us) | +/-CI | p99 (us)"
      " | +/-CI | p999 (us) | max (us) |")
    w("|:-----|:---:|--:|---------:|------:|--------:"
      "|------:|----------:|---------:|")

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
          f"| {p50['mean']:.0f} | {p50['ci']:.0f} "
          f"| {p99['mean']:.0f} | {p99['ci']:.0f} "
          f"| {p999['mean']:.0f} "
          f"| {mx['max']:.0f} |")
    w("")

    # Ratio sub-table
    w(f"**Latency ratio (TS/HD, higher = HD wins):**")
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
        p50r = ts_p50 / hd_p50 if hd_p50 > 0 else 0
        p99r = ts_p99 / hd_p99 if hd_p99 > 0 else 0
        w(f"| {load_labels.get(load, load)} "
          f"| {p50r:.2f}x | {p99r:.2f}x |")
    w("")

  w("![Latency](plots/latency_all.png)")
  w("")
  w("![Latency Ratio](plots/latency_ratio.png)")
  w("")

  # --- 2 vCPU worker comparison ---
  w("## 2 vCPU: 1 Worker vs 2 Workers")
  w("")
  w("Supplemental test showing the effect of oversubscription.")
  w("")

  for wconfig, wlabel in [("2vcpu_1w", "1 Worker"),
                           ("2vcpu_2w", "2 Workers")]:
    data = load_rate_data(wconfig)
    rates = sorted(set(r for (s, r) in data.keys()))
    w(f"### {wlabel}")
    w("")
    w("| Rate | HD (Mbps) | CV% | HD Loss | TS (Mbps) | TS Loss |")
    w("|-----:|----------:|----:|--------:|----------:|--------:|")
    for rate in rates:
      hd_tp = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts_tp = stats(data.get(("ts", rate), {"tp": []})["tp"])
      hd_lo = stats(data.get(("hd", rate), {"loss": []})["loss"])
      ts_lo = stats(data.get(("ts", rate), {"loss": []})["loss"])
      if not hd_tp or not ts_tp:
        continue
      w(f"| {rate} | {hd_tp['mean']:.0f} | {hd_tp['cv']:.1f} "
        f"| {hd_lo['mean']:.2f}% "
        f"| {ts_tp['mean']:.0f} | {ts_lo['mean']:.2f}% |")
    w("")

  w("![2 vCPU Workers](plots/2vcpu_workers.png)")
  w("")

  # --- Key findings ---
  w("## Key Findings")
  w("")
  w("### 1. HD advantage grows as resources shrink")
  w("")
  w("| Config | Peak Throughput Ratio | p99 Latency Ratio (at TS ceiling) |")
  w("|--------|---------------------:|----------------------------------:|")

  for config in ["2vcpu_1w", "4vcpu", "8vcpu", "16vcpu"]:
    data = load_rate_data(config)
    lat_data = load_latency_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    best_hd, best_ts = 0, 0
    for rate in rates:
      hd_s = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts_s = stats(data.get(("ts", rate), {"tp": []})["tp"])
      if hd_s and hd_s["mean"] > best_hd:
        best_hd = hd_s["mean"]
      if ts_s and ts_s["mean"] > best_ts:
        best_ts = ts_s["mean"]
    tp_ratio = best_hd / best_ts if best_ts > 0 else 0

    ts_p99 = lat_data.get(("ts", "ceil100"), {}).get("p99", [])
    hd_p99 = lat_data.get(("hd", "ceil100"), {}).get("p99", [])
    if ts_p99 and hd_p99:
      lat_ratio = (sum(ts_p99) / len(ts_p99)) / (
        sum(hd_p99) / len(hd_p99))
    else:
      lat_ratio = 0

    w(f"| {CONFIG_LABELS[config]} "
      f"| {tp_ratio:.1f}x | {lat_ratio:.1f}x |")

  w("")
  w("### 2. Throughput")
  w("")
  w("- **2 vCPU**: HD delivers 6.4 Gbps lossless while TS")
  w("  collapses to 300 Mbps at 5G offered (93% loss)")
  w("- **4 vCPU**: HD 6.4x throughput advantage at 15G offered")
  w("- **8 vCPU**: HD 3.0x at 20G, lossless through 7.5G")
  w("- **16 vCPU**: HD advantage narrows to 1.3x (see below)")
  w("")
  w("### 3. Tail latency under load")
  w("")
  w("HD's p99 advantage grows monotonically with load at every")
  w("configuration. At the TS ceiling:")
  w("- 4 vCPU: **5.7x** better p99 (405us vs 2314us)")
  w("- 8 vCPU: **3.8x** better p99 (312us vs 1190us)")
  w("- 16 vCPU: **3.3x** better p99 (328us vs 1090us)")
  w("")
  w("### 4. 16 vCPU regression — worker count problem")
  w("")
  w("16 vCPU (8 workers) underperforms 8 vCPU (4 workers):")
  w("- HD peak at 16 vCPU: 11.8 Gbps")
  w("- HD peak at 8 vCPU: 14.8 Gbps")
  w("- At 20G offered: HD drops to 8.4 Gbps with 51% loss, worse")
  w("  than at 15G. Active throughput collapse, not saturation.")
  w("- CV of 50-63% at 20G/25G — bimodal distribution")
  w("")
  w("Hypothesis: cross-shard overhead scales as N^2 with worker")
  w("count. Planned follow-up: test 16 vCPU with 4 workers.")
  w("")
  w("### 5. 2 vCPU: 1 worker >> 2 workers")
  w("")
  w("Oversubscription hurts. 2 workers on 2 vCPU produces bimodal")
  w("throughput (runs alternate between ~4G and ~2G). 1 worker is")
  w("stable and outperforms: 6.4 Gbps vs ~4 Gbps at 7.5G.")
  w("")

  w("## Data Quality")
  w("")
  w("### Variance flags (CV > 10%)")
  w("")
  flagged = []
  for config in ["16vcpu", "8vcpu", "4vcpu", "2vcpu_1w"]:
    data = load_rate_data(config)
    rates = sorted(set(r for (s, r) in data.keys()))
    for rate in rates:
      for server in ["hd", "ts"]:
        key = (server, rate)
        if key in data:
          s = stats(data[key]["tp"])
          if s and s["cv"] > 10:
            flagged.append((config, server.upper(), rate, s["cv"]))

  if flagged:
    w("| Config | Server | Rate | CV% |")
    w("|--------|--------|-----:|----:|")
    for config, server, rate, cv in flagged:
      w(f"| {CONFIG_LABELS.get(config, config)} "
        f"| {server} | {rate} | {cv:.1f}% |")
  w("")

  w("### Methodology")
  w("")
  w("- 25 runs at high rates, 5 at low rates")
  w("- Latency: 10-15 runs per load level, 4500 samples per run")
  w("- Background loads scaled to % of TS ceiling per config")
  w("- Strict isolation: one server at a time, cache drops between")
  w("- Go derper: v1.96.1 release build (-trimpath, stripped)")
  w("- HD: SPSC xfer rings, batched eventfd, SPSC frame return")
  w("")

  return "\n".join(lines)


# --- Main ---

if __name__ == "__main__":
  print("Generating plots...")
  plot_throughput_all()
  plot_loss_all()
  plot_ratio_all()
  plot_latency_all()
  plot_latency_ratio()
  plot_cost_story()
  plot_2w_comparison()

  print("Generating report...")
  report = generate_report()

  report_path = os.path.join(BASE, "REPORT.md")
  with open(report_path, "w") as f:
    f.write(report)

  print(f"Report: {report_path}")
  print(f"Plots:  {PLOTS}/")
