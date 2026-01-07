#!/bin/env python3

import argparse
import json
import numbers
import os
import re
import sys
from pathlib import Path

import pandas as pd

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
BENCHMARK_ORDER = [
    BENCHMARK_MAPPING[k]
    for k in sorted(
        BENCHMARK_MAPPING.keys(), key=lambda x: int(x.split(".")[0])
    )
]


def parse_stats_file(filepath):
    """Parses a stats.txt file and returns a dictionary of key-value pairs."""
    stats = {}
    with open(filepath) as f:
        for line in f:
            # Improved regex to only capture key-value pairs
            match = re.match(
                r"^\s*([\w\.\:]+)\s+([\d\.\-]+|nan)\s*(?:#.*)?$", line
            )
            if match:
                key = match.group(1)
                value = match.group(2)
                try:
                    stats[key] = float(value)
                except ValueError:
                    stats[key] = value
    return stats


def read_weights_file(weights_file_path):
    """Reads weights from a file with the specified format."""
    weights = {}
    try:
        with open(weights_file_path) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) == 2:
                    try:
                        weight = float(parts[0])
                        index = int(parts[1])
                        weights[index] = weight
                    except ValueError:
                        print(
                            f"Warning: Invalid weight format in line: {line.strip()} in {weights_file_path}"
                        )
                elif line.strip() != "":
                    print(
                        f"Warning: Unexpected line format in weights file {weights_file_path}: {line.strip()}"
                    )
    except FileNotFoundError:
        print(f"Error: Weights file not found: {weights_file_path}")
        return None
    return weights


def create_stats_dataframe(base_dir):
    """Creates DataFrame and adds weights with correct mapping."""
    data = []
    labels = []
    for subdir in os.listdir(base_dir):
        subdir_path = os.path.join(base_dir, subdir)
        if os.path.isdir(subdir_path):
            stats_file = os.path.join(subdir_path, "stats.txt")
            try:
                match = re.search(r"chkpt_([\w\.]+_r)_(\d+)", subdir)
                if match:
                    benchmark_name = match.group(1)
                    index = int(match.group(2))
                    benchmark_number = next(
                        (
                            k
                            for k, v in BENCHMARK_MAPPING.items()
                            if v == benchmark_name
                        ),
                        None,
                    )
                    if benchmark_number:
                        weights_file = os.path.join(
                            base_dir, f"{benchmark_number}.weights"
                        )
                        weights = read_weights_file(weights_file)
                        if weights is None:
                            continue  # Skip if weights file not found
                    else:
                        print(
                            f"Warning: No benchmark number found for {benchmark_name}"
                        )
                        continue

                benchmark_name = BENCHMARK_MAPPING[benchmark_number]
                weights_file = os.path.join(
                    base_dir, f"{benchmark_number}.weights"
                )

                weights = read_weights_file(weights_file)
                if weights is None:
                    continue  # skip this subdir if weights file is not found
            except (IndexError, KeyError):
                print(
                    f"Warning: Could not determine benchmark name for {subdir}"
                )
                continue

            if os.path.exists(stats_file):
                stats = parse_stats_file(stats_file)
                try:
                    index = int(subdir.split("_r_")[-1])
                except ValueError:
                    print(
                        f"Warning: Invalid index in subdirectory name: {subdir}"
                    )
                    continue

                try:
                    stats["IPC"] = stats["board.processor.cores.core.ipc"]
                except:
                    print(f"Error reading IPC from {stats_file}")
                    # sys.exit(-1)

                if weights and index in weights:
                    stats["weight"] = weights[index]
                    stats["weighted_IPC"] = stats["IPC"] * stats["weight"]
                else:
                    print(
                        f"Warning: No weights or index {index} not found in weights file for {benchmark_name}"
                    )
                    stats["weight"] = "N/A"
                    stats["weighted_IPC"] = "N/A"
                data.append(stats)
                labels.append(subdir)
            else:
                print(f"Warning: stats.txt not found in {subdir}")

    if not data:
        print(
            f"Error: No stats.txt files found in subdirectories of '{base_dir}'."
        )
        return None

    df = pd.DataFrame(data, index=labels)

    # Calculate per-benchmark IPC
    benchmark_ipc = {}
    # Iterate benchmarks in numeric order of their SPEC IDs.
    for benchmark_number in sorted(
        BENCHMARK_MAPPING.keys(), key=lambda x: int(x.split(".")[0])
    ):
        benchmark_name = BENCHMARK_MAPPING[benchmark_number]
        benchmark_entries = df[
            df.index.str.contains(f"chkpt_{benchmark_name}_")
        ]
        if not benchmark_entries.empty:
            weighted_ipcs = benchmark_entries[
                benchmark_entries["weighted_IPC"] != "N/A"
            ]["weighted_IPC"].astype(float)
            if not weighted_ipcs.empty:
                benchmark_ipc[benchmark_name] = weighted_ipcs.sum()
            else:
                print(
                    f"Warning: No valid weighted IPCs found for {benchmark_name}"
                )
                benchmark_ipc[benchmark_name] = "N/A"
        else:
            print(f"Warning: No entries found for benchmark {benchmark_name}")
            sys.exit(-1)
            benchmark_ipc[benchmark_name] = "N/A"

    return df, benchmark_ipc  # Return both DataFrame and benchmark IPCs


def rename_to_last_two_parts(col_name):
    """
    Renames a column to its last two dot-separated substrings.
    Returns the original name if it has fewer than two parts.
    """
    parts = col_name.split(".")
    if len(parts) >= 2:
        # Join the last two elements with a dot
        return ".".join(parts[-2:])
    else:
        # Return original name if fewer than 2 parts (e.g., 'IPC', 'ShortName')
        return col_name


def print_aligned_table(df):
    """Pretty-print a DataFrame with fixed-width columns."""
    if df is None or df.empty:
        print("No data to display.")
        return

    df_str = df.copy()
    # Convert all entries to strings with 3 decimal places where numeric.
    for col in df_str.columns:
        df_str[col] = df_str[col].apply(
            lambda x: f"{x:.3f}" if isinstance(x, numbers.Number) else str(x)
        )

    columns = ["index"] + list(df_str.columns)
    widths = [max(len("index"), max(len(str(idx)) for idx in df_str.index))]
    for col in df_str.columns:
        col_width = max(len(col), max(len(str(val)) for val in df_str[col]))
        widths.append(col_width)

    # Header
    header_cells = [col.ljust(widths[i]) for i, col in enumerate(columns)]
    print(" | ".join(header_cells))
    print("-+-".join("-" * w for w in widths))

    # Rows
    for idx, row in df_str.iterrows():
        cells = [str(idx).ljust(widths[0])]
        for i, col in enumerate(df_str.columns, start=1):
            cells.append(str(row[col]).ljust(widths[i]))
        print(" | ".join(cells))


def add_geomean_row(df, baseline_col=None):
    """
    Adds a geomean row to the DataFrame for numeric columns.
    If baseline_col is provided, percent columns are assumed to follow
    immediately after each dir column and are computed based on geomean.
    """
    import numpy as np

    if df is None or df.empty:
        return df

    geomean = lambda s: (
        float(np.exp(np.log(s[s > 0]).mean())) if (s > 0).any() else np.nan
    )

    new_rows = {}
    for col in df.columns:
        if baseline_col and col.startswith("%"):
            continue
        try:
            series = pd.to_numeric(df[col], errors="coerce")
        except Exception:
            continue
        new_rows[col] = geomean(series.dropna())

    geomean_df = pd.DataFrame(new_rows, index=["geomean"])

    if baseline_col:
        # Compute percent cols after adding geomean values.
        for col in list(df.columns):
            if col == baseline_col or col.startswith("%"):
                continue
            pct_col = f"% {col}"
            if pct_col in df.columns and baseline_col in geomean_df.columns:
                geomean_df[pct_col] = (
                    (geomean_df[col] - geomean_df[baseline_col])
                    / geomean_df[baseline_col]
                    * 100.0
                )

    return pd.concat([df, geomean_df])


def add_spec_score_row(df, baseline_col=None):
    """
    Adds a spec_score row (sum across benchmarks) for numeric columns.
    """
    if df is None or df.empty:
        return df

    spec = {}
    for col in df.columns:
        if baseline_col and col.startswith("%"):
            continue
        try:
            series = pd.to_numeric(df[col], errors="coerce")
        except Exception:
            continue
        spec[col] = series.sum()

    spec_df = pd.DataFrame(spec, index=["spec_score"])

    if baseline_col:
        for col in list(df.columns):
            if col == baseline_col or col.startswith("%"):
                continue
            pct_col = f"% {col}"
            if pct_col in df.columns and baseline_col in spec_df.columns:
                spec_df[pct_col] = (
                    (spec_df[col] - spec_df[baseline_col])
                    / spec_df[baseline_col]
                    * 100.0
                )

    return pd.concat([df, spec_df])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Process stats.txt files in subdirectories."
    )
    parser.add_argument(
        "--dirs",
        required=True,
        nargs="+",
        help="The directories containing the results.",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Output JSON file name (default: stats_summary.json)",
        default="stats_summary.json",
    )
    parser.add_argument(
        "--stats",
        required=True,
        nargs="+",
        help="Regex pattern for extracting stats.",
    )
    parser.add_argument(
        "--exact",
        action="store_true",
        help="Match exact stat.",
    )
    parser.add_argument(
        "--checkpoint",
        help="Get stats for the checkpoint.",
    )

    args = parser.parse_args()
    base_dirs = args.dirs
    extract_stats = args.stats
    output_file = args.output
    exact_stat = args.exact

    all_stats = {}
    all_ipc = []
    all_ipc_df = pd.DataFrame()
    for base_directory in base_dirs:
        try:
            if not os.path.isdir(base_directory):
                raise FileNotFoundError(
                    f"Directory '{base_directory}' not found."
                )

            base_dir = str(Path(base_directory))

            df_stats, benchmark_ipc = create_stats_dataframe(base_dir)
            all_stats[base_dir] = df_stats
            benchmark_ipc_df = pd.DataFrame.from_dict(
                benchmark_ipc, orient="index", columns=[base_dir]
            )
            all_ipc_df = pd.concat([all_ipc_df, benchmark_ipc_df], axis=1)

        except FileNotFoundError as e:
            print(f"Error: {e}")
        except Exception as e:
            print(f"An error occurred: {e}")

    # Ensure IPC rows follow benchmark number order.
    if not all_ipc_df.empty:
        all_ipc_df = all_ipc_df.reindex(BENCHMARK_ORDER)

    if all_stats is not None:
        # Convert DataFrame to dictionary for JSON output
        with open(output_file, "w") as f:
            json_strings = {
                base_dir: df.to_json(orient="index", indent=4)
                for base_dir, df in all_stats.items()
            }
            json_objects = {
                base_dir: json.loads(json_string)
                for base_dir, json_string in json_strings.items()
            }
            json.dump(json_objects, f, indent=4)

        print(f"Stats summary saved to {output_file} in JSON format")

        matching_stats = []
        if extract_stats is not None:
            bench_list = []
            stat_df_bench = pd.DataFrame()
            for bench_run, df in all_stats.items():
                bench_list.append(bench_run)
                for stat in extract_stats:
                    print(
                        f"Extracting stat pattern '{stat}' in bench {bench_run}"
                    )
                    stat_df = (
                        df.filter(items=[stat])
                        if exact_stat
                        else df.filter(regex=rf"{stat}")
                    )
                    if not stat_df.empty:
                        matching_stats.extend([col for col in stat_df.columns])
                        stat_df_bench = stat_df_bench.join(
                            stat_df, how="outer", rsuffix=f"_{bench_run}"
                        )

            # Calculate %diff for all stats
            for stat in matching_stats:
                base_bench = bench_list[0]
                for bench in bench_list[1:]:
                    stat_bench = stat + f"_{bench}"
                    percent_stat = stat + f"_{bench}" + "%"
                    stat_df_bench[percent_stat] = (
                        (stat_df_bench[stat_bench] / stat_df_bench[stat]) - 1
                    ) * 100.0

            stat_df_bench = stat_df_bench.rename(
                columns=rename_to_last_two_parts
            )
            if not args.checkpoint:
                base_name = bench_list[0] if len(bench_list) > 0 else None
                stat_df_bench = add_geomean_row(stat_df_bench, base_name)
                if any("ipc" in c.lower() for c in stat_df_bench.columns):
                    stat_df_bench = add_spec_score_row(
                        stat_df_bench, base_name
                    )
                print_aligned_table(stat_df_bench)
            else:
                filtered = stat_df_bench.filter(
                    like=f"{args.checkpoint}", axis=0
                )
                base_name = bench_list[0] if len(bench_list) > 0 else None
                filtered = add_geomean_row(filtered, base_name)
                if any("ipc" in c.lower() for c in filtered.columns):
                    filtered = add_spec_score_row(filtered, base_name)
                print_aligned_table(filtered)

    new_columns = []
    base_col = all_ipc_df.columns[0]
    for i, col in enumerate(all_ipc_df.columns):
        new_columns.append(col)
        if i > 0:  # Start from the second column (index 1)
            new_col_name = f"% {col}"
            all_ipc_df[new_col_name] = (
                (all_ipc_df[col] - all_ipc_df[base_col]) / all_ipc_df[base_col]
            ) * 100
            # Handle division by zero error
            # all_ipc_df.loc[all_ipc_df[base_col] == 0, new_col_name] = all_ipc_df.loc[all_ipc_df[base_col] == 0].apply(lambda x: float('inf') if x[col]!=0 else 0, axis=1)
            new_columns.append(new_col_name)

    all_ipc_df = all_ipc_df[new_columns]
    all_ipc_df = add_geomean_row(all_ipc_df, base_col)
    all_ipc_df = add_spec_score_row(all_ipc_df, base_col)
    print_aligned_table(all_ipc_df)
