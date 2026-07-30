#!/usr/bin/env python3
"""Render the benchmark comparison chart used in the README.

Numbers are the best of 8 `bench` runs (best-case strips OS-scheduler noise from
a latency micro-benchmark). Edit the two dicts below after re-running `bench` on
a new machine, then regenerate:

    python scripts/plot_benchmark.py   # -> docs/benchmark.png
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

# Best-of-8 results (2,000,000-op workload, 1024-tick band).
NAIVE = {"p50": 538.2, "p99": 2796.5, "p99.9": 31396.9, "throughput": 3.22}
FAST = {"p50": 343.9, "p99": 2317.7, "p99.9": 20821.3, "throughput": 4.02}

GRID = "#d8dee9"
NAIVE_C = "#8894a8"  # muted slate = the baseline
FAST_C = "#1eb980"  # green = the faster build
TEXT = "#2b3440"


def ns_label(v):
    return f"{v/1000:.1f} µs" if v >= 1000 else f"{v:.0f} ns"


def main() -> int:
    fig, (ax_lat, ax_tp) = plt.subplots(1, 2, figsize=(10, 4.2), width_ratios=[2, 1])
    fig.suptitle(
        "Order-book performance: naive std::map vs cache-friendly ladder",
        fontsize=13, fontweight="bold", color=TEXT, x=0.5, y=0.98,
    )

    # --- Latency percentiles (log scale: p50 and p99.9 differ ~60x) ----------
    pcts = ["p50", "p99", "p99.9"]
    x = range(len(pcts))
    w = 0.38
    naive_vals = [NAIVE[p] for p in pcts]
    fast_vals = [FAST[p] for p in pcts]

    b1 = ax_lat.bar([i - w / 2 for i in x], naive_vals, w, label="naive (map)", color=NAIVE_C)
    b2 = ax_lat.bar([i + w / 2 for i in x], fast_vals, w, label="fast (ladder)", color=FAST_C)
    ax_lat.set_yscale("log")
    ax_lat.set_ylabel("latency per order (log scale)", color=TEXT, fontsize=10)
    ax_lat.set_title("Per-order latency — lower is better", color=TEXT, fontsize=11, pad=8)
    ax_lat.set_xticks(list(x))
    ax_lat.set_xticklabels(pcts)
    ax_lat.yaxis.set_major_formatter(FuncFormatter(lambda v, _: ns_label(v)))
    for bars, vals in ((b1, naive_vals), (b2, fast_vals)):
        for rect, v in zip(bars, vals):
            ax_lat.text(rect.get_x() + rect.get_width() / 2, v * 1.06, ns_label(v),
                        ha="center", va="bottom", fontsize=8, color=TEXT)
    ax_lat.legend(frameon=False, fontsize=9, loc="upper left")

    # --- Throughput ----------------------------------------------------------
    tb = ax_tp.bar(["naive", "fast"], [NAIVE["throughput"], FAST["throughput"]],
                   color=[NAIVE_C, FAST_C], width=0.6)
    ax_tp.set_title("Throughput — higher is better", color=TEXT, fontsize=11, pad=8)
    ax_tp.set_ylabel("million ops / sec", color=TEXT, fontsize=10)
    for rect, v in zip(tb, [NAIVE["throughput"], FAST["throughput"]]):
        ax_tp.text(rect.get_x() + rect.get_width() / 2, v + 0.05, f"{v:.1f}M",
                   ha="center", va="bottom", fontsize=9, color=TEXT)
    ax_tp.set_ylim(0, max(NAIVE["throughput"], FAST["throughput"]) * 1.18)

    for ax in (ax_lat, ax_tp):
        ax.grid(axis="y", color=GRID, linewidth=0.7, alpha=0.7)
        ax.set_axisbelow(True)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
        for spine in ("left", "bottom"):
            ax.spines[spine].set_color(GRID)
        ax.tick_params(colors=TEXT, labelsize=9)

    fig.text(0.5, 0.005, "best of 8 runs · 2,000,000-op workload · 1024-tick band",
             ha="center", fontsize=8, color="#8894a8")
    fig.tight_layout(rect=(0, 0.02, 1, 0.96))

    out = Path(__file__).resolve().parent.parent / "docs" / "benchmark.png"
    out.parent.mkdir(exist_ok=True)
    fig.savefig(out, dpi=150, facecolor="white")
    print(f"Wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
