#!/usr/bin/env python3
"""Compare best normal DFG mappings with separately maintained templates."""

from __future__ import annotations

import argparse
import csv
import json
import re
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_NORMAL = (
    REPO_ROOT / "generated" / "morpher_runs" / "arch_sweeps"
    / "edge_mem_arch_sweep" / "allrun.csv"
)
DEFAULT_TEMPLATES = (
    REPO_ROOT / "generated" / "morpher_runs" / "template_results"
    / "template_candidates.csv"
)
DEFAULT_OUTPUT_DIR = DEFAULT_TEMPLATES.parent


COMPUTE_PREFIXES = (
    "ADD", "SUB", "MUL", "DIV", "MAC",
    "AND", "OR", "XOR", "LS", "RS", "ARS",
    "CMP", "CLT", "CGT", "PCMP", "NCMP",
    "SEXT", "MOVC", "MOVCL", "LOADCL",
    "VADD", "VSUB", "VMUL", "VDIV", "VMAC",
    "VAND", "VOR", "VXOR", "VLS", "VRS", "VARS",
    "VCMP", "VCLT", "VCGT", "VSEXT", "VRED",
)
LOAD_PREFIXES = ("LOAD", "OLOAD", "VLOAD", "VOLOAD")
STORE_PREFIXES = ("STORE", "OSTORE", "VSTORE", "VOSTORE")


FIELDS = [
    "benchmark", "function", "architecture",
    "normal_available", "normal_experiment", "normal_variant",
    "normal_compute_pe_util", "normal_compute_pe_util_percent",
    "normal_active_lane_util", "normal_active_lane_util_percent",
    "normal_ii", "normal_compute_util", "normal_compute_util_percent",
    "normal_scalar_eq_compute_ops", "normal_scalar_eq_ops_per_cycle",
    "normal_unroll_factor", "normal_mapping_file",
    "template_available", "template_family", "template", "template_transform",
    "template_source_shape", "template_semantic_compatible",
    "template_semantic_reason",
    "template_compute_pe_util", "template_compute_pe_util_percent",
    "template_active_lane_util", "template_active_lane_util_percent",
    "template_ii", "template_compute_util", "template_compute_util_percent",
    "template_scalar_eq_ops_per_cycle", "template_multihop",
    "template_result_class", "template_source_equivalent",
    "template_steady_state_verified", "template_verification_scope",
    "template_functional_verified", "template_verify_matches",
    "template_verify_mismatches", "template_source_file",
    "template_util_delta", "template_util_delta_percent_points",
    "template_beats_normal", "template_recovers_missing_normal",
    "template_adoptable", "winner", "adoption_reason",
]


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--normal-results", type=Path, default=DEFAULT_NORMAL)
  parser.add_argument("--template-results", type=Path, default=DEFAULT_TEMPLATES)
  parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
  return parser.parse_args()


def truth(value: object) -> bool:
  return str(value).strip().lower() in {"1", "true", "yes"}


def arch_width(arch: str) -> int:
  match = re.search(r"vec(\d+)", arch)
  return int(match.group(1)) if match else 1


def experiment_multihop(experiment: str) -> int | None:
  match = re.search(r"_h(\d+)__", experiment)
  return int(match.group(1)) if match else None


def eligible_template_row(row: dict[str, str]) -> bool:
  if not truth(row.get("semantic_compatible", "")):
    return False
  if row.get("result_class") == "functional_mismatch":
    return False
  if (
      row.get("result_family") == "phase_unrolled_window"
      and not truth(row.get("treg_audit_valid", ""))
  ):
    return False
  return True


def staged_dfg(mapping_file: str) -> Path | None:
  if not mapping_file:
    return None
  directory = Path(mapping_file).parent
  candidates = sorted(directory.glob("*_PartPredDFG.xml"))
  return candidates[0] if len(candidates) == 1 else None


def compute_node_lanes(path: Path) -> dict[int, int]:
  root = ET.fromstring(
      "<Root>" + path.read_text(encoding="utf-8") + "</Root>"
  )
  nodes: dict[int, int] = {}
  for node in root.findall("./DFG/Node"):
    op = (node.findtext("OP") or "").strip()
    if op.startswith(LOAD_PREFIXES) or op.startswith(STORE_PREFIXES):
      continue
    if not op.startswith(COMPUTE_PREFIXES):
      continue
    lanes = max(int(node.get("simd_lanes") or 1), 1) if op.startswith("V") else 1
    nodes[int(node.get("idx") or -1)] = lanes
  return nodes


def core_coords(directory: Path) -> set[tuple[int, int]]:
  candidates = sorted(directory.glob("*_arch_runtime.json"))
  if len(candidates) != 1:
    return {(x, y) for y in range(4) for x in range(1, 5)}
  data = json.loads(candidates[0].read_text(encoding="utf-8"))
  coords: set[tuple[int, int]] = set()
  for group in data.get("ARCH", {}).get("CGRA", {}).get("SUBMODS", []):
    for item in group.get("MODS", []):
      if item.get("MOD") == "PE":
        coords.add((int(item["X"]), int(item["Y"])))
  return coords or {(x, y) for y in range(4) for x in range(1, 5)}


def json_mapping_path(mapping: Path) -> Path | None:
  match = re.match(r"(.+?)_MTP=.*\.mappingwithlatency\.txt$", mapping.name)
  if match:
    candidate = mapping.parent / (match.group(1) + "mapping_i.txtmapping.json")
    if candidate.is_file():
      return candidate
  candidates = sorted(mapping.parent.glob("*mapping_i.txtmapping.json"))
  return candidates[-1] if candidates else None


def mapped_compute_occupancy(
    mapping: Path, compute_nodes: dict[int, int], ii: int, arch_simd_width: int,
) -> tuple[int, int]:
  placements: dict[int, tuple[int, int, int, bool]] = {}
  if mapping.stat().st_size == 0:
    nonempty = [
        path for path in mapping.parent.glob(f"*II={ii}*mappingwithlatency.txt")
        if path.stat().st_size > 0
    ]
    if nonempty:
      mapping = sorted(nonempty)[-1]
  if mapping.stat().st_size:
    cores = core_coords(mapping.parent)
    for line in mapping.read_text(encoding="utf-8", errors="ignore").splitlines():
      parts = line.split(",")
      if len(parts) != 4:
        continue
      try:
        node, x, y, latency = (int(value) for value in parts)
      except ValueError:
        continue
      placements[node] = (x, y, latency, (x, y) in cores)
  else:
    json_path = json_mapping_path(mapping)
    if json_path is None:
      return 0, 0
    data = json.loads(json_path.read_text(encoding="utf-8"))
    for pe_name, pe in data.get("CGRA_INS", {}).get("SUBMODS", {}).items():
      coords = re.search(r"X(\d+).*Y(\d+)", pe_name)
      if not coords:
        continue
      x, y = int(coords.group(1)), int(coords.group(2))
      is_core = "PE_MEM" not in pe_name and "DUMMY" not in pe_name

      def collect(value: object) -> None:
        if not isinstance(value, dict):
          return
        op_info = value.get("OP_INFO")
        if isinstance(op_info, dict):
          for slot, info in op_info.items():
            if not isinstance(info, dict) or not isinstance(info.get("id"), int):
              continue
            try:
              latency = int(slot)
            except ValueError:
              continue
            placements[int(info["id"])] = (x, y, latency, is_core)
        for child in value.values():
          if isinstance(child, dict):
            collect(child)

      collect(pe)

  occupied_lanes: dict[tuple[int, int, int], int] = {}
  for node, lanes in compute_nodes.items():
    placement = placements.get(node)
    if placement is None:
      continue
    x, y, latency, is_core = placement
    if not is_core:
      continue
    key = (x, y, latency % ii)
    occupied_lanes[key] = max(
        occupied_lanes.get(key, 0), min(lanes, arch_simd_width)
    )
  return len(occupied_lanes), sum(occupied_lanes.values())


def normal_inventory(path: Path, multihop: int | None = None) -> tuple[
    set[tuple[str, str, str]],
    dict[tuple[str, str, str], dict[str, object]],
]:
  inventory: set[tuple[str, str, str]] = set()
  best: dict[tuple[str, str, str], dict[str, object]] = {}
  compute_cache: dict[Path, dict[int, int]] = {}
  with path.open(encoding="utf-8", newline="") as stream:
    for row in csv.DictReader(stream):
      key = (row["benchmark"], row["function"], row["arch"])
      inventory.add(key)
      if multihop is not None and experiment_multihop(row["experiment"]) != multihop:
        continue
      if not row.get("achieved_ii") or not row.get("mapping_file"):
        continue
      dfg = staged_dfg(row["mapping_file"])
      if dfg is None or not dfg.is_file():
        continue
      if dfg not in compute_cache:
        compute_cache[dfg] = compute_node_lanes(dfg)
      ii = int(row["achieved_ii"])
      width = arch_width(row["arch"])
      occupied_slots, scalar_eq_slots = mapped_compute_occupancy(
          Path(row["mapping_file"]), compute_cache[dfg], ii, width
      )
      if occupied_slots == 0 and compute_cache[dfg]:
        continue
      pe_util = occupied_slots / (16 * ii)
      lane_util = (
          scalar_eq_slots / (occupied_slots * width) if occupied_slots else 0.0
      )
      util = scalar_eq_slots / (16 * width * ii)
      candidate: dict[str, object] = {
          "row": row,
          "ii": ii,
          "compute_ops": scalar_eq_slots,
          "pe_util": pe_util,
          "lane_util": lane_util,
          "util": util,
          "ops_per_cycle": scalar_eq_slots / ii,
      }
      current = best.get(key)
      if current is None or (util, -ii) > (
          float(current["util"]), -int(current["ii"])
      ):
        best[key] = candidate
  return inventory, best


def benchmark_lookup(
    inventory: set[tuple[str, str, str]],
) -> tuple[dict[str, set[str]], dict[str, set[str]], dict[str, set[str]]]:
  functions_by_benchmark: dict[str, set[str]] = defaultdict(set)
  benchmarks_by_function: dict[str, set[str]] = defaultdict(set)
  benchmarks_by_suite_name: dict[str, set[str]] = defaultdict(set)
  for benchmark, function, _arch in inventory:
    functions_by_benchmark[benchmark].add(function)
    benchmarks_by_function[function].add(benchmark)
    parts = benchmark.split("/")
    if len(parts) >= 2:
      benchmarks_by_suite_name[f"{parts[0]}__{parts[1]}"].add(benchmark)
  return functions_by_benchmark, benchmarks_by_function, benchmarks_by_suite_name


def resolve_template_key(
    row: dict[str, str],
    inventory: set[tuple[str, str, str]],
    functions_by_benchmark: dict[str, set[str]],
    benchmarks_by_function: dict[str, set[str]],
    benchmarks_by_suite_name: dict[str, set[str]],
) -> tuple[str, str, str] | None:
  benchmark = row["benchmark"]
  function = row["function"]
  arch = row["architecture"]
  if (benchmark, function, arch) in inventory:
    return benchmark, function, arch

  resolved_benchmark = ""
  if "__" in benchmark:
    parts = benchmark.split("__")
    if len(parts) >= 2:
      matches = benchmarks_by_suite_name.get(f"{parts[0]}__{parts[1]}", set())
      if len(matches) == 1:
        resolved_benchmark = next(iter(matches))
  if not resolved_benchmark:
    matches = benchmarks_by_function.get(function, set())
    if len(matches) == 1:
      resolved_benchmark = next(iter(matches))
  if not resolved_benchmark:
    return None

  functions = functions_by_benchmark.get(resolved_benchmark, set())
  resolved_function = function if function in functions else ""
  if not resolved_function and len(functions) == 1:
    resolved_function = next(iter(functions))
  key = (resolved_benchmark, resolved_function, arch)
  return key if key in inventory else None


def template_inventory(
    path: Path,
    inventory: set[tuple[str, str, str]],
) -> tuple[dict[tuple[str, str, str], dict[str, str]], int]:
  functions_by_benchmark, benchmarks_by_function, by_suite = benchmark_lookup(inventory)
  best: dict[tuple[str, str, str], dict[str, str]] = {}
  unresolved = 0
  with path.open(encoding="utf-8", newline="") as stream:
    for row in csv.DictReader(stream):
      if not eligible_template_row(row):
        continue
      key = resolve_template_key(
          row, inventory, functions_by_benchmark, benchmarks_by_function, by_suite
      )
      if key is None:
        unresolved += 1
        continue
      util = float(row["scalar_eq_compute_util"])
      ii = int(row["ii"])
      current = best.get(key)
      if current is None or (util, -ii) > (
          float(current["scalar_eq_compute_util"]), -int(current["ii"])
      ):
        best[key] = row
  return best, unresolved


def comparison_row(
    key: tuple[str, str, str],
    normal: dict[str, object] | None,
    template: dict[str, str] | None,
) -> dict[str, object]:
  benchmark, function, arch = key
  normal_util = float(normal["util"]) if normal else None
  template_util = float(template["scalar_eq_compute_util"]) if template else None
  beats = bool(
      normal_util is not None and template_util is not None
      and template_util > normal_util + 1e-12
  )
  recovers = normal is None and template is not None
  source_equivalent = bool(template and truth(template["source_equivalent"]))
  steady_state_verified = bool(
      template and truth(template.get("steady_state_verified", ""))
  )
  verified = bool(template and truth(template["functional_verified"]))
  adoptable = bool(template and source_equivalent and verified)
  use_template = adoptable and (beats or recovers)
  if use_template:
    winner = "verified_template"
    adoption_reason = "verified template improves compute utilization"
  elif normal:
    winner = "regular_dfg"
    if not template:
      adoption_reason = "no mapped template alternative"
    elif not adoptable:
      adoption_reason = "template is not source-equivalent and functionally verified"
    else:
      adoption_reason = "verified template does not improve compute utilization"
  else:
    winner = "none"
    adoption_reason = (
        "template mapping exists but is not source-equivalent and verified"
        if template else "neither normal nor template mapping succeeded"
    )

  normal_row = normal["row"] if normal else {}
  delta = (
      template_util - normal_util
      if template_util is not None and normal_util is not None else None
  )
  return {
      "benchmark": benchmark,
      "function": function,
      "architecture": arch,
      "normal_available": bool(normal),
      "normal_experiment": normal_row.get("experiment", ""),
      "normal_variant": normal_row.get("variant", ""),
      "normal_compute_pe_util": f"{normal['pe_util']:.6f}" if normal else "",
      "normal_compute_pe_util_percent": f"{100 * normal['pe_util']:.3f}" if normal else "",
      "normal_active_lane_util": f"{normal['lane_util']:.6f}" if normal else "",
      "normal_active_lane_util_percent": f"{100 * normal['lane_util']:.3f}" if normal else "",
      "normal_ii": normal["ii"] if normal else "",
      "normal_compute_util": f"{normal_util:.6f}" if normal_util is not None else "",
      "normal_compute_util_percent": f"{100 * normal_util:.3f}" if normal_util is not None else "",
      "normal_scalar_eq_compute_ops": normal["compute_ops"] if normal else "",
      "normal_scalar_eq_ops_per_cycle": f"{normal['ops_per_cycle']:.3f}" if normal else "",
      "normal_unroll_factor": normal_row.get("selected_unroll_factor", ""),
      "normal_mapping_file": normal_row.get("mapping_file", ""),
      "template_available": bool(template),
      "template_family": template["result_family"] if template else "",
      "template": template["template"] if template else "",
      "template_transform": template["transform"] if template else "",
      "template_source_shape": template["source_shape"] if template else "",
      "template_semantic_compatible": bool(template),
      "template_semantic_reason": template["semantic_reason"] if template else "",
      "template_compute_pe_util": template["compute_pe_util"] if template else "",
      "template_compute_pe_util_percent": template["compute_pe_util_percent"] if template else "",
      "template_active_lane_util": template["active_lane_util"] if template else "",
      "template_active_lane_util_percent": template["active_lane_util_percent"] if template else "",
      "template_ii": template["ii"] if template else "",
      "template_compute_util": template["compute_util"] if template else "",
      "template_compute_util_percent": template["compute_util_percent"] if template else "",
      "template_scalar_eq_ops_per_cycle": template["scalar_eq_ops_per_cycle"] if template else "",
      "template_multihop": template["multihop"] if template else "",
      "template_result_class": template["result_class"] if template else "",
      "template_source_equivalent": source_equivalent,
      "template_steady_state_verified": steady_state_verified,
      "template_verification_scope": template.get("verification_scope", "") if template else "",
      "template_functional_verified": verified,
      "template_verify_matches": template["verify_matches"] if template else "",
      "template_verify_mismatches": template["verify_mismatches"] if template else "",
      "template_source_file": template["source_file"] if template else "",
      "template_util_delta": f"{delta:.6f}" if delta is not None else "",
      "template_util_delta_percent_points": f"{100 * delta:.3f}" if delta is not None else "",
      "template_beats_normal": beats,
      "template_recovers_missing_normal": recovers,
      "template_adoptable": adoptable,
      "winner": winner,
      "adoption_reason": adoption_reason,
  }


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=FIELDS)
    writer.writeheader()
    writer.writerows(rows)


def main() -> int:
  args = parse_args()
  inventory, normal = normal_inventory(args.normal_results.resolve())
  templates, unresolved = template_inventory(args.template_results.resolve(), inventory)
  rows = [comparison_row(key, normal.get(key), templates.get(key)) for key in sorted(inventory)]
  improvements = [
      row for row in rows
      if row["template_adoptable"]
      and (row["template_beats_normal"] or row["template_recovers_missing_normal"])
  ]
  opportunities = [
      row for row in rows
      if row["template_available"] and not row["template_adoptable"]
      and (row["template_beats_normal"] or row["template_recovers_missing_normal"])
  ]
  output_dir = args.output_dir.resolve()
  write_csv(output_dir / "normal_vs_template.csv", rows)
  write_csv(output_dir / "template_improvements.csv", improvements)
  write_csv(output_dir / "template_opportunities_unverified.csv", opportunities)
  print(
      f"wrote {len(rows)} comparison rows, {len(improvements)} verified "
      f"template improvements, and {len(opportunities)} unverified opportunities "
      f"({unresolved} semantic template rows unresolved)"
  )
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
