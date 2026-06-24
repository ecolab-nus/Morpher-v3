#!/usr/bin/env python3
"""Build one normalized CSV for all successful systolic/template mappings."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from morpher_runner.systolic_binding import source_model  # noqa: E402
DEFAULT_LEGACY = (
    REPO_ROOT / "generated" / "morpher_runs" / "systolic_arch_sweep"
    / "20260525_full_nonvred_j32"
    / "systolic_arch_results_with_rerun_best.csv"
)
DEFAULT_SOURCE_ROOT = (
    REPO_ROOT / "generated" / "morpher_runs" / "source_template_binding"
)
DEFAULT_OUTPUT = (
    REPO_ROOT / "generated" / "morpher_runs" / "template_results"
    / "template_results.csv"
)


FIELDS = [
    "result_family",
    "benchmark",
    "function",
    "record_index",
    "kernel",
    "template",
    "transform",
    "source_shape",
    "architecture",
    "simd_width",
    "multihop",
    "active_cells",
    "compute_nodes",
    "ii",
    "compute_pe_util",
    "compute_pe_util_percent",
    "active_lane_util",
    "active_lane_util_percent",
    "scalar_eq_compute_util",
    "scalar_eq_compute_util_percent",
    "compute_util",
    "compute_util_percent",
    "scalar_eq_ops_per_cycle",
    "mapper_success",
    "binding_status",
    "treg_audit_valid",
    "steady_state_verified",
    "verification_scope",
    "functional_verified",
    "verify_matches",
    "verify_mismatches",
    "source_equivalent",
    "semantic_compatible",
    "semantic_reason",
    "result_class",
    "selected_run",
    "source_file",
]


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--legacy-results", type=Path, default=DEFAULT_LEGACY)
  parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
  parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
  return parser.parse_args()


def relative(path: Path) -> str:
  try:
    return str(path.resolve().relative_to(REPO_ROOT))
  except ValueError:
    return str(path.resolve())


def as_bool(value: object) -> bool:
  if isinstance(value, bool):
    return value
  return str(value).strip().lower() in {"1", "true", "yes"}


def simd_width(arch: str) -> int:
  match = re.search(r"vec(\d+)", arch)
  return int(match.group(1)) if match else 1


def config_facts(directory: Path) -> tuple[str, int]:
  configs = sorted(directory.glob("*config*.yaml"))
  if not configs:
    return "", 4
  text = configs[-1].read_text(encoding="utf-8")
  arch_match = re.search(r"^\s*arch_template_json\s*:\s*[\"']?([^\"'\n]+)", text, re.M)
  hop_match = re.search(r"^\s*multihop\s*:\s*[\"']?(\d+)", text, re.M)
  arch = arch_match.group(1).strip() if arch_match else ""
  hops = int(hop_match.group(1)) if hop_match else 4
  return arch, hops


def benchmark_from_report(report: str) -> str:
  if not report:
    return ""
  path = Path(report)
  parent = path.parent.name
  if parent and parent != "candidate_reports":
    return parent
  return path.stem


def utilization(active: int, capacity: int, ii: int) -> float:
  return active / (capacity * ii) if capacity > 0 and ii > 0 else 0.0


def candidate_records(legacy_path: Path) -> dict[tuple[str, str], list[dict]]:
  summary = legacy_path.parent / "candidate_summary.csv"
  records: dict[tuple[str, str], list[dict]] = {}
  if not summary.is_file():
    return records
  with summary.open(encoding="utf-8", newline="") as stream:
    for row in csv.DictReader(stream):
      report = Path(row.get("report") or "")
      if not report.is_file():
        continue
      records[(row["benchmark"], row["function"])] = [
          json.loads(line) for line in report.read_text(encoding="utf-8").splitlines()
          if line.strip()
      ]
  return records


def legacy_semantic_contract(
    template: str, record: dict | None,
) -> tuple[str, bool, str]:
  if record is None:
    return "", False, "template row cannot be linked to a source candidate record"
  model = source_model(record)
  shape = model.source_shape
  products = model.products_per_reduction_step
  if (
      template == "gemm_fanout"
      and shape == "two_axis_reduction"
      and products == 1
  ):
    return shape, True, (
        "two parallel output axes and one product per reduction step permit "
        "GEMM fanout/output-stationary lowering"
    )
  if (
      template in {"matvec_row", "matvec_parallel"}
      and shape == "one_axis_reduction"
      and products == 1
  ):
    return shape, True, (
        "one parallel output axis and one product per reduction step permit "
        "batched dot-product lowering"
    )
  if template == "gemm_chain" and shape == "fixed_weighted_sum":
    return shape, True, (
        "fixed weighted-sum arithmetic permits a chain/window lowering; exact "
        "source accesses must still be bound and simulated"
    )
  return shape, False, (
      f"source contract {shape} with {products} product(s) per reduction step "
      f"does not match {template}"
  )


def legacy_rows(path: Path) -> list[dict[str, object]]:
  rows: list[dict[str, object]] = []
  records = candidate_records(path)
  with path.open(encoding="utf-8", newline="") as stream:
    for source in csv.DictReader(stream):
      if source.get("map_status") != "success" or not source.get("ii"):
        continue
      ii = int(source["ii"])
      util = float(source["comp_util"])
      lanes = simd_width(source["arch"])
      active = int(source.get("cells") or 0)
      synth_match = re.search(r"_synth(\d+)$", source.get("kernel", ""))
      record_index = int(synth_match.group(1)) if synth_match else -1
      candidates = records.get((source.get("benchmark", ""), source.get("function", "")), [])
      record = candidates[record_index] if 0 <= record_index < len(candidates) else None
      shape, semantic_compatible, semantic_reason = legacy_semantic_contract(
          source.get("template", ""), record
      )
      rows.append({
          "result_family": "legacy_shape_template",
          "benchmark": source.get("benchmark", ""),
          "function": source.get("function", ""),
          "record_index": record_index if record_index >= 0 else "",
          "kernel": source.get("kernel", ""),
          "template": source.get("template", ""),
          "transform": "shape_only",
          "source_shape": shape,
          "architecture": source.get("arch", ""),
          "simd_width": lanes,
          "multihop": 4,
          "active_cells": active,
          "compute_nodes": active,
          "ii": ii,
          "compute_pe_util": f"{util:.6f}",
          "compute_pe_util_percent": f"{100.0 * util:.3f}",
          "active_lane_util": "1.000000",
          "active_lane_util_percent": "100.000",
          "scalar_eq_compute_util": f"{util:.6f}",
          "scalar_eq_compute_util_percent": f"{100.0 * util:.3f}",
          "compute_util": f"{util:.6f}",
          "compute_util_percent": f"{100.0 * util:.3f}",
          "scalar_eq_ops_per_cycle": f"{active * lanes / ii:.3f}",
          "mapper_success": True,
          "binding_status": "shape_only",
          "treg_audit_valid": "",
          "steady_state_verified": False,
          "verification_scope": "",
          "functional_verified": False,
          "verify_matches": "",
          "verify_mismatches": "",
          "source_equivalent": False,
          "semantic_compatible": semantic_compatible,
          "semantic_reason": semantic_reason,
          "result_class": "topology_mapping_only",
          "selected_run": source.get("selected_run", ""),
          "source_file": relative(path),
      })
  return rows


def binding_rows(root: Path) -> list[dict[str, object]]:
  selected: dict[tuple[object, ...], dict[str, object]] = {}
  for path in sorted(root.glob("**/bindings.csv")):
    arch, hops = config_facts(path.parent)
    verification_path = path.with_name("steady_state_verification.json")
    verifications = {}
    if verification_path.is_file():
      for item in json.loads(verification_path.read_text(encoding="utf-8")):
        verify_key = (
            str(item.get("source_report", "")),
            str(item.get("function", "")),
            str(item.get("record_index", "")),
            str(item.get("template", "")),
        )
        verifications[verify_key] = item
    with path.open(encoding="utf-8", newline="") as stream:
      for source in csv.DictReader(stream):
        if source.get("map_status") != "success" or not source.get("mapped_ii"):
          continue
        ii = int(source["mapped_ii"])
        active = int(source.get("active_cells") or 0)
        capacity = int(source.get("template_cells") or 16)
        util = utilization(active, capacity, ii)
        lanes = int(source.get("simd_width") or simd_width(arch))
        record_index = source.get("record_index", "")
        key = (
            source.get("source_report", ""), source.get("function", ""),
            record_index, source.get("template", ""), source.get("transform", ""),
            arch, lanes, hops, active, ii,
        )
        binding = source.get("binding_status", "")
        semantic_compatible = binding == "supported"
        verify_key = (
            source.get("source_report", ""), source.get("function", ""),
            str(record_index), source.get("template", ""),
        )
        verification = verifications.get(verify_key, {})
        steady_state_verified = as_bool(
            verification.get("steady_state_verified", False)
        )
        candidate = {
            "result_family": "source_bound_template",
            "benchmark": benchmark_from_report(source.get("source_report", "")),
            "function": source.get("function", ""),
            "record_index": record_index,
            "kernel": Path(source.get("artifact_xml", "")).stem,
            "template": source.get("template", ""),
            "transform": source.get("transform", ""),
            "source_shape": source.get("source_shape", ""),
            "architecture": arch,
            "simd_width": lanes,
            "multihop": int(source.get("mapper_multihop") or hops),
            "active_cells": active,
            "compute_nodes": active,
            "ii": ii,
            "compute_pe_util": f"{util:.6f}",
            "compute_pe_util_percent": f"{100.0 * util:.3f}",
            "active_lane_util": "1.000000",
            "active_lane_util_percent": "100.000",
            "scalar_eq_compute_util": f"{util:.6f}",
            "scalar_eq_compute_util_percent": f"{100.0 * util:.3f}",
            "compute_util": f"{util:.6f}",
            "compute_util_percent": f"{100.0 * util:.3f}",
            "scalar_eq_ops_per_cycle": f"{active * lanes / ii:.3f}",
            "mapper_success": True,
            "binding_status": binding,
            "treg_audit_valid": "",
            "steady_state_verified": steady_state_verified,
            "verification_scope": verification.get("verification_scope", ""),
            "functional_verified": False,
            "verify_matches": verification.get("verify_matches", ""),
            "verify_mismatches": verification.get("verify_mismatches", ""),
            "source_equivalent": False,
            "semantic_compatible": semantic_compatible,
            "semantic_reason": (
                "source loop/access contract matches template transform"
                if semantic_compatible else source.get("binding_reason", "")
            ),
            "result_class": (
                "mapped_main_steady_state_verified"
                if steady_state_verified else "mapped_main_unverified"
            ),
            "selected_run": "source_binding",
            "source_file": relative(path),
        }
        current = selected.get(key)
        if current is None or (
            bool(candidate["steady_state_verified"]),
            bool(candidate["functional_verified"]),
        ) > (
            bool(current["steady_state_verified"]),
            bool(current["functional_verified"]),
        ):
          selected[key] = candidate
  return list(selected.values())


def main() -> int:
  args = parse_args()
  topology = legacy_rows(args.legacy_results.resolve())
  attempts = binding_rows(args.source_root.resolve())
  sort_key = lambda row: (
      str(row["benchmark"]), str(row["function"]), str(row["architecture"]),
      str(row["template"]), int(row["ii"]), int(row["active_cells"]),
  )
  attempts.sort(key=sort_key)
  topology.sort(key=sort_key)
  candidates = [
      row for row in attempts
      if row["semantic_compatible"]
      and row["result_class"] != "functional_mismatch"
      and (
          row["result_family"] != "phase_unrolled_window"
          or row["functional_verified"]
          or row["result_class"] == "mapped_schedule_phase_model_inconclusive"
      )
  ]
  rows = [
      row for row in candidates
      if row["source_equivalent"] and row["functional_verified"]
  ]
  output = args.output.resolve()
  output.parent.mkdir(parents=True, exist_ok=True)

  def write(path: Path, selected: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
      writer = csv.DictWriter(stream, fieldnames=FIELDS)
      writer.writeheader()
      writer.writerows(selected)

  write(output, rows)
  write(output.with_name("template_attempts.csv"), attempts)
  write(output.with_name("template_candidates.csv"), candidates)
  write(output.with_name("template_topology_results.csv"), topology)
  write(
      output.with_name("template_morph_opportunities.csv"),
      [row for row in topology if row["semantic_compatible"]],
  )
  families: dict[str, int] = {}
  for row in rows:
    family = str(row["result_family"])
    families[family] = families.get(family, 0) + 1
  print(json.dumps({
      "output": relative(output),
      "rows": len(rows),
      "families": families,
      "topology_rows": len(topology),
      "source_attempt_rows": len(attempts),
      "source_candidate_rows": len(candidates),
      "morph_opportunity_rows": sum(
          bool(row["semantic_compatible"]) for row in topology
      ),
  }, indent=2))
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
