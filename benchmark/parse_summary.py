#!/usr/bin/env python3
"""
Parse a k6 text summary file and append one CSV row to results/summary.csv.

Usage (called by Makefile):
    python3 parse_summary.py <txt_file> <tool> <method> <format> <width> <height> <vus> <version> [stats_file]
"""
import re
import sys
from datetime import datetime
from pathlib import Path


def find(pattern: str, text: str, group: int = 1, default: str = '0') -> str:
    m = re.search(pattern, text)
    return m.group(group) if m else default


def find_ms(pattern_prefix: str, text: str) -> str:
    """Extract a k6 duration value and always return it in ms.
    k6 prints 'avg=48.41ms' for short durations and 'avg=20.73s' for long ones."""
    m = re.search(pattern_prefix + r'([\d.]+)(ms|s)', text)
    if not m:
        return '0'
    value, unit = float(m.group(1)), m.group(2)
    return f'{value * 1000:.2f}' if unit == 's' else f'{value:.2f}'


def parse_mem_mb(s: str) -> float:
    """Parse docker stats MemUsage strings like '123.4MiB', '1.2GiB', '512kB' to MB."""
    s = s.strip()
    if s.endswith('GiB'):
        return float(s[:-3]) * 1024
    if s.endswith('MiB'):
        return float(s[:-3])
    if s.endswith('kB') or s.endswith('KiB'):
        return float(s.rstrip('BiKk')) / 1024
    if s.endswith('GB'):
        return float(s[:-2]) * 1024
    if s.endswith('MB'):
        return float(s[:-2])
    return float(s)


def parse_stats(stats_path: Path) -> tuple[str, str, str]:
    """Return (peak_cpu_pct, avg_cpu_pct, peak_mem_mb) from a docker stats poll file."""
    if not stats_path.exists():
        return '0', '0', '0'

    cpu_values: list[float] = []
    mem_values: list[float] = []

    for line in stats_path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(',', 2)
        if not parts:
            continue
        # Two formats are supported:
        #   legacy: "cpu%,mem / limit"
        #   new:    "unix_ts,cpu%,mem / limit"
        if parts[0].endswith('%'):
            cpu_field = parts[0]
            mem_field = parts[1] if len(parts) > 1 else ''
        else:
            if len(parts) < 3:
                continue
            cpu_field = parts[1]
            mem_field = parts[2]
        try:
            cpu_values.append(float(cpu_field.rstrip('%')))
        except ValueError:
            pass
        try:
            mem_values.append(parse_mem_mb(mem_field.split('/')[0]))
        except ValueError:
            pass

    if not cpu_values:
        return '0', '0', '0'

    peak_cpu = f'{max(cpu_values):.1f}'
    avg_cpu  = f'{sum(cpu_values) / len(cpu_values):.1f}'
    peak_mem = f'{max(mem_values):.1f}' if mem_values else '0'
    return peak_cpu, avg_cpu, peak_mem


def main() -> None:
    if len(sys.argv) < 9:
        print(f'Usage: {sys.argv[0]} <txt> <tool> <method> <format> <width> <height> <vus> <version> [stats]')
        sys.exit(1)

    txt_path   = Path(sys.argv[1])
    tool, method, fmt, width, height, vus, version = sys.argv[2:9]
    stats_path = Path(sys.argv[9]) if len(sys.argv) > 9 else None

    if not txt_path.exists():
        print(f'ERROR: {txt_path} not found', file=sys.stderr)
        sys.exit(1)

    text = txt_path.read_text()

    rps   = find(r'http_reqs[.:\s]+(\d+)\s+([\d.]+)/s',         text, group=2)
    total = find(r'http_reqs[.:\s]+(\d+)\s+[\d.]+/s',           text, group=1)
    avg   = find_ms(r'http_req_duration[.:\s]+avg=',             text)
    p50   = find_ms(r'med=',                                     text)
    p90   = find_ms(r'p\(90\)=',                                 text)
    p95   = find_ms(r'p\(95\)=',                                 text)
    fails = find(r'checks_failed[.:\s]+[\d.]+%\s+(\d+) out of', text)

    peak_cpu, avg_cpu, peak_mem = parse_stats(stats_path) if stats_path else ('0', '0', '0')

    timestamp = datetime.now().strftime('%Y-%m-%dT%H:%M:%S')

    summary_csv = txt_path.parent / 'summary.csv'
    header = ('version,timestamp,tool,method,format,width,height,vus,'
              'rps,avg_ms,p50_ms,p90_ms,p95_ms,total_reqs,failures,'
              'peak_cpu_pct,avg_cpu_pct,peak_mem_mb')
    row    = (f'{version},{timestamp},{tool},{method},{fmt},{width},{height},{vus},'
              f'{rps},{avg},{p50},{p90},{p95},{total},{fails},'
              f'{peak_cpu},{avg_cpu},{peak_mem}')

    if not summary_csv.exists():
        summary_csv.write_text(header + '\n')

    with summary_csv.open('a') as f:
        f.write(row + '\n')

    print(f'Appended to {summary_csv}: {row}')


if __name__ == '__main__':
    main()
