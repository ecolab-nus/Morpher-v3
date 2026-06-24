#!/usr/bin/env python3
"""Create the canonical normal/source-template/morph-opportunity summary."""

from __future__ import annotations

import csv
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT = REPO_ROOT / "generated" / "morpher_runs" / "template_results"
SOURCE_COMPARISON = ROOT / "normal_vs_template.csv"
MORPH_COMPARISON = ROOT / "morph_comparison" / "normal_vs_template.csv"
OUTPUT = ROOT / "template_mapping_summary.csv"
UTIL_OUTPUT = ROOT / "template_utilization_comparison.csv"


FIELDS = [
    "benchmark", "function", "architecture",
    "normal_available", "normal_ii", "normal_multihop",
    "normal_compute_pe_util_percent",
    "normal_active_lane_util_percent", "normal_scalar_eq_compute_util_percent",
    "source_template_available", "source_template", "source_transform",
    "source_template_ii", "source_multihop", "source_compute_pe_util_percent",
    "source_active_lane_util_percent", "source_scalar_eq_compute_util_percent",
    "source_result_class", "source_steady_state_verified",
    "source_verification_scope", "source_functional_verified",
    "source_beats_normal", "source_util_delta_percent_points",
    "morph_opportunity_available", "morph_template", "morph_template_ii",
    "morph_multihop",
    "morph_compute_pe_util_percent", "morph_active_lane_util_percent",
    "morph_scalar_eq_compute_util_percent", "morph_beats_normal",
    "morph_util_delta_percent_points", "best_template_class",
    "best_template", "best_template_ii", "best_template_util_percent",
    "best_template_confidence", "adopted_winner", "adoption_reason",
]

UTIL_FIELDS = [
    "benchmark",
    "kernel",
    "architecture",
    "original_dfg_util_percent",
    "original_dfg_multihop",
    "topology_based_util_percent",
    "topology_based_multihop",
    "source_bound_util_percent",
    "source_bound_multihop",
]


def read(path: Path) -> dict[tuple[str, str, str], dict[str, str]]:
  with path.open(encoding="utf-8", newline="") as stream:
    return {
        (row["benchmark"], row["function"], row["architecture"]): row
        for row in csv.DictReader(stream)
    }


def select_best_template(
    source: dict[str, str], morph: dict[str, str]
) -> tuple[str, dict[str, str], str]:
  """Prefer source-instantiated evidence; morph-only rows are opportunity bounds."""
  if source["template_available"] == "True":
    return (
        "instantiated_source_template",
        source,
        source["template_result_class"],
    )
  if morph["template_available"] == "True":
    return (
        "morph_only_topology",
        morph,
        "source_contract_matched_but_not_instantiated",
    )
  return "none", {}, "no_template_mapping"


def experiment_multihop(experiment: str) -> str:
  match = re.search(r"_h(\d+)__", experiment)
  return match.group(1) if match else ""


def utilization_view(row: dict[str, str]) -> dict[str, str]:
  """Return compute-only, SIMD-lane-aware utilization for quick comparison."""
  return {
      "benchmark": row["benchmark"],
      "kernel": row["function"],
      "architecture": row["architecture"],
      "original_dfg_util_percent": row["normal_scalar_eq_compute_util_percent"],
      "original_dfg_multihop": row["normal_multihop"],
      "topology_based_util_percent": row["morph_scalar_eq_compute_util_percent"],
      "topology_based_multihop": row["morph_multihop"],
      "source_bound_util_percent": row["source_scalar_eq_compute_util_percent"],
      "source_bound_multihop": row["source_multihop"],
  }


def main() -> int:
  source = read(SOURCE_COMPARISON)
  morph = read(MORPH_COMPARISON)
  rows = []
  for key in sorted(source):
    src = source[key]
    shp = morph[key]
    best_class, best, confidence = select_best_template(src, shp)
    rows.append({
        "benchmark": key[0],
        "function": key[1],
        "architecture": key[2],
        "normal_available": src["normal_available"],
        "normal_ii": src["normal_ii"],
        "normal_multihop": experiment_multihop(src["normal_experiment"]),
        "normal_compute_pe_util_percent": src["normal_compute_pe_util_percent"],
        "normal_active_lane_util_percent": src["normal_active_lane_util_percent"],
        "normal_scalar_eq_compute_util_percent": src["normal_compute_util_percent"],
        "source_template_available": src["template_available"],
        "source_template": src["template"],
        "source_transform": src["template_transform"],
        "source_template_ii": src["template_ii"],
        "source_multihop": src["template_multihop"],
        "source_compute_pe_util_percent": src["template_compute_pe_util_percent"],
        "source_active_lane_util_percent": src["template_active_lane_util_percent"],
        "source_scalar_eq_compute_util_percent": src["template_compute_util_percent"],
        "source_result_class": src["template_result_class"],
        "source_steady_state_verified": src["template_steady_state_verified"],
        "source_verification_scope": src["template_verification_scope"],
        "source_functional_verified": src["template_functional_verified"],
        "source_beats_normal": src["template_beats_normal"],
        "source_util_delta_percent_points": src["template_util_delta_percent_points"],
        "morph_opportunity_available": shp["template_available"],
        "morph_template": shp["template"],
        "morph_template_ii": shp["template_ii"],
        "morph_multihop": shp["template_multihop"],
        "morph_compute_pe_util_percent": shp["template_compute_pe_util_percent"],
        "morph_active_lane_util_percent": shp["template_active_lane_util_percent"],
        "morph_scalar_eq_compute_util_percent": shp["template_compute_util_percent"],
        "morph_beats_normal": shp["template_beats_normal"],
        "morph_util_delta_percent_points": shp["template_util_delta_percent_points"],
        "best_template_class": best_class,
        "best_template": best.get("template", ""),
        "best_template_ii": best.get("template_ii", ""),
        "best_template_util_percent": best.get("template_compute_util_percent", ""),
        "best_template_confidence": confidence,
        "adopted_winner": src["winner"],
        "adoption_reason": src["adoption_reason"],
    })
  with OUTPUT.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=FIELDS)
    writer.writeheader()
    writer.writerows(rows)
  with UTIL_OUTPUT.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=UTIL_FIELDS)
    writer.writeheader()
    writer.writerows(utilization_view(row) for row in rows)
  print(f"wrote {len(rows)} rows to {OUTPUT.relative_to(REPO_ROOT)}")
  print(f"wrote {len(rows)} rows to {UTIL_OUTPUT.relative_to(REPO_ROOT)}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
