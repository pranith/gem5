#!/usr/bin/env python3
import argparse
import csv
import re
from collections import defaultdict
from statistics import mean


ALLOC_RE = re.compile(
    r"^(?P<cycle>\d+): .*Allocating a new MB entry for Addr:0x(?P<addr>[0-9a-f]+) ver:(?P<ver>\d+)\b"
)
DRAIN_RE = re.compile(
    r"^(?P<cycle>\d+): .*Drain response for merge buffer entry with block addr:0x(?P<addr>[0-9a-f]+) ver:(?P<ver>\d+)\b"
)


def parse_trace(path):
    alloc = defaultdict(list)
    drain = defaultdict(list)
    with open(path, "r", errors="ignore") as handle:
        for line in handle:
            match = ALLOC_RE.match(line)
            if match:
                cycle = int(match.group("cycle"))
                addr = match.group("addr")
                ver = int(match.group("ver"))
                alloc[(addr, ver)].append(cycle)
                continue
            match = DRAIN_RE.match(line)
            if match:
                cycle = int(match.group("cycle"))
                addr = match.group("addr")
                ver = int(match.group("ver"))
                drain[(addr, ver)].append(cycle)
    return alloc, drain


def build_occurrences_by_addr(alloc, drain, prefer_ver=None):
    occurrences = defaultdict(list)
    versions_by_addr = defaultdict(list)
    for (addr, ver) in alloc:
        versions_by_addr[addr].append(ver)

    for addr, versions in versions_by_addr.items():
        versions = sorted(set(versions))
        selected_ver = None
        if prefer_ver is not None and prefer_ver in versions:
            selected_ver = prefer_ver
        elif versions:
            selected_ver = versions[0]
        if selected_ver is None:
            continue
        a_list = alloc.get((addr, selected_ver), [])
        d_list = drain.get((addr, selected_ver), [])
        n = min(len(a_list), len(d_list))
        for i in range(n):
            occurrences[addr].append((selected_ver, a_list[i], d_list[i]))
    return occurrences


def iter_mb_rows(alloc, drain, addr_filter=None, ver_filter=None):
    for (addr, ver), a_list in alloc.items():
        if addr_filter and addr != addr_filter:
            continue
        if ver_filter is not None and ver != ver_filter:
            continue
        d_list = drain.get((addr, ver), [])
        n = min(len(a_list), len(d_list))
        for idx in range(n):
            yield addr, ver, idx, a_list[idx], d_list[idx]


def main():
    parser = argparse.ArgumentParser(
        description="Compare MB alloc/drain deltas between two gem5 runs."
    )
    parser.add_argument("--mb-trace", required=True, help="Path to mb trace.txt")
    parser.add_argument(
        "--nover-trace", required=True, help="Path to nover trace.txt"
    )
    parser.add_argument(
        "--addr",
        help="Filter by block address (hex without 0x, e.g. 98100).",
    )
    parser.add_argument(
        "--mb-version",
        type=int,
        help="Filter by MB version in the mb run.",
    )
    parser.add_argument(
        "--format",
        choices=["csv", "table"],
        default="csv",
        help="Output format.",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="Print summary stats to stderr.",
    )
    args = parser.parse_args()

    addr_filter = args.addr.lower() if args.addr else None

    mb_alloc, mb_drain = parse_trace(args.mb_trace)
    nv_alloc, nv_drain = parse_trace(args.nover_trace)

    nv_occ = build_occurrences_by_addr(nv_alloc, nv_drain, prefer_ver=0)

    rows = []
    for addr, ver, idx, a, d in iter_mb_rows(
        mb_alloc, mb_drain, addr_filter=addr_filter, ver_filter=args.mb_version
    ):
        mb_delta = d - a
        nv = nv_occ.get(addr, [])
        nv_ver = None
        nv_alloc_cycle = None
        nv_drain_cycle = None
        nv_delta = None
        if idx < len(nv):
            nv_ver, nv_alloc_cycle, nv_drain_cycle = nv[idx]
            nv_delta = nv_drain_cycle - nv_alloc_cycle
        delta_diff = mb_delta - nv_delta if nv_delta is not None else None
        rows.append(
            {
                "addr": f"0x{addr}",
                "mb_ver": ver,
                "occ": idx,
                "mb_alloc": a,
                "mb_drain": d,
                "mb_delta": mb_delta,
                "nover_ver": nv_ver,
                "nover_alloc": nv_alloc_cycle,
                "nover_drain": nv_drain_cycle,
                "nover_delta": nv_delta,
                "delta_diff": delta_diff,
            }
        )

    if args.format == "csv":
        writer = csv.DictWriter(
            sys.stdout,
            fieldnames=[
                "addr",
                "mb_ver",
                "occ",
                "mb_alloc",
                "mb_drain",
                "mb_delta",
                "nover_ver",
                "nover_alloc",
                "nover_drain",
                "nover_delta",
                "delta_diff",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)
    else:
        header = (
            "addr",
            "mb_ver",
            "occ",
            "mb_alloc",
            "mb_drain",
            "mb_delta",
            "nover_ver",
            "nover_alloc",
            "nover_drain",
            "nover_delta",
            "delta_diff",
        )
        print("\t".join(header))
        for row in rows:
            print("\t".join(str(row[h]) for h in header))

    if args.summary:
        deltas = [r["delta_diff"] for r in rows if r["delta_diff"] is not None]
        if deltas:
            print(
                f"delta_diff count={len(deltas)} mean={mean(deltas):.2f} "
                f"min={min(deltas)} max={max(deltas)}",
                file=sys.stderr,
            )


if __name__ == "__main__":
    import sys

    main()
