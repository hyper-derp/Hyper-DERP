#!/usr/bin/env python3
"""Regenerate HD-bench plots in HD color scheme (dark + light).

Reads raw data from ~/dev/HD-bench/results/20260328/, outputs to
static/img/bench/ (dark) and static/img/bench/light/ (light).
"""

import glob
import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker  # noqa: F401

BENCH = os.path.expanduser(
  "~/dev/HD-bench/results/20260328"
)
SITE = os.path.dirname(os.path.abspath(__file__))
OUT_DARK = os.path.join(SITE, "static", "img", "bench")
OUT_LIGHT = os.path.join(SITE, "static", "img", "bench", "light")

# -- HD color scheme --
HD_COLOR = "#f91bff"
HD_OFF = "#c700cd"
HD_FILL = "#f91bff33"
TS_COLOR = "#ffe346"
TS_OFF = "#f4cf00"
TS_FILL = "#ffe34633"
LINK = "#1c8bee"
BG_COLOR = "#101010"
BG_LIGHT = "#1f1f1f"
TEXT_COLOR = "#f2f2f2"
TEXT_MUTED = "#828282"
GRID_COLOR = "#333333"

CURRENT_MODE = "dark"


def set_style(mode="dark"):
  """Configure matplotlib for dark or light mode."""
  global CURRENT_MODE, TS_COLOR, TS_FILL
  CURRENT_MODE = mode
  if mode == "dark":
    bg, bg2 = BG_COLOR, BG_LIGHT
    fg, fg2 = TEXT_COLOR, TEXT_MUTED
    grid, legend_bg = GRID_COLOR, BG_LIGHT
    TS_COLOR = "#ffe346"
    TS_FILL = "#ffe34633"
  else:
    bg, bg2 = "#fafafa", "#f2f2f2"
    fg, fg2 = "#313131", "#828282"
    grid, legend_bg = "#e6e6e6", "#f2f2f2"
    TS_COLOR = "#f4cf00"
    TS_FILL = "#f4cf0033"
  plt.rcParams.update({
    "figure.facecolor": bg,
    "axes.facecolor": bg2,
    "axes.edgecolor": grid,
    "axes.labelcolor": fg,
    "text.color": fg,
    "xtick.color": fg2,
    "ytick.color": fg2,
    "grid.color": grid,
    "grid.alpha": 0.5,
    "legend.facecolor": legend_bg,
    "legend.edgecolor": grid,
    "legend.labelcolor": fg,
    "font.family": "sans-serif",
    "font.size": 12,
    "figure.dpi": 150,
    "axes.titlesize": 14,
    "axes.titleweight": "bold",
    "axes.labelsize": 12,
  })


COLORS = {"hd": HD_COLOR, "ts": None}  # TS set dynamically
LABELS = {"hd": "Hyper-DERP (kTLS)", "ts": "Tailscale derper"}

CONFIG_COLORS = {
  "2vcpu_1w": HD_COLOR,
  "4vcpu_2w": "#de8c3b",
  "8vcpu_4w": "#3bde8c",
  "16vcpu_8w": "#3bdede",
}

CONFIGS = ["2vcpu_1w", "4vcpu_2w", "8vcpu_4w", "16vcpu_8w"]
CONFIG_LABELS = {
  "2vcpu_1w": "2 vCPU (1w)",
  "4vcpu_2w": "4 vCPU (2w)",
  "8vcpu_4w": "8 vCPU (4w)",
  "16vcpu_8w": "16 vCPU (8w)",
}
CONFIG_VCPUS = {
  "2vcpu_1w": 2,
  "4vcpu_2w": 4,
  "8vcpu_4w": 8,
  "16vcpu_8w": 16,
}

# -- Stats --

T_TABLE = {
  2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776,
  6: 2.571, 7: 2.447, 8: 2.365, 9: 2.306,
  10: 2.262, 15: 2.145, 20: 2.093, 25: 2.064,
  30: 2.045, 50: 2.009, 100: 1.984,
}


def t_crit(n):
  """Two-tailed 95% t critical value."""
  df = n - 1
  if df < 2:
    return 12.706
  if df in T_TABLE:
    return T_TABLE[df]
  if df > 100:
    return 1.96
  keys = sorted(T_TABLE.keys())
  for i in range(len(keys) - 1):
    if keys[i] <= df <= keys[i + 1]:
      f = (df - keys[i]) / (keys[i + 1] - keys[i])
      return (T_TABLE[keys[i]] * (1 - f)
              + T_TABLE[keys[i + 1]] * f)
  return 2.0


def stats(vals):
  """Descriptive statistics with 95% CI."""
  n = len(vals)
  if n == 0:
    return None
  mean = sum(vals) / n
  if n > 1:
    var = sum((x - mean) ** 2 for x in vals) / (n - 1)
    sd = math.sqrt(var)
    ci = t_crit(n) * sd / math.sqrt(n)
  else:
    sd = ci = 0
  return {"n": n, "mean": mean, "sd": sd, "ci": ci}


def load_rate_data(config):
  """Load rate sweep JSON for a config."""
  config_dir = os.path.join(BENCH, config)
  data = {}
  for f in glob.glob(os.path.join(config_dir, "agg_*.json")):
    try:
      with open(f) as fh:
        d = json.load(fh)
    except (json.JSONDecodeError, OSError):
      continue
    basename = os.path.basename(f).replace(".json", "")
    parts = basename.split("_")
    if len(parts) < 4 or parts[0] != "agg":
      continue
    server = parts[1]
    try:
      rate = int(parts[2])
    except ValueError:
      continue
    key = (server, rate)
    if key not in data:
      data[key] = {"tp": [], "loss": []}
    data[key]["tp"].append(d.get("throughput_mbps", 0))
    loss = d.get("message_loss_pct", 0)
    data[key]["loss"].append(loss if loss else 0)
  return data


def _save(fig, name, out_dir):
  """Save figure to out_dir."""
  path = os.path.join(out_dir, name)
  fig.savefig(path, dpi=150, bbox_inches="tight")
  plt.close(fig)
  print(f"  {CURRENT_MODE}/{name}", file=sys.stderr)


def plot_throughput_all(out):
  """2x2 subplots: throughput vs offered rate."""
  fig, axes = plt.subplots(2, 2, figsize=(14, 10))
  axes = axes.flatten()
  for idx, config in enumerate(CONFIGS):
    ax = axes[idx]
    data = load_rate_data(config)
    rates = sorted(set(r for (_, r) in data.keys()))
    for server in ["ts", "hd"]:
      c = HD_COLOR if server == "hd" else TS_COLOR
      x, y, err = [], [], []
      for rate in rates:
        key = (server, rate)
        if key not in data:
          continue
        s = stats(data[key]["tp"])
        if s is None:
          continue
        x.append(rate / 1000)
        y.append(s["mean"])
        err.append(s["ci"])
      ax.errorbar(
        x, y, yerr=err, marker="o", markersize=4,
        color=c, label=LABELS[server],
        capsize=3, linewidth=1.5,
      )
    max_rate = max(rates) / 1000 if rates else 10
    ax.plot(
      [0, max_rate], [0, max_rate * 1000], ":",
      color=TEXT_MUTED, alpha=0.5, label="Wire rate",
    )
    ax.set_title(CONFIG_LABELS[config])
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Delivered Throughput (Mbps)")
    ax.legend(fontsize=8)
    ax.grid(True)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
  fig.suptitle(
    "Throughput vs Offered Rate (n=20 per point)",
    fontsize=14, fontweight="bold",
  )
  plt.tight_layout()
  _save(fig, "throughput_all.png", out)


def plot_loss_all(out):
  """2x2 subplots: message loss %."""
  fig, axes = plt.subplots(2, 2, figsize=(14, 10))
  axes = axes.flatten()
  for idx, config in enumerate(CONFIGS):
    ax = axes[idx]
    data = load_rate_data(config)
    rates = sorted(set(r for (_, r) in data.keys()))
    for server in ["ts", "hd"]:
      c = HD_COLOR if server == "hd" else TS_COLOR
      x, y, err = [], [], []
      for rate in rates:
        key = (server, rate)
        if key not in data:
          continue
        s = stats(data[key]["loss"])
        if s is None:
          continue
        x.append(rate / 1000)
        y.append(s["mean"])
        err.append(s["ci"])
      ax.errorbar(
        x, y, yerr=err, marker="o", markersize=4,
        color=c, label=LABELS[server],
        capsize=3, linewidth=1.5,
      )
    ax.set_title(CONFIG_LABELS[config])
    ax.set_xlabel("Offered Rate (Gbps)")
    ax.set_ylabel("Message Loss (%)")
    ax.legend(fontsize=8)
    ax.grid(True)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=-2, top=100)
  fig.suptitle(
    "Message Loss vs Offered Rate (n=20 per point)",
    fontsize=14, fontweight="bold",
  )
  plt.tight_layout()
  _save(fig, "loss_all.png", out)


def plot_ratio_all(out):
  """Grouped bar: HD/TS throughput ratio."""
  fig, ax = plt.subplots(figsize=(12, 6))
  bar_width = 0.18
  all_rates = set()
  ratios = {}
  for config in CONFIGS:
    data = load_rate_data(config)
    rates = sorted(set(r for (_, r) in data.keys()))
    for rate in rates:
      hd_s = stats(data.get(("hd", rate), {"tp": []})["tp"])
      ts_s = stats(data.get(("ts", rate), {"tp": []})["tp"])
      if hd_s and ts_s and ts_s["mean"] > 0:
        all_rates.add(rate)
        if config not in ratios:
          ratios[config] = {}
        ratios[config][rate] = hd_s["mean"] / ts_s["mean"]
  show_rates = sorted(r for r in all_rates if r >= 3000)
  if not show_rates:
    plt.close(fig)
    return
  x = range(len(show_rates))
  for i, config in enumerate(CONFIGS):
    vals = [ratios.get(config, {}).get(r, 0)
            for r in show_rates]
    offset = (i - len(CONFIGS) / 2 + 0.5) * bar_width
    bars = ax.bar(
      [xi + offset for xi in x], vals, bar_width,
      label=CONFIG_LABELS[config],
      color=CONFIG_COLORS[config], alpha=0.85,
    )
    for bar, v in zip(bars, vals):
      if v > 0:
        ax.text(
          bar.get_x() + bar.get_width() / 2,
          bar.get_height(),
          f"{v:.1f}x", ha="center", va="bottom",
          fontsize=7, fontweight="bold",
        )
  ax.axhline(y=1.0, color=TEXT_MUTED, linestyle="--",
             alpha=0.5)
  ax.set_xticks(list(x))
  ax.set_xticklabels(
    [f"{r / 1000:.0f}G" for r in show_rates])
  ax.set_xlabel("Offered Rate")
  ax.set_ylabel("HD / TS Throughput Ratio")
  ax.set_title(
    "Hyper-DERP Advantage by vCPU Config (>= 3 Gbps)")
  ax.legend()
  ax.grid(True, axis="y")
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  _save(fig, "ratio_all.png", out)


def _get_peaks():
  """Peak throughput per (config, server)."""
  peaks = {}
  for config in CONFIGS:
    data = load_rate_data(config)
    best = {"hd": 0, "ts": 0}
    for (server, _), vals in data.items():
      s = stats(vals["tp"])
      if s and s["mean"] > best.get(server, 0):
        best[server] = s["mean"]
    peaks[config] = best
  return peaks


def plot_peak_scaling(out):
  """Peak throughput vs vCPU count."""
  peaks = _get_peaks()
  vcpus = [CONFIG_VCPUS[c] for c in CONFIGS]
  hd_peaks = [peaks[c]["hd"] for c in CONFIGS]
  ts_peaks = [peaks[c]["ts"] for c in CONFIGS]
  fig, ax = plt.subplots(figsize=(10, 6))
  ax.plot(
    vcpus, hd_peaks, marker="o", markersize=6,
    color=HD_COLOR, label=LABELS["hd"], linewidth=2,
  )
  ax.plot(
    vcpus, ts_peaks, marker="o", markersize=6,
    color=TS_COLOR, label=LABELS["ts"], linewidth=2,
  )
  if hd_peaks[0] > 0:
    base = hd_peaks[0]
    ref = [base * (v / 2) for v in vcpus]
    ax.plot(
      vcpus, ref, ":", color=TEXT_MUTED, alpha=0.5,
      label="Linear scaling (from 2 vCPU HD)",
    )
  for v, hp, tp in zip(vcpus, hd_peaks, ts_peaks):
    ax.annotate(
      f"{hp:.0f}", (v, hp), textcoords="offset points",
      xytext=(0, 8), ha="center", fontsize=8,
      color=HD_COLOR,
    )
    ax.annotate(
      f"{tp:.0f}", (v, tp), textcoords="offset points",
      xytext=(0, -14), ha="center", fontsize=8,
      color=TS_COLOR,
    )
  ax.set_xlabel("Relay vCPU Count")
  ax.set_ylabel("Peak Throughput (Mbps)")
  ax.set_title("Peak Throughput Scaling by vCPU Count")
  ax.set_xticks(vcpus)
  ax.legend()
  ax.grid(True)
  ax.set_xlim(left=0)
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  _save(fig, "peak_scaling.png", out)


def plot_cost_story(out):
  """HD at N vCPU vs TS at 2N vCPU."""
  peaks = _get_peaks()
  pairs = [
    ("2vcpu_1w", "4vcpu_2w"),
    ("4vcpu_2w", "8vcpu_4w"),
    ("8vcpu_4w", "16vcpu_8w"),
  ]
  pair_labels, hd_vals, ts_vals = [], [], []
  hd_vcpus, ts_vcpus = [], []
  for hd_cfg, ts_cfg in pairs:
    hd_v = CONFIG_VCPUS[hd_cfg]
    ts_v = CONFIG_VCPUS[ts_cfg]
    hd_vcpus.append(hd_v)
    ts_vcpus.append(ts_v)
    pair_labels.append(f"{hd_v} vCPU vs {ts_v} vCPU")
    hd_vals.append(peaks[hd_cfg]["hd"])
    ts_vals.append(peaks[ts_cfg]["ts"])
  fig, ax = plt.subplots(figsize=(10, 6))
  x = range(len(pairs))
  w = 0.3
  ax.bar(
    [xi - w / 2 for xi in x], hd_vals, w,
    label="Hyper-DERP (kTLS)", color=HD_COLOR, alpha=0.85,
  )
  ax.bar(
    [xi + w / 2 for xi in x], ts_vals, w,
    label="Tailscale derper",
    color=TS_COLOR, alpha=0.85,
  )
  for i in x:
    ax.text(
      i - w / 2, hd_vals[i] / 2,
      f"{hd_vcpus[i]} vCPU",
      ha="center", va="center",
      fontsize=16, color=TEXT_COLOR, fontweight="bold",
    )
    ax.text(
      i - w / 2, hd_vals[i] + 30,
      f"{hd_vals[i]:.0f}",
      ha="center", va="bottom",
      fontsize=9, color=HD_COLOR, fontweight="bold",
    )
    ax.text(
      i + w / 2, ts_vals[i] / 2,
      f"{ts_vcpus[i]} vCPU",
      ha="center", va="center",
      fontsize=16, color=TEXT_COLOR, fontweight="bold",
    )
    ax.text(
      i + w / 2, ts_vals[i] + 30,
      f"{ts_vals[i]:.0f}",
      ha="center", va="bottom",
      fontsize=9, color=TS_COLOR, fontweight="bold",
    )
    if ts_vals[i] > 0:
      ratio = hd_vals[i] / ts_vals[i]
      y_top = max(hd_vals[i], ts_vals[i])
      ax.annotate(
        f"{ratio:.2f}x", (i, y_top + 350),
        ha="center", fontsize=11, fontweight="bold",
        color=LINK,
      )
  ax.set_xticks(list(x))
  ax.set_xticklabels(pair_labels, fontsize=10)
  ax.set_ylabel("Peak Throughput (Mbps)")
  ax.set_title(
    "Same Throughput, Half the Cores")
  ax.legend(fontsize=10)
  ax.grid(True)
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  _save(fig, "cost_story.png", out)


LATENCY_DIR = os.path.expanduser(
  "~/dev/HD-bench/results/20260403/latency/latency"
)
PEER_DIR = os.path.join(BENCH, "peer_sweep")
TUNNEL_DIR = os.path.expanduser(
  "~/dev/HD-bench/results/20260403/tunnel_v2"
)

LOAD_LEVELS = ["idle", "25pct", "50pct", "75pct", "100pct", "150pct"]
LOAD_LABELS = {
  "idle": "Idle", "25pct": "25%", "50pct": "50%",
  "75pct": "75%", "100pct": "100%", "150pct": "150%",
}


def _load_latency(config, server, level):
  """Load latency run data for a config/server/level."""
  d = os.path.join(LATENCY_DIR, config)
  prefix = f"lat_{server}_{level}_r"
  p99s, p50s = [], []
  for f in sorted(glob.glob(os.path.join(d, prefix + "*.json"))):
    try:
      with open(f) as fh:
        data = json.load(fh)
    except (json.JSONDecodeError, OSError):
      continue
    lat = data.get("latency_ns", {})
    p50s.append(lat.get("p50", 0) / 1000)
    p99s.append(lat.get("p99", 0) / 1000)
  return {"p50": stats(p50s), "p99": stats(p99s)}


def plot_latency(out):
  """Latency vs load level for 8v and 16v (p50 and p99)."""
  fig, axes = plt.subplots(1, 2, figsize=(14, 6))
  configs = [("8vcpu", "8 vCPU"), ("16vcpu", "16 vCPU")]
  for idx, (config, title) in enumerate(configs):
    ax = axes[idx]
    for server, color, label in [
      ("hd", HD_COLOR, "HD"),
      ("ts", TS_COLOR, "TS"),
    ]:
      x_pos = list(range(len(LOAD_LEVELS)))
      for pctl, ls, marker in [
        ("p50", "-", "o"),
        ("p99", "--", "s"),
      ]:
        y, err = [], []
        valid_x = []
        for i, level in enumerate(LOAD_LEVELS):
          d = _load_latency(config, server, level)
          s = d[pctl]
          if s and s["n"] > 0:
            y.append(s["mean"])
            err.append(s["ci"])
            valid_x.append(i)
        if y:
          ax.errorbar(
            valid_x, y, yerr=err, marker=marker,
            markersize=4, color=color, linestyle=ls,
            label=f"{label} {pctl}", capsize=3,
            linewidth=1.5,
          )
    ax.set_title(title)
    ax.set_xticks(list(range(len(LOAD_LEVELS))))
    ax.set_xticklabels(
      [LOAD_LABELS[l] for l in LOAD_LEVELS], fontsize=9)
    ax.set_xlabel("Background Load (% of TS ceiling)")
    ax.set_ylabel("Latency (us)")
    ax.legend(fontsize=8)
    ax.grid(True)
    ax.set_ylim(bottom=0)
  fig.suptitle(
    "Relay Latency vs Load Level (n=10, 4500 samples/run)",
    fontsize=14, fontweight="bold",
  )
  plt.tight_layout()
  _save(fig, "latency_load.png", out)


def plot_peer_scaling(out):
  """Throughput at different peer counts (8 vCPU, 10G)."""
  peer_counts = [20, 40, 60, 80, 100]
  config_map = {
    20: "8vcpu_4w",
    40: "8vcpu_4w_40p",
    60: "8vcpu_4w_60p",
    80: "8vcpu_4w_80p",
    100: "8vcpu_4w_100p",
  }
  fig, ax = plt.subplots(figsize=(10, 6))
  for server, color, label in [
    ("hd", HD_COLOR, LABELS["hd"]),
    ("ts", TS_COLOR, LABELS["ts"]),
  ]:
    x, y, err = [], [], []
    for peers in peer_counts:
      cfg = config_map[peers]
      if peers == 20:
        cfg_dir = os.path.join(BENCH, cfg)
      else:
        cfg_dir = os.path.join(PEER_DIR, cfg)
      pattern = os.path.join(cfg_dir, f"agg_{server}_10000_r*.json")
      tps = []
      for f in glob.glob(pattern):
        try:
          with open(f) as fh:
            d = json.load(fh)
          tps.append(d.get("throughput_mbps", 0))
        except (json.JSONDecodeError, OSError):
          continue
      s = stats(tps)
      if s and s["n"] > 0:
        x.append(peers)
        y.append(s["mean"])
        err.append(s["ci"])
    if y:
      ax.errorbar(
        x, y, yerr=err, marker="o", markersize=6,
        color=color, label=label, capsize=3, linewidth=2,
      )
      for xi, yi in zip(x, y):
        ax.annotate(
          f"{yi:.0f}", (xi, yi),
          textcoords="offset points", xytext=(0, 10),
          ha="center", fontsize=8, color=color,
        )
  ax.set_xlabel("Peer Count")
  ax.set_ylabel("Throughput (Mbps)")
  ax.set_title(
    "Peer Scaling: 8 vCPU at 10 Gbps Offered (n=10-20)")
  ax.set_xticks(peer_counts)
  ax.legend()
  ax.grid(True)
  ax.set_xlim(left=10)
  ax.set_ylim(bottom=0)
  plt.tight_layout()
  _save(fig, "peer_scaling.png", out)


ALL_PLOTS = [
  plot_throughput_all,
  plot_loss_all,
  plot_ratio_all,
  plot_peak_scaling,
  plot_cost_story,
  plot_latency,
  plot_peer_scaling,
]


def main():
  for d in [OUT_DARK, OUT_LIGHT]:
    os.makedirs(d, exist_ok=True)
  for mode, out in [("dark", OUT_DARK), ("light", OUT_LIGHT)]:
    print(f"Generating {mode} plots...", file=sys.stderr)
    set_style(mode)
    for fn in ALL_PLOTS:
      fn(out)
  print("Done.", file=sys.stderr)


if __name__ == "__main__":
  main()
