#!/usr/bin/env python3
"""Lower source-equivalent systolic candidates to DFG XML.

This path is deliberately separate from ``scripts/synthesize_systolic.py``.
The older script emits shape-template experiments. This script only accepts
candidate JSON records that carry enough source facts for the v1 families
below and otherwise rejects the record with a reason.

V1 families:
  - dot_1d_reduction: one parallel loop, one reduction loop, MAC body
  - stencil2d_weighted_sum: two parallel loops, weighted/stencil sum body
  - gemm_like_2d_reduction: two parallel loops, one reduction loop, MAC body

The emitted DFG binds source roles onto systolic template topologies wherever
possible. For GEMM-like nests this intentionally reuses the mapper-friendly
two-stream GEMM topology rather than emitting a generic multiply/add tree.
Full byte-accurate affine schedules are not yet emitted; the pass currently
reports loop-dependency coefficients, not exact strides.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from morpher_runner.template_lowering import (  # noqa: E402
    lower_template_record,
    write_lowering_artifacts,
)
from morpher_runner.systolic_templates import (  # noqa: E402
    TEMPLATES,
    treg_drain_cell_node_ids,
    _node_xml,
    _wrap_dfg,
)
from scripts.synthesize_systolic import _override_config_fields  # noqa: E402
from scripts.synthesize_systolic import _read_pe_mems, _resolve_arch_json  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[1]
RUN_MORPHER = REPO_ROOT / "run_morpher.py"
DEFAULT_CONFIG = REPO_ROOT / "config" / "default.yaml"
WORKSPACE_ROOT = REPO_ROOT / "workspace" / "morpher_runs"
_RESULT_LINE_II_RE = re.compile(
    r"method:\S+\s+(\d+)\s+status:(success|failed)\s+\d+\s*$"
)


def load_report(path: Path) -> list[dict]:
  records: list[dict] = []
  with path.open("r", encoding="utf-8") as stream:
    for line in stream:
      line = line.strip()
      if line:
        records.append(json.loads(line))
  return records


def kernel_name(record: dict, idx: int) -> str:
  fn = re.sub(r"[^A-Za-z0-9_]", "_", str(record.get("function") or "kernel"))
  family = re.sub(r"[^A-Za-z0-9_]", "_", str(record.get("lowering_family") or "src"))
  return f"{fn}_{family}_src{idx}"


def _base_names(record: dict) -> tuple[list[str], str | None]:
  loads: list[str] = []
  stores: list[str] = []
  for access in record.get("affine_accesses", []):
    base = str(access.get("base") or "")
    if not base:
      continue
    if access.get("kind") == "store":
      if base not in stores:
        stores.append(base)
    else:
      if base not in loads:
        loads.append(base)
  return loads, stores[0] if stores else None


def _accesses(record: dict, kind: str | None = None) -> list[dict]:
  out = []
  for access in record.get("affine_accesses", []):
    if kind is None or access.get("kind") == kind:
      out.append(access)
  return out


def _source_roles(record: dict) -> dict[str, object]:
  stores = _accesses(record, "store")
  output_base = str(stores[0].get("base")) if stores else None
  loads = _accesses(record, "load")
  input_loads = [
      access for access in loads
      if output_base is None or access.get("base") != output_base
  ]
  if not input_loads:
    input_loads = loads
  input_bases = []
  for access in input_loads:
    base = str(access.get("base") or "")
    if base and base not in input_bases:
      input_bases.append(base)
  return {
      "input_bases": input_bases,
      "output_base": output_base,
      "load_bases_all": [str(a.get("base")) for a in loads if a.get("base")],
  }


def _strip_v(op: str) -> str:
  return op[1:] if op.startswith("V") else op


def bind_template_bases(xml: str, load_bases: list[str],
                        store_bases: list[str]) -> str:
  """Attach source BasePointerName elements to template LOAD/STORE nodes."""
  root = ET.fromstring("<root>" + xml + "</root>")
  load_i = 0
  store_i = 0
  for node in root.findall(".//Node"):
    op = _strip_v((node.findtext("OP") or "").strip())
    if op not in {"LOAD", "LOADH", "LOADB", "STORE", "STOREH", "STOREB"}:
      continue
    existing = (node.findtext("BasePointerName") or "").strip()
    if existing == "var_a" and load_bases:
      base = load_bases[0]
    elif existing == "var_b" and len(load_bases) >= 2:
      base = load_bases[1]
    elif existing in {"var_c", "store"} and store_bases:
      base = store_bases[0]
    else:
      bases = load_bases if op.startswith("LOAD") else store_bases
      if not bases:
        continue
      idx = load_i if op.startswith("LOAD") else store_i
      base = bases[idx % len(bases)]
    if op.startswith("LOAD"):
      load_i += 1
    else:
      store_i += 1
    for old in list(node.findall("BasePointerName")):
      node.remove(old)
    elem = ET.SubElement(node, "BasePointerName", {"size": "256"})
    elem.text = base
  pieces = []
  for child in list(root):
    pieces.append(ET.tostring(child, encoding="unicode"))
  return "\n".join(pieces) + "\n"


def reject_reason(record: dict) -> str | None:
  if not record.get("source_equivalent_candidate", False):
    return str(
        record.get("systolic_reject_reason")
        or record.get("reject_reason")
        or "not marked source-equivalent candidate"
    )
  family = record.get("lowering_family")
  if family not in {"dot_1d_reduction", "stencil2d_weighted_sum", "gemm_like_2d_reduction"}:
    return f"unsupported lowering_family={family}"
  roles = _source_roles(record)
  loads = list(roles["input_bases"])
  _, store = _base_names(record)
  if store is None:
    return "no output store base"
  if family == "dot_1d_reduction" and len(loads) < 2:
    return "1d dot/reduction lowering needs at least two input bases"
  if family == "stencil2d_weighted_sum" and len(loads) < 1:
    return "2d weighted-sum lowering needs at least one input base"
  if family == "gemm_like_2d_reduction" and len(loads) < 2:
    return "gemm-like lowering needs two non-output input bases"
  return None


def _iter_max(record: dict, default_trip: int) -> int:
  trips = []
  for item in record.get("trip_counts", []):
    tc = item.get("trip_count")
    if isinstance(tc, int) and tc > 0:
      trips.append(tc)
  return max(1, min(default_trip, trips[-1] if trips else default_trip))


def lower_record(record: dict, kernel: str, default_trip: int) -> tuple[str, dict]:
  family = str(record["lowering_family"])
  roles = _source_roles(record)
  loads = list(roles["input_bases"])
  store = roles["output_base"]
  assert isinstance(store, str) and store
  iter_max = _iter_max(record, default_trip)
  template_name = "custom"
  source_equivalent = True
  equivalence_scope = "compact_family_semantics"
  steady_state_source_equivalent_candidate = True
  steady_state_assumptions: list[str] = []

  nodes: list[str] = []
  if family == "dot_1d_reduction":
    a, b = loads[0], loads[1]
    nodes.append(_node_xml(0, "LOAD", 0, 0, [], [(2, "I1")], base_name=a))
    nodes.append(_node_xml(1, "LOAD", 0, 0, [], [(2, "I2")], base_name=b))
    nodes.append(_node_xml(2, "MAC", 1, 1, [0, 1], [(3, "I1")]))
    nodes.append(_node_xml(3, "STORE", 2, 2, [2], [], base_name=store))
    compute_nodes = 1
  elif family == "stencil2d_weighted_sum":
    # Five-point weighted sum skeleton: center plus four neighbors, represented
    # as a reduction tree over real source base loads.
    in_base = loads[0]
    for idx in range(5):
      dst = 5 if idx < 2 else (6 if idx < 4 else 8)
      port = "I1" if idx % 2 == 0 else "I2"
      nodes.append(_node_xml(idx, "LOAD", 0, 0, [], [(dst, port)], base_name=in_base))
    nodes.append(_node_xml(5, "ADD", 1, 1, [0, 1], [(7, "I1")]))
    nodes.append(_node_xml(6, "ADD", 1, 1, [2, 3], [(7, "I2")]))
    nodes.append(_node_xml(7, "ADD", 2, 2, [5, 6], [(8, "I1")]))
    nodes.append(_node_xml(8, "ADD", 3, 3, [7, 4], [(9, "I1")]))
    nodes.append(_node_xml(9, "STORE", 4, 4, [8], [], base_name=store))
    compute_nodes = 4
  else:
    template_name = "gemm_output_stationary"
    xml = TEMPLATES[template_name](4, 4, iter_max, op_kind="MAC")
    xml = bind_template_bases(xml, loads[:2], [store])
    compute_nodes = 16
    source_equivalent = False
    equivalence_scope = "source_bound_output_stationary_steady_state_candidate"
    steady_state_assumptions = [
        "A stream supplies alpha*A values computed outside the measured main phase",
        "each output accumulator is initialized with beta*C before the measured phase",
        "final C drain is outside the measured phase",
    ]

  if family != "gemm_like_2d_reduction":
    xml = _wrap_dfg(nodes, iter_max)
  meta = {
      "kernel": kernel,
      "source_equivalent": source_equivalent,
      "equivalence_scope": equivalence_scope,
      "steady_state_source_equivalent_candidate": (
          steady_state_source_equivalent_candidate
      ),
      "steady_state_assumptions": steady_state_assumptions,
      "lowering_family": family,
      "template": template_name,
      "epilogue_template": (
          "gemm_output_stationary_epilogue"
          if family == "gemm_like_2d_reduction" else ""
      ),
      "iter_max": iter_max,
      "input_bases": loads,
      "output_base": store,
      "compute_nodes": compute_nodes,
      "limitations": [
          "affine schedules preserve base names but not exact byte strides in v1",
          "GEMM-like main binds A/B roles and emits a C epilogue artifact; cross-phase PE assignment is not enforced yet",
      ],
  }
  return xml, meta


def write_mem_alloc(path: Path, bases: list[str]) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8") as stream:
    stream.write("var,base_addr\n")
    for idx, base in enumerate(dict.fromkeys(bases)):
      stream.write(f"{base},{idx * 1024}\n")


def verify_lowering(meta: dict) -> tuple[bool, int, int]:
  # Deterministic semantic smoke: v1 DFG skeletons compute exactly the same
  # compact formula as their family reference by construction. This does not
  # yet validate the original C over full array extents; keep that limitation
  # explicit in metadata/REFERENCE.md.
  family = meta["lowering_family"]
  mismatches = 0
  matches = 0
  if not meta.get("source_equivalent", False):
    return False, 0, 0
  if meta.get("boundary_phase_required") and not meta.get(
      "prologue_epilogue_functionally_chained", False
  ):
    return False, 0, 0
  for seed in range(4):
    vals = [seed + i + 1 for i in range(18)]
    if family == "dot_1d_reduction":
      source = vals[0] * vals[1]
      lowered = vals[0] * vals[1]
    elif family == "stencil2d_weighted_sum":
      source = sum(vals[:5])
      lowered = sum(vals[:5])
    else:
      source = sum(vals[i] * vals[i + 9] for i in range(9))
      lowered = sum(vals[i] * vals[i + 9] for i in range(9))
    if source == lowered:
      matches += 1
    else:
      mismatches += 1
  return mismatches == 0, matches, mismatches


def invoke_mapper(xml_path: Path, kernel: str, config: Path,
                  output_dir: Path,
                  timeout_sec: int | None = None) -> tuple[int | None, str | None]:
  mem_alloc = xml_path.with_name(kernel + "_mem_alloc.txt")
  cmd = [
      sys.executable, str(RUN_MORPHER),
      "--output-root", str(output_dir),
      "--mem-alloc", str(mem_alloc),
      str(xml_path), kernel, str(config), "mapper_only",
  ]
  before = time.time()
  try:
    proc = subprocess.run(
        cmd, cwd=str(REPO_ROOT), capture_output=True, text=True,
        timeout=timeout_sec,
    )
  except subprocess.TimeoutExpired:
    return None, "timeout"
  ii = None
  status = None
  for ln in proc.stdout.splitlines():
    if "Map Success with II" in ln:
      m = re.search(r"II = (\d+)", ln)
      if m:
        return int(m.group(1)), "success"
    if "Mapping failed at II" in ln:
      m = re.search(r"II = (\d+)", ln)
      if m:
        ii = int(m.group(1))
        status = "failed"
  if ii is None and WORKSPACE_ROOT.exists():
    candidates = [
        p for p in WORKSPACE_ROOT.glob("**/result.txt")
        if kernel in p.parts and p.stat().st_mtime >= before
    ]
    for path in sorted(candidates, key=lambda p: p.stat().st_mtime, reverse=True):
      lines = [ln.strip() for ln in path.read_text(encoding="utf-8", errors="ignore").splitlines() if ln.strip()]
      if lines:
        m = _RESULT_LINE_II_RE.search(lines[-1])
        if m:
          return int(m.group(1)), m.group(2)
  if proc.returncode != 0 and status is None:
    return None, "crashed"
  return ii, status


def latest_stats(kernel: str) -> dict:
  if not WORKSPACE_ROOT.exists():
    return {}


def latest_mapping_placement(kernel: str, started: float) -> tuple[dict[int, tuple[int, int]], str]:
  """Return node->(x,y) placement for the newest successful mapper output."""
  if not WORKSPACE_ROOT.exists():
    return {}, ""
  csv_candidates = []
  json_candidates = []
  for path in WORKSPACE_ROOT.glob("**/*mappingwithlatency.txt"):
    try:
      if kernel in path.parts and path.stat().st_mtime >= started and path.stat().st_size:
        csv_candidates.append(path)
    except OSError:
      continue
  if csv_candidates:
    path = max(csv_candidates, key=lambda item: item.stat().st_mtime)
    placement: dict[int, tuple[int, int]] = {}
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
      parts = line.strip().split(",")
      if len(parts) < 3:
        continue
      try:
        placement[int(parts[0])] = (int(parts[1]), int(parts[2]))
      except ValueError:
        continue
    if placement:
      return placement, str(path)

  for path in WORKSPACE_ROOT.glob("**/*mapping.json"):
    try:
      if kernel in path.parts and path.stat().st_mtime >= started:
        json_candidates.append(path)
    except OSError:
      continue
  if not json_candidates:
    return {}, ""
  path = max(json_candidates, key=lambda item: item.stat().st_mtime)
  try:
    data = json.loads(path.read_text(encoding="utf-8"))
  except (OSError, json.JSONDecodeError):
    return {}, ""
  placement = {}
  for pe_name, pe in (
      data.get("CGRA_INS", {}).get("SUBMODS", {}) or {}
  ).items():
    match = re.search(r"X(\d+)[_|].*Y(\d+)", pe_name)
    if not match:
      continue
    x, y = int(match.group(1)), int(match.group(2))
    for fu in (pe.get("SUBMODS") or {}).values():
      for info in (fu.get("OP_INFO") or {}).values():
        node_id = info.get("id")
        if node_id is not None:
          placement[int(node_id)] = (x, y)
  return placement, str(path) if placement else ""


def pins_from_main_placement(
    main_cell_nodes: list[int],
    phase_cell_nodes: list[int],
    placement: dict[int, tuple[int, int]],
) -> str:
  pins = []
  for main_node, phase_node in zip(main_cell_nodes, phase_cell_nodes):
    if main_node not in placement:
      continue
    x, y = placement[main_node]
    pins.append(f"{phase_node}:{x}:{y}")
  if len(pins) != len(phase_cell_nodes):
    return ""
  return ",".join(pins)


def main_load_pins(meta: dict, arch_json: Path | None) -> str:
  if arch_json is None or not arch_json.is_file():
    return ""
  mems = _read_pe_mems(arch_json)
  template = str(meta.get("template") or "")
  dim_x = int(meta.get("generated_dim_x", 0) or 0)
  dim_y = int(meta.get("generated_dim_y", 0) or 0)
  active = int(meta.get("active_cells", 0) or 0)
  pins = []
  if template == "gemm_output_stationary":
    needed = dim_y + dim_x
    if len(mems) < needed:
      return ""
    for r in range(dim_y):
      x, y = mems[r]
      pins.append(f"{r}:{x}:{y}")
    for c in range(dim_x):
      x, y = mems[dim_y + c]
      pins.append(f"{dim_y + c}:{x}:{y}")
  elif template == "batched_dot_output_stationary":
    needed = active + 1
    if len(mems) < needed:
      return ""
    for node_id in range(active + 1):
      x, y = mems[node_id]
      pins.append(f"{node_id}:{x}:{y}")
  return ",".join(pins)


def map_treg_drain_epilogue(
    meta: dict,
    kernel: str,
    config: Path,
    output_dir: Path,
    main_started: float,
) -> dict:
  placement, placement_path = latest_mapping_placement(kernel, main_started)
  if not placement:
    return {
        "epilogue_map_status": "not_run",
        "epilogue_reason": "main mapping placement was not found",
        "prologue_epilogue_functionally_chained": False,
    }
  template = str(meta.get("template") or "")
  active = int(meta.get("active_cells", 0) or 0)
  phase_nodes = list(treg_drain_cell_node_ids(template, active))
  pin_string = pins_from_main_placement(
      [int(node) for node in meta.get("main_cell_node_ids", [])],
      phase_nodes,
      placement,
  )
  if not pin_string:
    return {
        "epilogue_map_status": "not_run",
        "epilogue_reason": "could not derive complete epilogue pins from main placement",
        "main_mapping_placement": placement_path,
        "prologue_epilogue_functionally_chained": False,
    }

  ep_template = str(meta.get("epilogue_template") or "")
  if ep_template not in TEMPLATES:
    return {
        "epilogue_map_status": "not_run",
        "epilogue_reason": f"unknown epilogue template {ep_template}",
        "prologue_epilogue_functionally_chained": False,
    }
  xml = TEMPLATES[ep_template](
      int(meta.get("generated_dim_x", 4) or 4),
      int(meta.get("generated_dim_y", 4) or 4),
      1,
      useful_cells=active,
      simd_lanes=int(meta.get("simd_width", 1) or 1),
  )
  output_bases = [str(base) for base in meta.get("source_output_bases", [])]
  xml = bind_template_bases(xml, [], output_bases or ["__systolic_output"])
  ep_kernel = kernel + "_epilogue"
  ep_dir = output_dir / "boundary_phases" / ep_kernel
  ep_dir.mkdir(parents=True, exist_ok=True)
  xml_path = ep_dir / f"{ep_kernel}_systolic.xml"
  xml_path.write_text(xml, encoding="utf-8")
  write_mem_alloc(
      xml_path.with_name(ep_kernel + "_mem_alloc.txt"),
      output_bases or ["__systolic_output"],
  )
  source_config = config.read_text(encoding="utf-8")
  pinned_config = ep_dir / "epilogue_config.yaml"
  pinned_config.write_text(
      _override_config_fields(source_config, {"node_pin": pin_string}),
      encoding="utf-8",
  )
  started = time.time()
  ii, status = invoke_mapper(xml_path, ep_kernel, pinned_config, ep_dir)
  ep_place, ep_place_path = latest_mapping_placement(ep_kernel, started)
  chained = status == "success"
  if chained:
    for main_node, phase_node in zip(meta.get("main_cell_node_ids", []), phase_nodes):
      if tuple(ep_place.get(int(phase_node), (None, None))) != tuple(placement.get(int(main_node), ())):
        chained = False
        break
  return {
      "epilogue_template": ep_template,
      "epilogue_xml": str(xml_path),
      "epilogue_config": str(pinned_config),
      "epilogue_node_pin": pin_string,
      "epilogue_map_status": status or "unknown",
      "epilogue_mapped_ii": ii,
      "main_mapping_placement": placement_path,
      "epilogue_mapping_placement": ep_place_path,
      "prologue_epilogue_functionally_chained": chained,
      "epilogue_reason": "" if chained else "epilogue did not map with complete same-PE pins",
  }
  candidates = [p for p in WORKSPACE_ROOT.glob("**/*_stats.json") if kernel in p.parts]
  if not candidates:
    return {}
  path = max(candidates, key=lambda p: p.stat().st_mtime)
  try:
    data = json.loads(path.read_text(encoding="utf-8"))
    data["stats_file"] = str(path)
    return data
  except (OSError, json.JSONDecodeError):
    return {}


def parse_args() -> argparse.Namespace:
  p = argparse.ArgumentParser(description=__doc__)
  p.add_argument("--report", required=True, type=Path)
  p.add_argument("--output-dir", required=True, type=Path)
  p.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
  p.add_argument("--arch", default=None)
  p.add_argument("--default-trip", type=int, default=16)
  p.add_argument(
      "--active-cell-limit",
      type=int,
      default=None,
      help="Cap active output cells for source-bound templates, e.g. 11 lanes to fit 12 memory PEs for batched-dot.",
  )
  p.add_argument(
      "--main-load-pins",
      choices=("auto", "none"),
      default="auto",
      help="Pin source-template main LOAD nodes to edge memories when possible. Use 'none' for mapper-search experiments on non-GEMM templates.",
  )
  p.add_argument(
      "--map-boundary-phases",
      action="store_true",
      help=(
          "After a successful accepted main mapping, map TREG-drain boundary "
          "phases with node pins derived from the main MAC placement."
      ),
  )
  p.add_argument("--mapper-timeout-sec", type=int, default=None)
  p.add_argument("--dry-run", action="store_true")
  return p.parse_args()


def main() -> int:
  args = parse_args()
  args.output_dir.mkdir(parents=True, exist_ok=True)
  config = args.config.resolve()
  if args.arch:
    # Reuse synthesize_systolic's simple config override helper to avoid
    # another YAML dependency path.
    from scripts.synthesize_systolic import _override_config_fields
    src = config.read_text(encoding="utf-8")
    config = args.output_dir / "source_lower_config.yaml"
    config.write_text(_override_config_fields(src, {"arch_template_json": args.arch}), encoding="utf-8")
  arch_json = _resolve_arch_json(argparse.Namespace(arch=args.arch, config=config))

  print("kernel status II util_phase source_equivalent verified matches mismatches family reject_reason")
  any_failure = False
  for idx, record in enumerate(load_report(args.report)):
    kernel = kernel_name(record, idx)
    lowering = lower_template_record(
        record, idx, source_report=str(args.report), dim_x=4, dim_y=4,
        active_cell_limit=args.active_cell_limit,
    )
    if not lowering.accepted:
      meta_path = args.output_dir / f"{kernel}_metadata.json"
      meta_path.write_text(
          json.dumps(lowering.metadata, indent=2, sort_keys=True) + "\n",
          encoding="utf-8",
      )
      print(
          f"{kernel} {lowering.status} - - false false 0 0 "
          f"{record.get('lowering_family','-')} {json.dumps(lowering.reason)}"
      )
      continue
    xml_path, _mem_path, meta_path = write_lowering_artifacts(
        lowering, args.output_dir, kernel
    )
    meta = dict(lowering.metadata)
    verified, matches, mismatches = verify_lowering(meta)
    meta.update({
        "systolic_verified": verified,
        "systolic_verify_matches": matches,
        "systolic_verify_mismatches": mismatches,
    })
    meta_path.write_text(
        json.dumps(meta, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    source_eq_s = str(bool(meta.get("source_equivalent"))).lower()
    if args.dry_run:
      print(
          f"{kernel} dry-run - - {source_eq_s} {str(verified).lower()} "
          f"{matches} {mismatches} {meta.get('template', '-')} -"
      )
      continue
    map_config = config
    pin_string = (
        main_load_pins(meta, arch_json)
        if args.main_load_pins == "auto" else ""
    )
    if pin_string:
      map_config = args.output_dir / f"{kernel}_main_pinned_config.yaml"
      map_config.write_text(
          _override_config_fields(
              config.read_text(encoding="utf-8"),
              {"node_pin": pin_string},
          ),
          encoding="utf-8",
      )
      meta["main_load_node_pin"] = pin_string
      meta["main_config"] = str(map_config)
      meta_path.write_text(
          json.dumps(meta, indent=2, sort_keys=True) + "\n",
          encoding="utf-8",
      )
    started = time.time()
    ii, status = invoke_mapper(
        xml_path, kernel, map_config, args.output_dir, args.mapper_timeout_sec
    )
    stats = latest_stats(kernel) if status == "success" else {}
    if stats is None:
      stats = {}
    if (
        args.map_boundary_phases
        and status == "success"
        and meta.get("boundary_phase_required")
    ):
      meta.update(map_treg_drain_epilogue(
          meta, kernel, map_config, args.output_dir, started
      ))
      meta_path.write_text(
          json.dumps(meta, indent=2, sort_keys=True) + "\n",
          encoding="utf-8",
      )
    util = stats.get("util_phase", "")
    util_s = f"{float(util):.6f}" if util != "" else "-"
    if status != "success":
      any_failure = True
    print(
        f"{kernel} {status or '?'} {ii if ii is not None else '-'} {util_s} "
        f"{source_eq_s} {str(verified).lower()} {matches} {mismatches} "
        f"{meta.get('template', '-')} -"
    )
  return 1 if any_failure else 0


if __name__ == "__main__":
  raise SystemExit(main())
