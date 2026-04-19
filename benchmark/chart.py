#!/usr/bin/env python3
"""
Generate benchmark charts from results/summary.csv.

Usage:
    pip install pandas matplotlib
    python3 benchmark/chart.py                        # reads results/summary.csv
    python3 benchmark/chart.py --csv path/to/file.csv
    python3 benchmark/chart.py --out charts/          # output directory
"""

import argparse
import sys
from pathlib import Path

import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ── palette ──────────────────────────────────────────────────────────────────
COLORS = {'imago': '#2563EB', 'imgproxy': '#DC2626'}
LABEL  = {'imago': 'imago (C++)', 'imgproxy': 'imgproxy (Go)'}

# ── helpers ───────────────────────────────────────────────────────────────────

FAILURE_THRESHOLD = 0.5  # drop runs where ≥50% of requests returned non-200


def load(csv_path: Path, version: str | None = None) -> pd.DataFrame:
    df = pd.read_csv(csv_path, on_bad_lines='warn')
    df.columns = df.columns.str.strip()
    for col in ('peak_cpu_pct', 'avg_cpu_pct', 'peak_mem_mb'):
        if col not in df.columns:
            df[col] = 0.0
    df['scenario'] = df['method'] + '\n' + df['format'] + ' ' + df['width'].astype(str) + 'x' + df['height'].astype(str)

    if 'timestamp' in df.columns:
        df = df.sort_values('timestamp')

    if {'failures', 'total_reqs'}.issubset(df.columns):
        fail_ratio = df['failures'] / df['total_reqs'].where(df['total_reqs'] > 0, 1)
        bad = df[fail_ratio >= FAILURE_THRESHOLD]
        if not bad.empty:
            print(
                f'WARNING: dropping {len(bad)} run(s) with ≥{FAILURE_THRESHOLD:.0%} '
                f'non-200 responses (see summary.csv for details):',
                file=sys.stderr,
            )
            for _, r in bad.iterrows():
                ts = r.get('timestamp', '?')
                ver = r.get('version', '?')
                print(
                    f'  - {ts}  {ver:>8}  {r["tool"]:>8}  {r["method"]}/{r["format"]} '
                    f'{r["width"]}x{r["height"]}: {int(r["failures"])}/{int(r["total_reqs"])} failed',
                    file=sys.stderr,
                )
            df = df[fail_ratio < FAILURE_THRESHOLD]
        df['_partial_fail'] = (df['failures'] > 0) & (df['failures'] < df['total_reqs'])
    else:
        df['_partial_fail'] = False

    if 'version' in df.columns:
        if version:
            df = df[df['version'] == version]
            if df.empty:
                raise SystemExit(
                    f'ERROR: version "{version}" has no healthy runs (all dropped due to failures).'
                )
        # keep latest run per (tool, scenario) so each bar has exactly one value
        df = df.drop_duplicates(subset=['tool', 'scenario'], keep='last')

    if df.empty:
        raise SystemExit('ERROR: no healthy benchmark rows to chart.')

    return df


def bar_group(ax, df: pd.DataFrame, metric: str, ylabel: str, title: str):
    scenarios = df['scenario'].unique()
    tools     = df['tool'].unique()
    n, m      = len(scenarios), len(tools)
    width     = 0.35
    x         = range(n)

    for i, tool in enumerate(tools):
        vals = [
            df[(df['scenario'] == s) & (df['tool'] == tool)][metric].values
            for s in scenarios
        ]
        heights = [v[0] if len(v) else 0 for v in vals]
        offset  = (i - (m - 1) / 2) * width
        bars    = ax.bar(
            [xi + offset for xi in x], heights,
            width=width,
            color=COLORS.get(tool, '#6B7280'),
            label=LABEL.get(tool, tool),
            zorder=3,
        )
        partials = [
            bool(df[(df['scenario'] == s) & (df['tool'] == tool)]['_partial_fail'].any())
            if '_partial_fail' in df.columns else False
            for s in scenarios
        ]
        for bar, h, partial in zip(bars, heights, partials):
            if h > 0:
                label = f'{h:.0f}' if metric == 'rps' else f'{h:.1f}'
                if partial:
                    label = '⚠ ' + label
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height() * 1.012,
                    label,
                    ha='center', va='bottom', fontsize=8, fontweight='bold',
                    color=COLORS.get(tool, '#6B7280'),
                )

    ax.set_xticks(list(x))
    ax.set_xticklabels(scenarios, fontsize=9)
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontweight='bold', pad=10)
    ax.legend(loc='upper right', framealpha=0.8)
    ax.yaxis.grid(True, linestyle='--', alpha=0.5, zorder=0)
    ax.set_axisbelow(True)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)


def speedup_chart(ax, df: pd.DataFrame):
    scenarios = df['scenario'].unique()
    speedups  = []
    for s in scenarios:
        imago    = df[(df['scenario'] == s) & (df['tool'] == 'imago')]['rps'].values
        imgproxy = df[(df['scenario'] == s) & (df['tool'] == 'imgproxy')]['rps'].values
        if len(imago) and len(imgproxy) and imgproxy[0] > 0:
            speedups.append(imago[0] / imgproxy[0])
        else:
            speedups.append(0)

    bars = ax.bar(range(len(scenarios)), speedups, color='#2563EB', zorder=3)
    ax.axhline(1.0, color='#DC2626', linewidth=1.5, linestyle='--', label='imgproxy baseline')

    for bar, v in zip(bars, speedups):
        if v > 0:
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.02,
                f'{v:.2f}×',
                ha='center', va='bottom', fontsize=9, fontweight='bold', color='#2563EB',
            )

    ax.set_xticks(range(len(scenarios)))
    ax.set_xticklabels(scenarios, fontsize=9)
    ax.set_ylabel('Throughput ratio (imago / imgproxy)')
    ax.legend(framealpha=0.8)
    ax.yaxis.grid(True, linestyle='--', alpha=0.5, zorder=0)
    ax.set_axisbelow(True)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)


def latency_percentile_chart(ax, df: pd.DataFrame):
    scenarios = df['scenario'].unique()
    for tool in df['tool'].unique():
        sub  = df[df['tool'] == tool]
        avgs = [sub[sub['scenario'] == s]['avg_ms'].mean() for s in scenarios]
        p90s = [sub[sub['scenario'] == s]['p90_ms'].mean() for s in scenarios]
        p95s = [sub[sub['scenario'] == s]['p95_ms'].mean() for s in scenarios]
        color = COLORS.get(tool, '#6B7280')
        label = LABEL.get(tool, tool)
        x = range(len(scenarios))
        ax.plot(x, avgs, 'o-',  color=color, label=f'{label} avg',  linewidth=2)
        ax.plot(x, p90s, 's--', color=color, label=f'{label} p90',  linewidth=1.5, alpha=0.7)
        ax.plot(x, p95s, '^:',  color=color, label=f'{label} p95',  linewidth=1.5, alpha=0.5)

    ax.set_xticks(range(len(scenarios)))
    ax.set_xticklabels(scenarios, fontsize=9)
    ax.set_ylabel('Latency (ms)')
    ax.legend(loc='upper left', fontsize=8, framealpha=0.8, ncol=2)
    ax.yaxis.grid(True, linestyle='--', alpha=0.5, zorder=0)
    ax.set_axisbelow(True)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)


def resource_chart(axes, df: pd.DataFrame):
    """Two side-by-side bar charts: peak CPU % and peak RAM MB per scenario/tool."""
    has_data = df['peak_cpu_pct'].gt(0).any() or df['peak_mem_mb'].gt(0).any()
    if not has_data:
        for ax in axes:
            ax.text(0.5, 0.5, 'No resource data\n(run benchmarks with docker stats enabled)',
                    ha='center', va='center', transform=ax.transAxes,
                    fontsize=11, color='#6B7280')
            ax.set_axis_off()
        return

    bar_group(axes[0], df, 'peak_cpu_pct', 'CPU %',    'Peak CPU usage (lower is better)')
    bar_group(axes[1], df, 'peak_mem_mb',  'Memory MB', 'Peak memory usage (lower is better)')


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description='Generate benchmark charts')
    parser.add_argument('--csv',     default='results/summary.csv')
    parser.add_argument('--out',     default='results/charts')
    parser.add_argument('--version', default=None, help='Filter to a specific version (e.g. abc1234)')
    args = parser.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        print(f'ERROR: {csv_path} not found. Run benchmarks first.', file=sys.stderr)
        sys.exit(1)

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    df = load(csv_path, version=args.version)
    print(f'Loaded {len(df)} rows from {csv_path}')
    print(df[['tool', 'method', 'format', 'width', 'height', 'rps', 'avg_ms', 'p90_ms', 'p95_ms']].to_string(index=False))

    versions = df['version'].unique() if 'version' in df.columns else []
    ver_str  = f"  (version: {', '.join(str(v) for v in versions)})" if len(versions) else ''
    ver_slug = f"_{args.version}" if args.version else ''

    started_at = ''
    if 'timestamp' in df.columns and not df['timestamp'].isna().all():
        ts_min = pd.to_datetime(df['timestamp'], errors='coerce').min()
        if pd.notna(ts_min):
            started_at = f"  [started {ts_min.strftime('%Y-%m-%d %H:%M:%S')}]"
    ver_str += started_at

    # ── figure 1: throughput + latency side by side ───────────────────────────
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle(f'imago vs imgproxy — Benchmark Results{ver_str}', fontsize=14, fontweight='bold', y=1.02)

    bar_group(axes[0], df, 'rps',    'Requests / second', 'Throughput (higher is better)')
    bar_group(axes[1], df, 'avg_ms', 'Latency (ms)',       'Avg latency (lower is better)')

    fig.tight_layout()
    out1 = out_dir / f'throughput_latency{ver_slug}.png'
    fig.savefig(out1, dpi=220, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved {out1}')

    # ── figure 2: speedup ratio ───────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(max(6, len(df['scenario'].unique()) * 2), 5))
    ax.set_title(f'imago speedup over imgproxy{ver_str}', fontweight='bold', pad=10)
    speedup_chart(ax, df)
    fig.tight_layout()
    out2 = out_dir / f'speedup{ver_slug}.png'
    fig.savefig(out2, dpi=220, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved {out2}')

    # ── figure 3: latency percentiles ─────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(max(8, len(df['scenario'].unique()) * 2), 5))
    ax.set_title(f'Latency percentiles{ver_str}', fontweight='bold', pad=10)
    latency_percentile_chart(ax, df)
    fig.tight_layout()
    out3 = out_dir / f'latency_percentiles{ver_slug}.png'
    fig.savefig(out3, dpi=220, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved {out3}')

    # ── figure 4: CPU and memory usage ────────────────────────────────────────
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle(f'imago vs imgproxy — Resource Usage{ver_str}', fontsize=14, fontweight='bold', y=1.02)
    resource_chart(axes, df)
    fig.tight_layout()
    out4 = out_dir / f'resource_usage{ver_slug}.png'
    fig.savefig(out4, dpi=220, bbox_inches='tight')
    plt.close(fig)
    print(f'Saved {out4}')

    print(f'\nAll charts written to {out_dir}/')


if __name__ == '__main__':
    main()
