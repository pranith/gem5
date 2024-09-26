#!/bin/env python3

import argparse
import json
import os, sys
import re

from pathlib import Path

import pandas as pd


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
    benchmark_mapping = {
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
                            for k, v in benchmark_mapping.items()
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

                benchmark_name = benchmark_mapping[benchmark_number]
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

                stats["IPC"] = stats["board.processor.cores.core.ipc"]
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
    for benchmark_number, benchmark_name in benchmark_mapping.items():
        benchmark_entries = df[df.index.str.contains(f"chkpt_{benchmark_name}_")]
        if not benchmark_entries.empty:
            weighted_ipcs = benchmark_entries[benchmark_entries['weighted_IPC'] != "N/A"]['weighted_IPC'].astype(float)
            if not weighted_ipcs.empty:
                benchmark_ipc[benchmark_name] = weighted_ipcs.sum()
            else:
                print(f"Warning: No valid weighted IPCs found for {benchmark_name}")
                benchmark_ipc[benchmark_name] = "N/A"
        else:
            print(f"Warning: No entries found for benchmark {benchmark_name}")
            sys.exit(-1)
            benchmark_ipc[benchmark_name] = "N/A"

    return df, benchmark_ipc  # Return both DataFrame and benchmark IPCs
    

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Process stats.txt files in subdirectories."
    )
    parser.add_argument(
        "base_dirs", nargs="+", help="The directories containing the results."
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Output JSON file name (default: stats_summary.json)",
        default="stats_summary.json",
    )

    args = parser.parse_args()
    base_dirs = args.base_dirs
    output_file = args.output

    all_stats = {}
    all_ipc   = []
    all_ipc_df = pd.DataFrame()
    for base_directory in base_dirs:
        try:
            if not os.path.isdir(base_directory):
                raise FileNotFoundError(f"Directory '{base_directory}' not found.")

            base_dir = str(Path(base_directory))

            df_stats, benchmark_ipc = create_stats_dataframe(base_dir)
            all_stats[base_dir] = df_stats
            benchmark_ipc_df = pd.DataFrame.from_dict(benchmark_ipc, orient='index', columns=[base_dir])
            all_ipc_df = pd.concat([all_ipc_df, benchmark_ipc_df], axis=1)

        except FileNotFoundError as e:
            print(f"Error: {e}")
        except Exception as e:
            print(f"An error occurred: {e}")

    if all_stats is not None:
        # Convert DataFrame to dictionary for JSON output
        with open(output_file, "w") as f:
            json_strings = {base_dir: df.to_json(orient="index", indent=4) for base_dir, df in all_stats.items()}
            json_objects = {base_dir: json.loads(json_string) for base_dir, json_string in json_strings.items()}
            json.dump(json_objects, f, indent=4)

        print(f"Stats summary saved to {output_file} in JSON format")


    new_columns = []
    base_col = all_ipc_df.columns[0]
    for i, col in enumerate(all_ipc_df.columns):
        new_columns.append(col)
        if i > 0:  # Start from the second column (index 1)
            new_col_name = f'{col}_%'
            all_ipc_df[new_col_name] = ((all_ipc_df[col] - all_ipc_df[base_col]) / all_ipc_df[base_col]) * 100
            #Handle division by zero error
            # all_ipc_df.loc[all_ipc_df[base_col] == 0, new_col_name] = all_ipc_df.loc[all_ipc_df[base_col] == 0].apply(lambda x: float('inf') if x[col]!=0 else 0, axis=1)
            new_columns.append(new_col_name)
            

    all_ipc_df = all_ipc_df[new_columns] 
    pd.options.display.float_format = '{:.2f}'.format
    print(all_ipc_df.to_string())
    # print(all_ipc_df)
