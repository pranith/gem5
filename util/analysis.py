#!/usr/bin/env python3

"""
Lightweight results summarizer for SPEC2017 runs.

Example:
  python util/analysis.py --dirs spec2017_baseline spec2017_mergebuffer \
    --stat board.processor.cores.core.ipc \
    --stat board.processor.cores.core.iew.lsqFullEvents
"""

import argparse
import os
import re
from pathlib import Path
from typing import (
    Dict,
    List,
    Optional,
    Tuple,
)

BENCHMARK_MAPPING = {
    "500.perlbench_r": "perlbench_r",
    "502.gcc_r": "cpugcc_r",
    "505.mcf_r": "mcf_r",
    "520.omnetpp_r": "omnetpp_r",
    "523.xalancbmk_r": "cpuxalan_r",
    "525.x264_r": "x264_r",
    "531.deepsjeng_r": "deepsjeng_r",
    "541.leela_r": "leela_r",
    "548.exchange2_r": "exchange2_r",
    "557.xz_r": "xz_r",
}
REV_BENCHMARK_MAPPING = {v: k for k, v in BENCHMARK_MAPPING.items()}


def load_weights(path: Path) -> Dict[int, float]:
    weights: Dict[int, float] = {}
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            weight, idx = line.split()
            weights[int(idx)] = float(weight)
    return weights


def parse_stats(path: Path) -> Dict[str, float]:
    stats: Dict[str, float] = {}
    line_re = re.compile(r"^\s*([\w\.\:\-]+)\s+([-+\deE\.]+)")
    with open(path) as f:
        for line in f:
            m = line_re.match(line)
            if not m:
                continue
            try:
                stats[m.group(1)] = float(m.group(2))
            except ValueError:
                # Ignore non-numeric entries.
                continue
    return stats


def accumulate_stats(
    base_dir: Path, stat_keys: List[str]
) -> Dict[str, Dict[str, float]]:
    """
    Returns: {stat: {benchmark: weighted_value}}
    """
    accum: Dict[str, Dict[str, float]] = {stat: {} for stat in stat_keys}

    for sub in os.listdir(base_dir):
        if not sub.startswith("chkpt_"):
            continue
        m = re.search(r"chkpt_([\w\.]+_r)_(\d+)", sub)
        if not m:
            continue
        bench_name = m.group(1)
        bench_num = REV_BENCHMARK_MAPPING.get(bench_name)
        if not bench_num:
            continue
        idx = int(m.group(2))

        weights_path = base_dir / f"{bench_num}.weights"
        if not weights_path.exists():
            continue
        weights = load_weights(weights_path)
        weight = weights.get(idx, 0.0)

        stats_path = base_dir / sub / "stats.txt"
        if not stats_path.exists():
            continue
        stats = parse_stats(stats_path)

        for stat in stat_keys:
            if stat not in stats:
                continue
            accum[stat][bench_name] = (
                accum[stat].get(bench_name, 0.0) + stats[stat] * weight
            )

    return accum


def collect_stat_names(base_dirs: List[Path]) -> List[str]:
    """Collect a union of all stat names across the provided result dirs."""
    names = set()
    for base_dir in base_dirs:
        for sub in os.listdir(base_dir):
            if not sub.startswith("chkpt_"):
                continue
            stats_path = base_dir / sub / "stats.txt"
            if not stats_path.exists():
                continue
            names.update(parse_stats(stats_path).keys())
    return sorted(names)


def percent_diff(
    base: Optional[float], other: Optional[float]
) -> Optional[float]:
    if base is None or other is None or base == 0:
        return None
    return (other / base - 1.0) * 100.0


def format_float(value: Optional[float]) -> str:
    if value is None:
        return "-"
    return f"{value:.3f}"


def geometric_mean(values: List[float]) -> Optional[float]:
    vals = [v for v in values if v is not None and v > 0]
    if not vals:
        return None
    from math import (
        pow,
        prod,
    )

    return pow(prod(vals), 1.0 / len(vals))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Summarize weighted stats across result dirs."
    )
    parser.add_argument(
        "--dirs",
        nargs="+",
        required=True,
        help="Result directories to compare.",
    )
    parser.add_argument(
        "--stat",
        action="append",
        required=True,
        dest="stats",
        help="Stat name or regex to summarize (can be passed multiple times).",
    )
    args = parser.parse_args()

    base_dirs = [Path(d) for d in args.dirs]
    for d in base_dirs:
        if not d.exists():
            raise FileNotFoundError(f"Directory not found: {d}")

    # Expand stat patterns to concrete stat names.
    available_stats = collect_stat_names(base_dirs)
    expanded_stats: List[str] = []
    seen = set()
    for pattern in args.stats:
        regex = re.compile(pattern)
        matches = [s for s in available_stats if regex.search(s)]
        if not matches:
            print(f"Warning: no stats matched pattern '{pattern}'")
        for m in matches:
            if m not in seen:
                seen.add(m)
                expanded_stats.append(m)

    if not expanded_stats:
        raise SystemExit("No stats matched requested patterns.")

    # Collect per-dir data.
    summaries = {
        str(d): accumulate_stats(d, expanded_stats) for d in base_dirs
    }

    # Print summary table.
    # Keep benchmark order by spec number for readability.
    benches = [
        BENCHMARK_MAPPING[k]
        for k in sorted(
            BENCHMARK_MAPPING.keys(), key=lambda x: int(x.split(".")[0])
        )
    ]
    dir_headers = args.dirs
    pct_headers = (
        [f"% {d}" for d in args.dirs[1:]] if len(args.dirs) > 1 else []
    )

    # Compute fixed column widths across all stats for consistency.
    widths: List[int] = [len("stat"), len("benchmark")]
    widths.extend(len(h) for h in dir_headers)
    widths.extend(len(h) for h in pct_headers)

    for stat in expanded_stats:
        widths[0] = max(widths[0], len(stat))
        # Consider extra rows for width calculation.
        extra_rows = ["geomean"]
        if "ipc" in stat.lower():
            extra_rows.append("spec_score")
        for bench in benches + extra_rows:
            widths[1] = max(widths[1], len(bench))
            row_vals: List[str] = []
            values: List[Optional[float]] = []
            for d in dir_headers:
                if bench in benches:
                    val = summaries[str(Path(d))][stat].get(bench)
                elif bench == "geomean":
                    val = geometric_mean(
                        [summaries[str(Path(d))][stat].get(b) for b in benches]
                    )
                elif bench == "spec_score":
                    vals = [
                        summaries[str(Path(d))][stat].get(b) for b in benches
                    ]
                    val = sum(v for v in vals if v is not None)
                else:
                    val = None
                values.append(val)
                row_vals.append(format_float(val))
            if len(dir_headers) > 1:
                base_val = values[0]
                for val in values[1:]:
                    row_vals.append(format_float(percent_diff(base_val, val)))
            for i, cell in enumerate(row_vals, start=2):
                widths[i] = max(widths[i], len(cell))

    for stat in expanded_stats:
        headers = ["stat", "benchmark"] + dir_headers + pct_headers
        # Header
        header_row = " | ".join(
            h.ljust(widths[i]) for i, h in enumerate(headers)
        )
        sep = "-+-".join("-" * widths[i] for i in range(len(headers)))
        print(header_row)
        print(sep)
        # Rows
        for bench in benches:
            row_cells: List[str] = [stat, bench]
            values: List[Optional[float]] = []
            for d in dir_headers:
                val = summaries[str(Path(d))][stat].get(bench)
                values.append(val)
                row_cells.append(format_float(val))
            if len(dir_headers) > 1:
                base_val = values[0]
                for val in values[1:]:
                    row_cells.append(format_float(percent_diff(base_val, val)))
            row = " | ".join(
                cell.ljust(widths[i]) for i, cell in enumerate(row_cells)
            )
            print(row)
        # Geometric mean row
        g_row: List[str] = [stat, "geomean"]
        g_values: List[Optional[float]] = []
        for d in dir_headers:
            gval = geometric_mean(
                [summaries[str(Path(d))][stat].get(b) for b in benches]
            )
            g_values.append(gval)
            g_row.append(format_float(gval))
        if len(dir_headers) > 1:
            base_val = g_values[0]
            for val in g_values[1:]:
                g_row.append(format_float(percent_diff(base_val, val)))
        print(
            " | ".join(cell.ljust(widths[i]) for i, cell in enumerate(g_row))
        )
        # SPEC score row (sum of per-benchmark values) for IPC-like stats.
        if "ipc" in stat.lower():
            s_row: List[str] = [stat, "spec_score"]
            s_values: List[Optional[float]] = []
            for d in dir_headers:
                vals = [summaries[str(Path(d))][stat].get(b) for b in benches]
                sval = sum(v for v in vals if v is not None)
                s_values.append(sval)
                s_row.append(format_float(sval))
            if len(dir_headers) > 1:
                base_val = s_values[0]
                for val in s_values[1:]:
                    s_row.append(format_float(percent_diff(base_val, val)))
            print(
                " | ".join(
                    cell.ljust(widths[i]) for i, cell in enumerate(s_row)
                )
            )
        print()  # blank line between stats


if __name__ == "__main__":
    main()
