#!/usr/bin/env python3
"""Parse benchmark JSON results and produce tables / plots.

Usage:
  # Single result summary:
  python3 plot_results.py result.json

  # Compare multiple runs:
  python3 plot_results.py run1.json run2.json ...

  # Latency CDF from raw samples:
  python3 plot_results.py --cdf ping_result.json

  # Save plots instead of showing:
  python3 plot_results.py --save-dir ./plots result.json
"""

import argparse
import json
import os
import sys
from pathlib import Path


def load_results(paths):
    results = []
    for p in paths:
        with open(p) as f:
            data = json.load(f)
            data["_file"] = os.path.basename(p)
            results.append(data)
    return results


def print_summary_table(results):
    """Print a text table summarizing all results."""
    hdr = (
        f"{'Label':<20} {'Mode':<6} {'Pkts':>8} "
        f"{'Size':>6} {'Elapsed':>10} {'PPS':>12} "
        f"{'Mbps':>10}"
    )
    sep = "-" * len(hdr)
    print(sep)
    print(hdr)
    print(sep)

    for r in results:
        label = r.get("test", r.get("_file", "?"))[:20]
        mode = r.get("mode", "?")[:6]
        pkts = r.get("packets", 0)
        size = r.get("config", {}).get("packet_size", 0)
        elapsed = f"{r.get('elapsed_ms', 0):.1f} ms"
        pps = f"{r.get('throughput_pps', 0):.0f}"
        mbps = f"{r.get('throughput_mbps', 0):.3f}"
        print(
            f"{label:<20} {mode:<6} {pkts:>8} "
            f"{size:>6} {elapsed:>10} {pps:>12} "
            f"{mbps:>10}"
        )

    print(sep)


def print_latency_table(results):
    """Print latency percentile table for results that
    have latency data."""
    lat_results = [
        r for r in results if "latency_ns" in r
    ]
    if not lat_results:
        return

    print()
    hdr = (
        f"{'Label':<20} {'Samples':>8} "
        f"{'Min':>10} {'P50':>10} {'P90':>10} "
        f"{'P99':>10} {'P999':>10} {'Max':>10}"
    )
    sep = "-" * len(hdr)
    print(sep)
    print(hdr)
    print(sep)

    for r in lat_results:
        label = r.get("test", r.get("_file", "?"))[:20]
        lat = r["latency_ns"]
        ns = lambda k: f"{lat.get(k, 0) / 1000:.1f} us"
        print(
            f"{label:<20} {lat.get('samples', 0):>8} "
            f"{ns('min'):>10} {ns('p50'):>10} "
            f"{ns('p90'):>10} {ns('p99'):>10} "
            f"{ns('p999'):>10} {ns('max'):>10}"
        )

    print(sep)


def plot_throughput_bar(results, save_dir=None):
    """Bar chart comparing throughput across runs."""
    import matplotlib.pyplot as plt

    labels = [
        r.get("test", r.get("_file", "?"))
        for r in results
    ]
    pps = [r.get("throughput_pps", 0) for r in results]
    mbps = [r.get("throughput_mbps", 0) for r in results]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    ax1.barh(labels, pps, color="#4a90d9")
    ax1.set_xlabel("Packets/sec")
    ax1.set_title("Throughput (packets/sec)")
    ax1.ticklabel_format(axis="x", style="scientific",
                         scilimits=(0, 0))

    ax2.barh(labels, mbps, color="#e8724a")
    ax2.set_xlabel("Mbps")
    ax2.set_title("Throughput (Mbps)")

    plt.tight_layout()
    if save_dir:
        plt.savefig(
            os.path.join(save_dir, "throughput.png"),
            dpi=150,
        )
        print(f"Saved {save_dir}/throughput.png")
    else:
        plt.show()


def plot_latency_percentiles(results, save_dir=None):
    """Bar chart of latency percentiles."""
    import matplotlib.pyplot as plt

    lat_results = [
        r for r in results if "latency_ns" in r
    ]
    if not lat_results:
        return

    fig, ax = plt.subplots(figsize=(10, 6))
    pcts = ["min", "p50", "p90", "p99", "p999", "max"]
    x_pos = range(len(pcts))
    width = 0.8 / max(len(lat_results), 1)

    for i, r in enumerate(lat_results):
        label = r.get("test", r.get("_file", "?"))
        lat = r["latency_ns"]
        vals = [lat.get(p, 0) / 1000 for p in pcts]
        offsets = [x + i * width for x in x_pos]
        ax.bar(offsets, vals, width, label=label)

    ax.set_xticks(
        [x + width * (len(lat_results) - 1) / 2
         for x in x_pos]
    )
    ax.set_xticklabels(pcts)
    ax.set_ylabel("Latency (us)")
    ax.set_title("Latency Percentiles")
    ax.legend()

    plt.tight_layout()
    if save_dir:
        plt.savefig(
            os.path.join(save_dir, "latency_pct.png"),
            dpi=150,
        )
        print(f"Saved {save_dir}/latency_pct.png")
    else:
        plt.show()


def plot_latency_cdf(results, save_dir=None):
    """CDF plot from raw latency samples."""
    import matplotlib.pyplot as plt
    import numpy as np

    raw_results = [
        r for r in results
        if "latency_ns" in r
        and "raw" in r.get("latency_ns", {})
    ]
    if not raw_results:
        print("No raw latency samples found. "
              "Use --raw-latency flag when running "
              "benchmarks.")
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    for r in raw_results:
        label = r.get("test", r.get("_file", "?"))
        raw = np.array(r["latency_ns"]["raw"]) / 1000
        sorted_vals = np.sort(raw)
        cdf = np.arange(1, len(sorted_vals) + 1) / len(
            sorted_vals
        )
        ax.plot(sorted_vals, cdf, label=label)

    ax.set_xlabel("Latency (us)")
    ax.set_ylabel("CDF")
    ax.set_title("Latency CDF")
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    if save_dir:
        plt.savefig(
            os.path.join(save_dir, "latency_cdf.png"),
            dpi=150,
        )
        print(f"Saved {save_dir}/latency_cdf.png")
    else:
        plt.show()


def plot_latency_histogram(results, save_dir=None):
    """Histogram of raw latency samples."""
    import matplotlib.pyplot as plt
    import numpy as np

    raw_results = [
        r for r in results
        if "latency_ns" in r
        and "raw" in r.get("latency_ns", {})
    ]
    if not raw_results:
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    for r in raw_results:
        label = r.get("test", r.get("_file", "?"))
        raw = np.array(r["latency_ns"]["raw"]) / 1000
        ax.hist(raw, bins=50, alpha=0.7, label=label)

    ax.set_xlabel("Latency (us)")
    ax.set_ylabel("Count")
    ax.set_title("Latency Distribution")
    ax.legend()

    plt.tight_layout()
    if save_dir:
        plt.savefig(
            os.path.join(save_dir, "latency_hist.png"),
            dpi=150,
        )
        print(f"Saved {save_dir}/latency_hist.png")
    else:
        plt.show()


def export_csv(results, output_path):
    """Export results as CSV for spreadsheet import."""
    import csv

    fields = [
        "test", "mode", "timestamp",
        "packet_count", "packet_size", "num_workers",
        "elapsed_ms", "packets", "bytes",
        "throughput_pps", "throughput_mbps",
        "lat_min_ns", "lat_p50_ns", "lat_p90_ns",
        "lat_p99_ns", "lat_p999_ns", "lat_max_ns",
        "lat_mean_ns",
    ]

    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()

        for r in results:
            lat = r.get("latency_ns", {})
            cfg = r.get("config", {})
            row = {
                "test": r.get("test", ""),
                "mode": r.get("mode", ""),
                "timestamp": r.get("timestamp", ""),
                "packet_count": cfg.get(
                    "packet_count", 0),
                "packet_size": cfg.get(
                    "packet_size", 0),
                "num_workers": cfg.get(
                    "num_workers", 0),
                "elapsed_ms": r.get("elapsed_ms", 0),
                "packets": r.get("packets", 0),
                "bytes": r.get("bytes", 0),
                "throughput_pps": r.get(
                    "throughput_pps", 0),
                "throughput_mbps": r.get(
                    "throughput_mbps", 0),
                "lat_min_ns": lat.get("min", ""),
                "lat_p50_ns": lat.get("p50", ""),
                "lat_p90_ns": lat.get("p90", ""),
                "lat_p99_ns": lat.get("p99", ""),
                "lat_p999_ns": lat.get("p999", ""),
                "lat_max_ns": lat.get("max", ""),
                "lat_mean_ns": lat.get("mean", ""),
            }
            writer.writerow(row)

    print(f"Exported {len(results)} results to "
          f"{output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Hyper-DERP benchmark result "
        "analysis")
    parser.add_argument(
        "files", nargs="+",
        help="JSON result files")
    parser.add_argument(
        "--cdf", action="store_true",
        help="Plot latency CDF")
    parser.add_argument(
        "--hist", action="store_true",
        help="Plot latency histogram")
    parser.add_argument(
        "--throughput", action="store_true",
        help="Plot throughput comparison")
    parser.add_argument(
        "--latency", action="store_true",
        help="Plot latency percentile bars")
    parser.add_argument(
        "--all-plots", action="store_true",
        help="Generate all available plots")
    parser.add_argument(
        "--csv", metavar="FILE",
        help="Export results to CSV")
    parser.add_argument(
        "--save-dir", metavar="DIR",
        help="Save plots to directory")
    parser.add_argument(
        "--no-table", action="store_true",
        help="Skip text table output")

    args = parser.parse_args()

    results = load_results(args.files)

    if not args.no_table:
        print_summary_table(results)
        print_latency_table(results)

    if args.csv:
        export_csv(results, args.csv)

    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)

    do_plots = (
        args.cdf or args.hist or args.throughput
        or args.latency or args.all_plots
    )

    if do_plots or args.save_dir:
        if args.throughput or args.all_plots:
            plot_throughput_bar(results, args.save_dir)
        if args.latency or args.all_plots:
            plot_latency_percentiles(
                results, args.save_dir)
        if args.cdf or args.all_plots:
            plot_latency_cdf(results, args.save_dir)
        if args.hist or args.all_plots:
            plot_latency_histogram(
                results, args.save_dir)


if __name__ == "__main__":
    main()
