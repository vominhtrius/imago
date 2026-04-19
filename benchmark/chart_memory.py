#!/usr/bin/env python3
"""Plot imago RSS over time across bench runs to surface memory leaks.

Reads timestamped docker-stats samples written by `make _k6` (one .stats file
per benchmark run) plus the imago shutdown log saved by `make _down` (which
contains the jemalloc stats dump emitted on clean exit). Produces:

    results/charts/memory_per_run.png           one subplot per run + slope fit
    results/charts/memory_timeline.png          combined timeline (real wall-clock)
    results/charts/memory_timeline_stitched.png runs concatenated back-to-back
                                                (idle gaps removed, one continuous
                                                line — easier to eyeball drift)

Stats file lines look like:
    1734567890,42.3%,512.4MiB / 4GiB

A positive linear-fit slope on RSS over time during steady-state load is the
classic leak signature; this script flags any run with slope > 1 MB/min.

Usage:
    python3 chart_memory.py --results results --out results/charts
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

LEAK_THRESHOLD_MB_PER_MIN = 1.0

STATS_RE = re.compile(
    r"^(?P<tool>imago|imgproxy)_(?P<method>\w+?)_(?P<format>\w+?)"
    r"_(?P<w>\d+)x(?P<h>\d+)_(?P<ts>\d+_\d+)\.stats$"
)


@dataclass(frozen=True)
class Run:
    method: str
    format: str
    size: str
    ts: str
    df: pd.DataFrame

    @property
    def label(self) -> str:
        return f"{self.method}_{self.format}_{self.size}_{self.ts}"


def parse_mem_mb(s: str) -> float | None:
    s = s.strip()
    try:
        if s.endswith("GiB"):
            return float(s[:-3]) * 1024.0
        if s.endswith("MiB"):
            return float(s[:-3])
        if s.endswith("KiB"):
            return float(s[:-3]) / 1024.0
        if s.endswith("GB"):
            return float(s[:-2]) * 1000.0
        if s.endswith("MB"):
            return float(s[:-2])
        if s.endswith("kB"):
            return float(s[:-2]) / 1000.0
    except ValueError:
        return None
    return None


def load_stats(path: Path) -> pd.DataFrame:
    rows: list[dict[str, float]] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(",", 2)
        if len(parts) < 3:
            continue
        try:
            ts = int(parts[0])
            cpu = float(parts[1].rstrip("%"))
        except ValueError:
            continue
        mem = parse_mem_mb(parts[2].split("/")[0])
        if mem is None:
            continue
        rows.append({"ts": ts, "cpu": cpu, "mem_mb": mem})
    return pd.DataFrame(rows)


def fit_slope(df: pd.DataFrame) -> tuple[float, float]:
    if len(df) < 2:
        intercept = float(df["mem_mb"].iloc[0]) if len(df) else 0.0
        return 0.0, intercept
    x = df["ts"].astype(float).to_numpy()
    y = df["mem_mb"].to_numpy()
    slope, intercept = np.polyfit(x - x[0], y, 1)
    return float(slope), float(intercept)


def collect_runs(results_dir: Path, tool: str) -> list[Run]:
    runs: list[Run] = []
    for path in sorted(results_dir.glob(f"{tool}_*.stats")):
        m = STATS_RE.match(path.name)
        if not m:
            continue
        df = load_stats(path)
        if df.empty:
            continue
        runs.append(
            Run(
                method=m["method"],
                format=m["format"],
                size=f"{m['w']}x{m['h']}",
                ts=m["ts"],
                df=df,
            )
        )
    return runs


def plot_per_run(runs: list[Run], out_path: Path, tool: str) -> None:
    n = len(runs)
    cols = min(2, n)
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(7 * cols, 3.5 * rows), squeeze=False)

    for i, run in enumerate(runs):
        ax = axes[i // cols][i % cols]
        df = run.df
        slope_per_sec, intercept = fit_slope(df)
        slope_per_min = slope_per_sec * 60.0
        t0 = int(df["ts"].min())
        t = (df["ts"] - t0).to_numpy()
        started_at = datetime.fromtimestamp(t0).strftime("%Y-%m-%d %H:%M:%S")

        ax.plot(t, df["mem_mb"], color="#2563EB", linewidth=1.5, label="RSS")
        ax.plot(
            t,
            intercept + slope_per_sec * t,
            color="#DC2626",
            linewidth=1.0,
            linestyle="--",
            label=f"fit: {slope_per_min:+.2f} MB/min",
        )

        title = f"{run.method} {run.format} {run.size}  [started {started_at}]"
        if slope_per_min > LEAK_THRESHOLD_MB_PER_MIN:
            title += "  ⚠ LEAK SUSPECT"
            ax.set_title(title, color="#DC2626", fontsize=10, fontweight="bold")
        else:
            ax.set_title(title, fontsize=10, fontweight="bold")

        ax.set_xlabel("seconds")
        ax.set_ylabel("RSS (MiB)")
        ax.grid(True, linestyle="--", alpha=0.4)
        ax.legend(fontsize=8, framealpha=0.8)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    for j in range(n, rows * cols):
        axes[j // cols][j % cols].axis("off")

    fig.suptitle(
        f"{tool} — RSS over time per benchmark", fontsize=13, fontweight="bold", y=1.0
    )
    fig.tight_layout()
    fig.savefig(out_path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {out_path}")


def plot_timeline(runs: list[Run], out_path: Path, tool: str) -> None:
    frames = []
    for run in runs:
        d = run.df.copy()
        d["label"] = run.label
        frames.append(d)
    combined = pd.concat(frames, ignore_index=True).sort_values("ts")
    t0 = int(combined["ts"].min())
    combined["t"] = combined["ts"] - t0

    overall_slope, _ = fit_slope(combined.rename(columns={"ts": "ts"}))
    start_mb = float(combined["mem_mb"].iloc[0])
    end_mb = float(combined["mem_mb"].iloc[-1])

    fig, ax = plt.subplots(figsize=(12, 5))
    cmap = plt.get_cmap("tab10")
    for i, (label, sub) in enumerate(combined.groupby("label", sort=False)):
        ax.plot(
            sub["t"],
            sub["mem_mb"],
            color=cmap(i % 10),
            linewidth=1.5,
            alpha=0.9,
            label=label,
        )
        # Vertical separator at the start of each run.
        ax.axvline(sub["t"].iloc[0], color="#9CA3AF", linewidth=0.4, linestyle=":")

    ax.axhline(
        start_mb,
        color="#9CA3AF",
        linewidth=0.8,
        linestyle=":",
        label=f"start: {start_mb:.0f} MiB",
    )

    started_at = datetime.fromtimestamp(t0).strftime("%Y-%m-%d %H:%M:%S")
    title = (
        f"{tool} — RSS across all runs  [started {started_at}]\n"
        f"start={start_mb:.0f} MiB, end={end_mb:.0f} MiB, "
        f"slope={overall_slope * 60:+.2f} MB/min"
    )
    if overall_slope * 60 > LEAK_THRESHOLD_MB_PER_MIN:
        title += "  ⚠ LEAK SUSPECT"
    ax.set_xlabel("seconds since first sample")
    ax.set_ylabel("RSS (MiB)")
    ax.set_title(title, fontweight="bold")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(fontsize=8, ncol=2, framealpha=0.8, loc="upper left")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {out_path}")


def plot_timeline_stitched(runs: list[Run], out_path: Path, tool: str) -> None:
    """Concatenate runs back-to-back on a synthetic x-axis so idle gaps between
    benchmark sessions disappear. Makes the drift/leak signal easier to read
    when a session has multiple benchmark rounds separated by imgproxy runs.
    """
    sorted_runs = sorted(runs, key=lambda r: r.df["ts"].min())
    t0 = int(sorted_runs[0].df["ts"].min())

    fig, ax = plt.subplots(figsize=(12, 5))
    cmap = plt.get_cmap("tab10")

    stitched_frames: list[pd.DataFrame] = []
    cursor = 0.0
    for i, run in enumerate(sorted_runs):
        d = run.df.copy().sort_values("ts").reset_index(drop=True)
        run_start = float(d["ts"].iloc[0])
        d["t_stitched"] = cursor + (d["ts"] - run_start)
        stitched_frames.append(d.assign(label=run.label))

        ax.plot(
            d["t_stitched"],
            d["mem_mb"],
            color=cmap(i % 10),
            linewidth=1.5,
            alpha=0.9,
            label=run.label,
        )
        ax.axvline(cursor, color="#9CA3AF", linewidth=0.4, linestyle=":")
        cursor = float(d["t_stitched"].iloc[-1]) + 1.0

    stitched = pd.concat(stitched_frames, ignore_index=True)
    x = stitched["t_stitched"].to_numpy()
    y = stitched["mem_mb"].to_numpy()
    slope_per_sec, intercept = np.polyfit(x, y, 1)
    slope_per_min = slope_per_sec * 60.0
    ax.plot(
        x,
        intercept + slope_per_sec * x,
        color="#DC2626",
        linewidth=1.0,
        linestyle="--",
        label=f"fit: {slope_per_min:+.2f} MB/min",
    )

    start_mb = float(stitched["mem_mb"].iloc[0])
    end_mb = float(stitched["mem_mb"].iloc[-1])
    started_at = datetime.fromtimestamp(t0).strftime("%Y-%m-%d %H:%M:%S")
    title = (
        f"{tool} — RSS across all runs (idle gaps removed)  "
        f"[first sample {started_at}]\n"
        f"start={start_mb:.0f} MiB, end={end_mb:.0f} MiB, "
        f"slope={slope_per_min:+.2f} MB/min across "
        f"{len(sorted_runs)} runs ({cursor / 60:.0f} active min)"
    )
    if slope_per_min > LEAK_THRESHOLD_MB_PER_MIN:
        title += "  ⚠ LEAK SUSPECT"
    ax.set_xlabel("seconds of active benchmark time (gaps collapsed)")
    ax.set_ylabel("RSS (MiB)")
    ax.set_title(title, fontweight="bold")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(fontsize=8, ncol=2, framealpha=0.8, loc="upper left")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved {out_path}")


def print_verdict(runs: list[Run]) -> None:
    print("\n=== Per-run leak analysis ===")
    print(
        f"{'method':<10}{'format':<8}{'size':<10}"
        f"{'start MiB':>10}{'end MiB':>10}{'slope MB/min':>15}  verdict"
    )
    for run in runs:
        slope_per_sec, _ = fit_slope(run.df)
        slope_per_min = slope_per_sec * 60.0
        start_mb = float(run.df["mem_mb"].iloc[0])
        end_mb = float(run.df["mem_mb"].iloc[-1])
        verdict = (
            "leak suspect" if slope_per_min > LEAK_THRESHOLD_MB_PER_MIN else "ok"
        )
        print(
            f"{run.method:<10}{run.format:<8}{run.size:<10}"
            f"{start_mb:>10.1f}{end_mb:>10.1f}{slope_per_min:>15.2f}  {verdict}"
        )


def summarize_jemalloc(results_dir: Path) -> None:
    """Pull the per-arena allocated/active/resident/retained line out of the
    most recent imago shutdown log so the operator gets a quick verdict
    without having to grep the full dump."""
    logs = sorted(results_dir.glob("imago_*.log"))
    if not logs:
        return
    latest = logs[-1]
    text = latest.read_text(errors="replace")
    if "jemalloc statistics" not in text:
        print(f"\n(no jemalloc dump found in {latest.name})")
        return
    print(f"\n=== jemalloc shutdown stats ({latest.name}) ===")
    interesting = [
        "Allocated:",
        "active:",
        "metadata:",
        "resident:",
        "mapped:",
        "retained:",
    ]
    for line in text.splitlines():
        for needle in interesting:
            if needle in line:
                print("  " + line.strip())
                break


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot imago RSS over time")
    parser.add_argument("--results", default="results")
    parser.add_argument("--out", default="results/charts")
    parser.add_argument("--tool", default="imago")
    args = parser.parse_args()

    results_dir = Path(args.results)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    runs = collect_runs(results_dir, args.tool)
    if not runs:
        print(
            f"No {args.tool}_*.stats files found in {results_dir}. "
            "Run a benchmark first.",
            file=sys.stderr,
        )
        sys.exit(1)

    plot_per_run(runs, out_dir / "memory_per_run.png", args.tool)
    plot_timeline(runs, out_dir / "memory_timeline.png", args.tool)
    plot_timeline_stitched(
        runs, out_dir / "memory_timeline_stitched.png", args.tool
    )
    print_verdict(runs)
    summarize_jemalloc(results_dir)


if __name__ == "__main__":
    main()
