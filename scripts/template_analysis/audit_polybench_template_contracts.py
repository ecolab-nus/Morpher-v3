#!/usr/bin/env python3
"""Audit old PolyBench topology matches against source access contracts."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from morpher_runner.systolic_binding import (  # noqa: E402
    assess_source_contract,
    source_model,
)


DEFAULT_RESULTS = REPO_ROOT / "generated" / "morpher_runs" / "template_results"
DEFAULT_REPORTS = (
    REPO_ROOT / "generated" / "morpher_runs" / "source_template_binding"
    / "20260619_polybench_candidates" / "candidate_reports"
)

DETAIL_FIELDS = [
    "benchmark", "kernel", "record_index", "old_templates",
    "old_mapped_architectures", "old_best_topology_util_percent",
    "old_contract_filter_passed", "source_bound_status", "source_template",
    "required_transform", "confidence", "source_load_bases",
    "source_output_bases", "source_mapping_available",
    "source_mapped_architectures", "source_best_mapped_ii",
    "source_best_compute_util_percent", "reason",
]

SUMMARY_FIELDS = [
    "benchmark", "kernel", "matched_records", "direct_records",
    "transformed_records", "conditional_records", "rejected_records",
    "kernel_source_bound_status", "summary",
]


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      "--topology", type=Path,
      default=DEFAULT_RESULTS / "template_topology_results.csv",
  )
  parser.add_argument(
      "--morph-opportunities", type=Path,
      default=DEFAULT_RESULTS / "template_morph_opportunities.csv",
  )
  parser.add_argument("--reports", type=Path, default=DEFAULT_REPORTS)
  parser.add_argument(
      "--source-candidates", type=Path,
      default=DEFAULT_RESULTS / "template_candidates.csv",
  )
  parser.add_argument(
      "--output", type=Path,
      default=DEFAULT_RESULTS / "polybench_source_contract_audit.csv",
  )
  return parser.parse_args()


def read_csv(path: Path) -> list[dict[str, str]]:
  with path.open(encoding="utf-8", newline="") as stream:
    return list(csv.DictReader(stream))


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)


def candidate_report(root: Path, kernel: str) -> Path:
  matches = sorted(root.glob(f"polybench__{kernel}__*/*.json"))
  if len(matches) != 1:
    raise RuntimeError(f"expected one candidate report for {kernel}, found {len(matches)}")
  return matches[0]


def candidate_kernel(benchmark: str) -> str:
  parts = benchmark.split("__")
  if len(parts) >= 2 and parts[0] == "polybench":
    return parts[1]
  path_parts = benchmark.split("/")
  return path_parts[1] if len(path_parts) >= 2 and path_parts[0] == "polybench" else ""


def main() -> int:
  args = parse_args()
  topology = [
      row for row in read_csv(args.topology)
      if row["benchmark"].startswith("polybench/")
  ]
  accepted = {
      (row["benchmark"], row["function"], row["record_index"])
      for row in read_csv(args.morph_opportunities)
      if row["benchmark"].startswith("polybench/")
  }
  mapped: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
  if args.source_candidates.is_file():
    for row in read_csv(args.source_candidates):
      kernel = candidate_kernel(row.get("benchmark", ""))
      if kernel:
        mapped[(kernel, row.get("record_index", ""), row.get("template", ""))].append(row)
  grouped: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
  for row in topology:
    grouped[(row["benchmark"], row["function"], row["record_index"])].append(row)

  details = []
  for (benchmark, kernel, record_index), old_rows in sorted(grouped.items()):
    report = candidate_report(args.reports, kernel)
    records = [
        json.loads(line) for line in report.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    record = records[int(record_index)]
    model = source_model(record)
    assessment = assess_source_contract(record)
    source_maps = mapped.get((kernel, record_index, assessment.template), [])
    best_source_map = max(
        source_maps,
        key=lambda row: (
            float(row.get("compute_util_percent") or 0.0),
            -int(row.get("ii") or 1_000_000),
        ),
        default={},
    )
    utils = [
        float(row["scalar_eq_compute_util_percent"])
        for row in old_rows if row.get("scalar_eq_compute_util_percent")
    ]
    details.append({
        "benchmark": benchmark,
        "kernel": kernel,
        "record_index": record_index,
        "old_templates": ";".join(sorted({row["template"] for row in old_rows})),
        "old_mapped_architectures": len({row["architecture"] for row in old_rows}),
        "old_best_topology_util_percent": f"{max(utils):.3f}" if utils else "",
        "old_contract_filter_passed": (
            (benchmark, kernel, record_index) in accepted
        ),
        "source_bound_status": assessment.status,
        "source_template": assessment.template,
        "required_transform": assessment.transform,
        "confidence": assessment.confidence,
        "source_load_bases": ";".join(dict.fromkeys(
            str(access.get("base") or "")
            for access in record.get("affine_accesses", [])
            if access.get("kind") == "load" and access.get("base")
        )),
        "source_output_bases": ";".join(model.output_bases),
        "source_mapping_available": bool(source_maps),
        "source_mapped_architectures": len({
            row.get("architecture", "") for row in source_maps
            if row.get("architecture")
        }),
        "source_best_mapped_ii": best_source_map.get("ii", ""),
        "source_best_compute_util_percent": best_source_map.get(
            "compute_util_percent", ""
        ),
        "reason": assessment.reason,
    })

  by_kernel: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
  for row in details:
    by_kernel[(str(row["benchmark"]), str(row["kernel"]))].append(row)
  summaries = []
  for (benchmark, kernel), rows in sorted(by_kernel.items()):
    counts = {
        status: sum(row["source_bound_status"] == status for row in rows)
        for status in ("direct", "transform", "conditional", "no")
    }
    if counts["no"]:
      kernel_status = "requires_algorithmic_rewrite"
      summary = "at least one old match is not a source-bound recurrence"
    elif counts["conditional"]:
      kernel_status = "conditional"
      summary = "source-bound shape is plausible but an explicit transform/proof is missing"
    else:
      kernel_status = "source_bound"
      summary = "all old matched records have direct or statically justified source contracts"
    summaries.append({
        "benchmark": benchmark,
        "kernel": kernel,
        "matched_records": len(rows),
        "direct_records": counts["direct"],
        "transformed_records": counts["transform"],
        "conditional_records": counts["conditional"],
        "rejected_records": counts["no"],
        "kernel_source_bound_status": kernel_status,
        "summary": summary,
    })

  write_csv(args.output, DETAIL_FIELDS, details)
  summary_path = args.output.with_name("polybench_source_contract_summary.csv")
  write_csv(summary_path, SUMMARY_FIELDS, summaries)
  print(f"wrote {len(details)} record audits to {args.output}")
  print(f"wrote {len(summaries)} kernel summaries to {summary_path}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
