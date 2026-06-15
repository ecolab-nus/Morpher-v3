#!/usr/bin/env python3
"""Aggregate per-phase mapping stats JSON files into a single CSV.

The Morpher mapper writes one ``<dfg>...<arch>_stats.json`` per successful
mapping (see ``CGRA::EmitMappingStats``). This script walks a directory tree,
collects those stats files, and emits a CSV with one row per stats file plus
optional combined / amortized utilization columns.

Util definitions:
    util_phase          = scalar_eq_compute / (num_scalar_eq_ALUs * II)
                          (steady-state per-iteration ALU utilization)
    util_amortized(K)   = K * scalar_eq_compute
                          / (num_scalar_eq_ALUs * (K + 2*(N-1)) * II)
                          where N is the SWP stage count. Captures the
                          prologue/epilogue (fill+drain) overhead for finite
                          iteration counts K.

Usage:
    python scripts/aggregate_mapping_stats.py <root_dir>
    python scripts/aggregate_mapping_stats.py <root_dir> --output stats.csv
    python scripts/aggregate_mapping_stats.py <root_dir> --K 100
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path


COLUMNS = [
    "stats_file",
    "arch_name",
    "phase",
    "II",
    "schedule_length_phase",
    "dfg_critical_path",
    "stage_count_estimate",
    "swp_fill_cycles",
    "swp_drain_cycles",
    "node_count",
    "num_PE",
    "num_PE_MEM",
    "num_PE_DUMMY",
    "PE_simd_width",
    "PE_MEM_simd_width",
    "num_scalar_eq_ALUs",
    "compute_vector",
    "compute_scalar",
    "load",
    "store",
    "iter_ctrl",
    "scalar_eq_compute_ops",
    "util_phase",
    "util_amortized_K",
]


def util_amortized(scalar_eq_compute: int,
                   num_alus: int,
                   ii: int,
                   stage_count: int,
                   K: int) -> float:
    if num_alus <= 0 or ii <= 0 or K <= 0:
        return 0.0
    total_cycles = (K + 2 * max(stage_count - 1, 0)) * ii
    total_compute = K * scalar_eq_compute
    denom = num_alus * total_cycles
    return float(total_compute) / float(denom) if denom > 0 else 0.0


def row_from_stats(stats_path: Path, K_for_amort: int) -> dict:
    data = json.loads(stats_path.read_text(encoding="utf-8"))
    grid = data.get("grid", {})
    ops = data.get("op_counts", {})
    ii = int(data.get("II", 0))
    n_alus = int(data.get("num_scalar_eq_ALUs", 0))
    scalar_eq = int(data.get("scalar_eq_compute_ops", 0))
    stage_n = int(data.get("stage_count_estimate", 1))
    return {
        "stats_file": str(stats_path),
        "arch_name": data.get("arch_name", ""),
        "phase": data.get("phase", "kernel"),
        "II": ii,
        "schedule_length_phase": data.get("schedule_length_phase", ""),
        "dfg_critical_path": data.get("dfg_critical_path", ""),
        "stage_count_estimate": stage_n,
        "swp_fill_cycles": data.get("swp_fill_cycles", ""),
        "swp_drain_cycles": data.get("swp_drain_cycles", ""),
        "node_count": data.get("node_count", ""),
        "num_PE": grid.get("num_PE", ""),
        "num_PE_MEM": grid.get("num_PE_MEM", ""),
        "num_PE_DUMMY": grid.get("num_PE_DUMMY", ""),
        "PE_simd_width": grid.get("PE_simd_width", ""),
        "PE_MEM_simd_width": grid.get("PE_MEM_simd_width", ""),
        "num_scalar_eq_ALUs": n_alus,
        "compute_vector": ops.get("compute_vector", 0),
        "compute_scalar": ops.get("compute_scalar", 0),
        "load": ops.get("load", 0),
        "store": ops.get("store", 0),
        "iter_ctrl": ops.get("iter_ctrl", 0),
        "scalar_eq_compute_ops": scalar_eq,
        "util_phase": data.get("util_phase", 0.0),
        "util_amortized_K": util_amortized(scalar_eq, n_alus, ii, stage_n, K_for_amort),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", type=Path, help="Directory to walk for *_stats.json files.")
    ap.add_argument("--output", "-o", type=Path, default=None,
                    help="Output CSV path; default = stdout.")
    ap.add_argument("--K", type=int, default=100,
                    help="Iteration count K used for util_amortized column "
                         "(default: 100). Accounts for SWP fill+drain "
                         "overhead at finite K.")
    ap.add_argument("--pattern", default="*_stats.json",
                    help="Glob pattern relative to root (default: *_stats.json).")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"root does not exist: {args.root}", file=sys.stderr)
        return 2

    stats_files = sorted(args.root.rglob(args.pattern))
    if not stats_files:
        print(f"No files matching {args.pattern} under {args.root}", file=sys.stderr)
        return 1

    rows = []
    for sp in stats_files:
        try:
            rows.append(row_from_stats(sp, args.K))
        except (json.JSONDecodeError, OSError) as exc:
            print(f"skip {sp}: {exc}", file=sys.stderr)

    out_stream = args.output.open("w", encoding="utf-8", newline="") if args.output else sys.stdout
    try:
        writer = csv.DictWriter(out_stream, fieldnames=COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    finally:
        if args.output:
            out_stream.close()

    if args.output:
        print(f"wrote {len(rows)} rows to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
