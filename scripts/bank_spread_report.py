#!/usr/bin/env python3
"""Per-array bank-spread report from *_layout_observation.json files.

The mapper emits a layout observation after every Map Success (diagnostic-only).
Each touched memPE entry carries its wired SPM bank (canonical, time-independent
key). This report shows, per array: number of LD/ST nodes, distinct memPEs
(spatial), distinct banks, and the bank list — the metric for evaluating
placement policies such as --bank_affinity_cost.

Usage:
  python3 scripts/bank_spread_report.py <observation.json> [more.json ...]

With two or more files, each is reported separately (label = parent dir + file),
so A/B runs can be eyeballed side by side.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def summarize_observation(doc: dict) -> dict:
    arrays = doc.get("arrays", {})
    rows = []
    total_bank_spread = 0
    for name in sorted(arrays):
        arr = arrays[name]
        touched = arr.get("memPEs_touched", [])
        banks = sorted({pe.get("bank", "") for pe in touched if pe.get("bank")})
        row = {
            "array": name,
            "ld_st": len(arr.get("ld_st_node_idxs", [])),
            "mem_pes": len({(pe.get("x"), pe.get("y")) for pe in touched}),
            "banks": banks,
        }
        total_bank_spread += len(banks)
        rows.append(row)
    return {"rows": rows, "total_bank_spread": total_bank_spread}


def matching_decision_path(path: Path) -> Path:
    suffix = "_layout_observation.json"
    if path.name.endswith(suffix):
        return path.with_name(path.name[:-len(suffix)] + "_layout_decision.json")
    return path.with_name(path.stem + "_layout_decision.json")


def report(path: Path) -> None:
    doc = json.loads(path.read_text(encoding="utf-8"))
    summary = summarize_observation(doc)
    print(f"\n== {path} ==")
    if not summary["rows"]:
        print("  (no arrays recorded)")
        return
    print(f"  {'array':<24} {'ld_st':>5} {'memPEs':>6} {'banks':>5}  bank list")
    for row in summary["rows"]:
        print(
            f"  {row['array']:<24} {row['ld_st']:>5} {row['mem_pes']:>6} "
            f"{len(row['banks']):>5}  {', '.join(row['banks'])}"
        )
    print(
        "  total bank-spread (sum of per-array distinct banks): "
        f"{summary['total_bank_spread']}"
    )

    decision_path = matching_decision_path(path)
    if decision_path.is_file():
        decision = json.loads(decision_path.read_text(encoding="utf-8"))
        strategies = {
            name: arr.get("strategy", "unknown")
            for name, arr in decision.get("arrays", {}).items()
        }
        fallback_arrays = sorted(
            name for name, strategy in strategies.items()
            if strategy.startswith("fallback")
        )
        decided = sum(strategy != "untouched" for strategy in strategies.values())
        print(
            f"  layout fallbacks: {len(fallback_arrays)}/{decided} decided arrays"
            + (f" ({', '.join(fallback_arrays)})" if fallback_arrays else "")
        )


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for arg in sys.argv[1:]:
        p = Path(arg)
        if not p.is_file():
            print(f"skip (not a file): {p}")
            continue
        report(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
