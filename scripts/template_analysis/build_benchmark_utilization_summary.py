#!/usr/bin/env python3
"""Build one hop-separated original/source-bound/topology utilization CSV."""

from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.template_analysis.compare_template_results import normal_inventory  # noqa: E402


ROOT = REPO_ROOT / "generated" / "morpher_runs" / "template_results"
MAPPING_SUMMARY = ROOT / "template_mapping_summary.csv"
NORMAL_RESULTS = (
    REPO_ROOT / "generated" / "morpher_runs" / "arch_sweeps"
    / "edge_mem_arch_sweep" / "allrun.csv"
)
SOURCE_RESULTS = ROOT / "template_candidates.csv"
SOURCE_ATTEMPTS = ROOT / "template_attempts.csv"
TOPOLOGY_RESULTS = ROOT / "template_topology_results.csv"
MORPH_RESULTS = ROOT / "template_morph_opportunities.csv"
CONTRACT_AUDIT = ROOT / "polybench_source_contract_audit.csv"
OUTPUT = ROOT / "benchmark_utilization_summary.csv"
MULTIHOPS = (1, 4)

FIELDS = [
    "benchmark",
    "kernel",
    "architecture",
    "multihop",
    "original_dfg_util_percent",
    "source_bound_util_percent",
    "topology_based_util_percent",
    "source_bound_contract",
    "source_bound_basis",
]


def read(path: Path) -> list[dict[str, str]]:
  with path.open(encoding="utf-8", newline="") as stream:
    return list(csv.DictReader(stream))


def polybench_contracts(path: Path) -> dict[tuple[str, str], str]:
  grouped: dict[tuple[str, str], list[str]] = defaultdict(list)
  for row in read(path):
    grouped[(row["benchmark"], row["kernel"])].append(row["source_bound_status"])
  result = {}
  for key, statuses in grouped.items():
    if "no" in statuses:
      result[key] = "rejected"
    elif "conditional" in statuses:
      result[key] = "conditional"
    elif "transform" in statuses:
      result[key] = "transformed"
    else:
      result[key] = "direct"
  return result


class ResultKeyResolver:
  def __init__(self, keys: set[tuple[str, str, str]]):
    self.keys = keys
    self.functions_by_benchmark: dict[str, set[str]] = defaultdict(set)
    self.benchmarks_by_function: dict[str, set[str]] = defaultdict(set)
    self.benchmarks_by_suite: dict[str, set[str]] = defaultdict(set)
    for benchmark, function, _arch in keys:
      self.functions_by_benchmark[benchmark].add(function)
      self.benchmarks_by_function[function].add(benchmark)
      parts = benchmark.split("/")
      if len(parts) >= 2:
        self.benchmarks_by_suite[f"{parts[0]}__{parts[1]}"].add(benchmark)

  def resolve(self, row: dict[str, str]) -> tuple[str, str, str] | None:
    benchmark = row["benchmark"]
    function = row["function"]
    arch = row["architecture"]
    direct = (benchmark, function, arch)
    if direct in self.keys:
      return direct
    resolved_benchmark = ""
    parts = benchmark.split("__")
    if len(parts) >= 2:
      matches = self.benchmarks_by_suite.get(f"{parts[0]}__{parts[1]}", set())
      if len(matches) == 1:
        resolved_benchmark = next(iter(matches))
    if not resolved_benchmark:
      matches = self.benchmarks_by_function.get(function, set())
      if len(matches) == 1:
        resolved_benchmark = next(iter(matches))
    if not resolved_benchmark:
      return None
    functions = self.functions_by_benchmark[resolved_benchmark]
    resolved_function = function if function in functions else ""
    if not resolved_function and len(functions) == 1:
      resolved_function = next(iter(functions))
    key = (resolved_benchmark, resolved_function, arch)
    return key if key in self.keys else None


def best_template_by_hop(
    path: Path, resolver: ResultKeyResolver,
) -> dict[tuple[str, str, str, int], dict[str, str]]:
  best = {}
  for row in read(path):
    try:
      hop = int(row["multihop"])
      util = float(row["compute_util_percent"])
      ii = int(row["ii"])
    except (KeyError, TypeError, ValueError):
      continue
    if hop not in MULTIHOPS:
      continue
    key = resolver.resolve(row)
    if key is None:
      continue
    full_key = (*key, hop)
    current = best.get(full_key)
    if current is None or (util, -ii) > (
        float(current["compute_util_percent"]), -int(current["ii"])
    ):
      best[full_key] = row
  return best


def source_bound_value(
    mapped: dict[str, str] | None,
    contract: str,
    topology: dict[str, str] | None,
) -> tuple[str, str]:
  if mapped:
    if mapped.get("_implied_from_h1") == "True":
      return mapped["compute_util_percent"], "mapped_h1_feasible_at_h4"
    verified = mapped.get("steady_state_verified") == "True"
    if verified and contract == "conditional":
      basis = "mapped_source_phase_steady_state_verified"
    elif verified:
      basis = "mapped_steady_state_verified"
    else:
      basis = "mapped_source_artifact_unverified"
    return mapped["compute_util_percent"], basis
  if contract in {"direct", "transformed", "conditional"} and topology:
    return topology["compute_util_percent"], "topology_projection"
  return "", ""


def main() -> int:
  base_rows = read(MAPPING_SUMMARY)
  keys = {
      (row["benchmark"], row["function"], row["architecture"])
      for row in base_rows
  }
  resolver = ResultKeyResolver(keys)
  contracts = polybench_contracts(CONTRACT_AUDIT)
  source_results = best_template_by_hop(SOURCE_RESULTS, resolver)
  source_attempts = best_template_by_hop(SOURCE_ATTEMPTS, resolver)
  raw_topology = best_template_by_hop(TOPOLOGY_RESULTS, resolver)
  morph_topology = best_template_by_hop(MORPH_RESULTS, resolver)
  normal_results = {
      hop: normal_inventory(NORMAL_RESULTS, multihop=hop)[1]
      for hop in MULTIHOPS
  }

  rows = []
  for source in base_rows:
    key = (source["benchmark"], source["function"], source["architecture"])
    is_polybench = source["benchmark"].startswith("polybench/")
    contract = contracts.get((source["benchmark"], source["function"]), "")
    for hop in MULTIHOPS:
      normal = normal_results[hop].get(key)
      mapped = source_results.get((*key, hop))
      if is_polybench and contract in {"direct", "transformed", "conditional"}:
        attempt = source_attempts.get((*key, hop))
        if attempt and (
            mapped is None
            or (
                float(attempt["compute_util_percent"]), -int(attempt["ii"])
            ) > (
                float(mapped["compute_util_percent"]), -int(mapped["ii"])
            )
        ):
          mapped = attempt
        if mapped is None and hop == 4:
          h1_mapping = (
              source_results.get((*key, 1))
              or source_attempts.get((*key, 1))
          )
          if h1_mapping:
            mapped = dict(h1_mapping)
            mapped["_implied_from_h1"] = "True"
      topology = (
          raw_topology.get((*key, hop))
          if is_polybench else morph_topology.get((*key, hop))
      )
      source_util, source_basis = source_bound_value(mapped, contract, topology)
      topology_candidates = [
          value for value in (
              topology["compute_util_percent"] if topology else "",
              mapped["compute_util_percent"] if mapped else "",
          ) if value
      ]
      topology_util = (
          f"{max(float(value) for value in topology_candidates):.3f}"
          if topology_candidates else ""
      )
      row_contract = contract
      if not is_polybench and mapped:
        row_contract = mapped.get("result_class", "mapped_source")
      rows.append({
          "benchmark": source["benchmark"],
          "kernel": source["function"],
          "architecture": source["architecture"],
          "multihop": hop,
          "original_dfg_util_percent": (
              f"{100.0 * float(normal['util']):.3f}" if normal else ""
          ),
          "source_bound_util_percent": source_util,
          "topology_based_util_percent": topology_util,
          "source_bound_contract": row_contract,
          "source_bound_basis": source_basis,
      })
  with OUTPUT.open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=FIELDS)
    writer.writeheader()
    writer.writerows(rows)
  print(f"wrote {len(rows)} rows to {OUTPUT.relative_to(REPO_ROOT)}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
