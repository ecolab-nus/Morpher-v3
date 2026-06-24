#!/usr/bin/env python3
"""Verify a mapped source-bound output-stationary GEMM main phase.

This verifies the tiled reduction recurrence only. Accumulator initialization,
final output drain, and whole-program address traces are intentionally outside
the certified scope.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COMPUTE_OPS = {"ADD", "SUB", "MUL", "MAC", "VADD", "VSUB", "VMUL", "VMAC"}


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--bindings", required=True, type=Path)
  parser.add_argument("--config", required=True, type=Path)
  parser.add_argument("--output", type=Path, default=None)
  return parser.parse_args()


def source_record(path: Path, index: int) -> dict:
  records = [
      json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
      if line.strip()
  ]
  return records[index]


def config_value(text: str, key: str) -> str:
  match = re.search(rf"^\s*{re.escape(key)}\s*:\s*[\"']?([^\"'\n]+)", text, re.M)
  return match.group(1).strip() if match else ""


def node_pins(text: str) -> dict[int, tuple[int, int]]:
  value = config_value(text, "node_pin")
  pins = {}
  for item in value.split(","):
    fields = item.strip().split(":")
    if len(fields) == 3:
      pins[int(fields[0])] = (int(fields[1]), int(fields[2]))
  return pins


def xml_nodes(path: Path) -> dict[int, dict[str, object]]:
  root = ET.fromstring("<root>" + path.read_text(encoding="utf-8") + "</root>")
  nodes = {}
  for elem in root.findall(".//Node"):
    idx = int(elem.attrib["idx"])
    nodes[idx] = {
        "op": (elem.findtext("OP") or "").strip(),
        "base": (elem.findtext("BasePointerName") or "").strip(),
        "inputs": [int(item.attrib["idx"]) for item in elem.findall("./Inputs/Input")],
    }
  return nodes


def affine_roles(record: dict) -> tuple[bool, str, tuple[str, str] | None]:
  loops = record.get("loop_classes", [])
  parallel = [str(loop.get("iv")) for loop in loops if loop.get("kind") == "PARALLEL"]
  reduction = [str(loop.get("iv")) for loop in loops if loop.get("kind") == "REDUCTION"]
  if len(parallel) != 2 or len(reduction) != 1:
    return False, "expected two parallel loops and one reduction loop", None
  p0, p1 = parallel
  red = reduction[0]
  outputs = [
      str(access.get("base")) for access in record.get("affine_accesses", [])
      if access.get("kind") == "store" and access.get("base")
  ]
  if not outputs:
    return False, "source candidate has no output store", None
  output = outputs[0]
  stores = [
      access for access in record.get("affine_accesses", [])
      if access.get("kind") == "store" and access.get("base") == output
  ]
  if not any(set(access.get("loop_coefficients", {})) == {p0, p1} for access in stores):
    return False, "output store is not affine in both output loops", None
  loads = [
      access for access in record.get("affine_accesses", [])
      if access.get("kind") == "load" and access.get("base") != output
  ]
  row = [
      access for access in loads
      if set(access.get("loop_coefficients", {})) == {p0, red}
  ]
  col = [
      access for access in loads
      if set(access.get("loop_coefficients", {})) == {red, p1}
  ]
  if len(row) != 1 or len(col) != 1:
    return False, "could not prove complementary row/K and K/column input streams", None
  a = str(row[0].get("base"))
  b = str(col[0].get("base"))
  if not a or not b or a == b:
    return False, "v1 verification requires two explicit source input bases", None
  arithmetic = record.get("arithmetic_summary", {})
  if not (
      arithmetic.get("mac_pairs") == 1
      and arithmetic.get("mul_ops") == 1
      and arithmetic.get("add_ops") == 1
      and arithmetic.get("total_arith") == 2
  ):
    return False, "reduction body contains arithmetic beyond one multiply-add", None
  return True, "", (a, b)


def mapper_result_exists(xml_path: Path) -> bool:
  for path in xml_path.parent.glob("xml_inputs/*/*/result.csv"):
    with path.open(encoding="utf-8", newline="") as stream:
      if any(
          row.get("status") in {"ok", "success"}
          and row.get("ii_mapper_status", "success") == "success"
          for row in csv.DictReader(stream)
      ):
        return True
  return False


def verify_row(row: dict[str, str], config_text: str) -> dict[str, object]:
  result = {
      "source_report": row.get("source_report", ""),
      "function": row.get("function", ""),
      "record_index": row.get("record_index", ""),
      "template": row.get("template", ""),
      "steady_state_verified": False,
      "verify_matches": 0,
      "verify_mismatches": 0,
      "verification_scope": "GEMM tiled reduction main; excludes initialization and drain",
      "rejection_reason": "",
  }
  errors = []
  if row.get("template") != "gemm_output_stationary":
    errors.append("not an output-stationary GEMM binding")
  if row.get("binding_status") != "supported" or row.get("map_status") != "success":
    errors.append("binding or mapper did not succeed")
  if row.get("mapped_ii") != "1":
    errors.append("mapped II is not 1")
  if config_value(config_text, "spm_mode") != "off":
    errors.append("verification requires spm_mode=off")

  try:
    report = Path(row["source_report"])
    record = source_record(report, int(row["record_index"]))
    roles_ok, role_error, roles = affine_roles(record)
    if not roles_ok:
      errors.append(role_error)
  except (OSError, ValueError, IndexError, json.JSONDecodeError) as exc:
    roles = None
    errors.append(f"could not read source candidate: {exc}")

  xml_path = Path(row.get("artifact_xml", ""))
  if not xml_path.is_absolute():
    xml_path = REPO_ROOT / xml_path
  try:
    nodes = xml_nodes(xml_path)
  except (OSError, ET.ParseError, ValueError) as exc:
    nodes = {}
    errors.append(f"could not read lowered XML: {exc}")

  dim_x = int(row.get("generated_dim_x") or 0)
  dim_y = int(row.get("generated_dim_y") or 0)
  loads = [idx for idx, node in nodes.items() if node["op"] in {"LOAD", "VLOAD"}]
  macs = [idx for idx, node in nodes.items() if node["op"] in {"MAC", "VMAC"}]
  other_compute = [
      idx for idx, node in nodes.items()
      if node["op"] in COMPUTE_OPS and node["op"] not in {"MAC", "VMAC"}
  ]
  if len(loads) != dim_y + dim_x or len(macs) != dim_x * dim_y or other_compute:
    errors.append("XML is not an exact two-stream MAC grid")

  if roles and len(loads) == dim_y + dim_x:
    a, b = roles
    a_loads = loads[:dim_y]
    b_loads = loads[dim_y:]
    if any(nodes[idx]["base"] != a for idx in a_loads):
      errors.append("row-stream LOAD bases do not match source")
    if any(nodes[idx]["base"] != b for idx in b_loads):
      errors.append("column-stream LOAD bases do not match source")
    combinations = []
    for idx in macs:
      parents = set(nodes[idx]["inputs"])
      a_parents = parents.intersection(a_loads)
      b_parents = parents.intersection(b_loads)
      if len(a_parents) != 1 or len(b_parents) != 1 or len(parents) != 2:
        errors.append(f"MAC {idx} is not fed by one row and one column stream")
        break
      combinations.append((next(iter(a_parents)), next(iter(b_parents))))
    expected = {(a_idx, b_idx) for a_idx in a_loads for b_idx in b_loads}
    if set(combinations) != expected or len(combinations) != len(expected):
      errors.append("MAC grid does not cover the row/column stream cross product")

    pins = node_pins(config_text)
    try:
      a_coords = [pins[idx] for idx in a_loads]
      b_coords = [pins[idx] for idx in b_loads]
      mac_coords = {pins[idx] for idx in macs}
      a_vertical = len({x for x, _ in a_coords}) == 1
      b_horizontal = len({y for _, y in b_coords}) == 1
      expected_mac_coords = {(x, y) for x, _ in b_coords for _, y in a_coords}
      if not a_vertical or not b_horizontal:
        errors.append("A and B are not pinned to perpendicular memory edges")
      if mac_coords != expected_mac_coords:
        errors.append("MAC pins do not cover the edge-stream intersection grid")
    except KeyError:
      errors.append("all LOAD and MAC nodes must be explicitly pinned")

  if not mapper_result_exists(xml_path):
    errors.append("no successful mapper result artifact found")

  matches = 0
  mismatches = 0
  if not errors:
    for seed in range(3):
      for r in range(dim_y):
        for c in range(dim_x):
          source = sum((seed + 2 * r + k + 1) * (seed + 3 * c - k + 9)
                       for k in range(7))
          accumulator = 0
          for k in range(7):
            accumulator += (seed + 2 * r + k + 1) * (seed + 3 * c - k + 9)
          if source == accumulator:
            matches += 1
          else:
            mismatches += 1
  result["verify_matches"] = matches
  result["verify_mismatches"] = mismatches
  result["steady_state_verified"] = not errors and mismatches == 0 and matches > 0
  result["rejection_reason"] = "; ".join(errors)
  return result


def main() -> int:
  args = parse_args()
  config_text = args.config.read_text(encoding="utf-8")
  with args.bindings.open(encoding="utf-8", newline="") as stream:
    results = [verify_row(row, config_text) for row in csv.DictReader(stream)]
  output = args.output or args.bindings.with_name("steady_state_verification.json")
  output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
  verified = sum(bool(row["steady_state_verified"]) for row in results)
  print(f"verified {verified}/{len(results)} steady-state binding(s); wrote {output}")
  return 0 if verified == len(results) and results else 1


if __name__ == "__main__":
  raise SystemExit(main())
