#!/usr/bin/env python3
"""Synthesize a systolic-shaped DFG XML from a SystolicCandidatePass JSON
record, invoke the existing mapper via run_morpher.py, and report measured II
and a post-hoc utilization estimate.

Usage:
  scripts/synthesize_systolic.py \
      --report <SystolicCandidatePass JSON output file> \
      --output-dir <where to write synthesized XMLs> \
      [--config config/scalar.yaml] \
      [--dim-x 4] [--dim-y 4] \
      [--default-trip 16] \
      [--only-verdict yes,maybe] \
      [--template gemm_fanout|gemm_chain|matvec_row|stencil_2d] \
      [--op-kind ADD|MAC] \
      [--dry-run]

Honest notes (see REFERENCE.md):
- The synthesized DFG is structurally representative, not functionally
  equivalent to the source kernel. Op identity is flexible; topology is what
  matters.
- Output-stationary prologue (per-cell accumulator zero-init) is opt-in via
  --emit-prologue. Epilogue (output drain) is not yet synthesized — for the
  current main templates, useful cells store their accumulator value every
  iter, so an epilogue is structurally redundant. See REFERENCE.md.
- Default trip count of 16 is used when SCEV could not resolve a loop trip.
- Utilization here is post-hoc: useful_cells / (PE_total * II * iter).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

def time_now() -> float:
  return time.time()

# Make morpher_runner importable.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from morpher_runner.systolic_templates import (
    TEMPLATES, TEMPLATE_DEFAULT_CELLS, PROLOGUE_FOR_TEMPLATE,
    EPILOGUE_FOR_TEMPLATE, pick_template,
)  # noqa: E402
from morpher_runner.systolic_trace import emit_artifacts  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[1]
RUN_MORPHER = REPO_ROOT / "run_morpher.py"
DEFAULT_CONFIG = REPO_ROOT / "config" / "default.yaml"
WORKSPACE_ROOT = REPO_ROOT / "workspace" / "morpher_runs"

_RESULT_LINE_II_RE = re.compile(
    r"method:\S+\s+(\d+)\s+status:(success|failed)\s+\d+\s*$"
)


def parse_args() -> argparse.Namespace:
  p = argparse.ArgumentParser(description=__doc__,
                              formatter_class=argparse.RawDescriptionHelpFormatter)
  p.add_argument("--report", required=True, type=Path,
                 help="JSON output of SystolicCandidatePass (one record per line)")
  p.add_argument("--output-dir", required=True, type=Path,
                 help="Directory for synthesized XMLs and per-kernel mapper output")
  p.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                 help="Morpher config YAML (memory_mode=static recommended)")
  p.add_argument("--dim-x", type=int, default=4)
  p.add_argument("--dim-y", type=int, default=4)
  p.add_argument("--default-trip", type=int, default=16,
                 help="Default loop trip when SCEV could not resolve")
  p.add_argument("--only-verdict",
                 default="yes,maybe",
                 help="Comma-separated verdicts to synthesize (e.g., 'yes' to skip maybes)")
  p.add_argument("--template",
                 choices=sorted(TEMPLATES.keys()),
                 default=None,
                 help="Override the auto-picked template")
  p.add_argument("--op-kind", default="auto",
                 choices=["auto", "ADD", "MAC"],
                 help="Compute-node op identity. 'auto' (default) selects MAC when "
                      "the candidate has mac_pairs>0 (needs arch advertising MAC), "
                      "else ADD. Explicit ADD/MAC forces the choice.")
  p.add_argument("--useful-cells", type=int, default=0,
                 help="Override the number of MAC cells the template emits (rest are NOPs). 0 = template default.")
  p.add_argument("--fill-factor", type=float, default=-1.0,
                 help="Shape-aware fill factor in (0,1]. Default -1.0 means 'auto': use "
                      "kernel_shape_factor from the SystolicCandidatePass JSON, which "
                      "detects triangular loops via SCEV. Explicit positive value overrides.")
  p.add_argument("--prefer-parallel-matvec", action="store_true",
                 help="For 1-parallel/1-reduction kernels, route to matvec_parallel (fills the "
                      "4x4 grid by tiling) instead of matvec_row (one row, honest 0.25 util).")
  p.add_argument("--arch", default=None,
                 help="Override arch_template_json from the config (e.g. edge_mem_4x4/scalar_2side.json)")
  p.add_argument("--simd-lanes", type=int, default=0,
                 help="Override the SIMD lane width used by templates. 0 (default) "
                      "auto-detects from the resolved arch JSON's SIMD_WIDTH "
                      "(scalar archs default to 1). When >1, templates emit "
                      "V-prefixed ops (VLOAD/VMAC/VADD/VSTORE) and tag every "
                      "node with `simd_packed=true simd_lanes=N`. Use 1 to force "
                      "scalar ops even on a vec arch (the scalar fallback path).")
  p.add_argument("--pin-loads",
                 choices=["none", "first", "all"], default="none",
                 help="LOAD-node pinning strategy. 'first' (recommended) pins only LOAD_0 "
                      "to a deterministic top-left PE_MEM as a placement seed for the SA "
                      "optimizer — reduces run-to-run variance without over-constraining "
                      "the rest of the layout. 'all' pins every LOAD to a PE_MEM in the "
                      "arch's natural side order (left-col, then right-col, then top/"
                      "bottom rows). 'all' is honest about how a real systolic layout "
                      "would look but often over-constrains the mapper for templates "
                      "that fan-out broadly (e.g. gemm_fanout where the SA prefers "
                      "spreading LOADs across multiple edges). 'none' (default) disables "
                      "pinning. See REFERENCE.md for the trade-off.")
  p.add_argument("--dataflow",
                 choices=["streaming", "output_stationary", "weight_stationary"],
                 default="streaming",
                 help="Dataflow style for the main DFG. 'streaming' (default) "
                      "uses the auto-picked template (gemm_fanout / gemm_chain "
                      "/ matvec_*) which stores outputs per iter. "
                      "'output_stationary' routes to gemm_output_stationary: "
                      "2x loads/cycle (A and B), W*H MACs accumulating in TREG, "
                      "and NO per-iter stores — output drain happens via "
                      "--emit-epilogue. 'weight_stationary' routes to "
                      "gemm_weight_stationary: weights held in TREG (loaded by "
                      "--emit-prologue), input loads stream in, outputs stream "
                      "out per cycle. Overrides --template.")
  p.add_argument("--emit-prologue", action="store_true",
                 help="Also synthesize and map a one-shot prologue DFG. For "
                      "output_stationary / streaming, the prologue zero-inits "
                      "per-cell accumulator memory; for weight_stationary, the "
                      "prologue loads weights from var_b into per-cell "
                      "stationary slots. The prologue is written next to the "
                      "main DFG as <kernel>_prologue_systolic.xml and mapped "
                      "separately. Per-DFG stats (compute_ops, util, II) are "
                      "emitted via EmitMappingStats — read them via "
                      "scripts/aggregate_mapping_stats.py. See REFERENCE.md "
                      "for current limitations (the prologue clears MEMORY, "
                      "not TREG, so hardware correctness still depends on a "
                      "future cross-phase PE assignment).")
  p.add_argument("--emit-epilogue", action="store_true",
                 help="Also synthesize and map a one-shot epilogue DFG that "
                      "drains per-cell accumulators to memory. Only meaningful "
                      "for output_stationary (the streaming + weight_stationary "
                      "mains already store per iter). Same stats / chaining "
                      "caveats as --emit-prologue.")
  p.add_argument("--dry-run", action="store_true",
                 help="Synthesize XMLs only; do not invoke the mapper")
  p.add_argument("--run-sim", action="store_true",
                 help="After mapping, generate a reference trace and run the "
                      "artifact_simulator. Reports Matches/Mismatches for each "
                      "synthesized DFG.")
  return p.parse_args()


def load_report(path: Path) -> list:
  """Read the SystolicCandidatePass JSON report. Each line is one JSON object."""
  records = []
  with open(path, "r", encoding="utf-8") as f:
    for ln in f:
      ln = ln.strip()
      if not ln:
        continue
      try:
        records.append(json.loads(ln))
      except json.JSONDecodeError as e:
        sys.stderr.write("warn: skipping malformed line: %s\n" % e)
  return records


def compute_iter_max(record: dict, default_trip: int) -> int:
  """Steady-state iteration count for the main DFG.

  Use the product of reduction trip counts when known; otherwise fall back to
  default_trip. Trip counts aren't in the JSON yet; we use default_trip as
  baseline (see Phase-4 limitation 4 in REFERENCE.md).
  """
  # The current JSON does not carry trips; this is the conservative default.
  reduction_count = sum(
      1 for c in record.get("loop_classes", []) if c.get("kind") == "REDUCTION"
  )
  if reduction_count == 0:
    return default_trip
  return default_trip ** min(reduction_count, 2)


def synthesize(record: dict, args: argparse.Namespace,
               simd_lanes: int = 1) -> tuple[str, dict]:
  """Pick a template and emit XML. Returns (xml_string, metadata_dict).

  simd_lanes: vector width per node. 1 → emit scalar LOAD/ADD/MAC/STORE.
  >1 → emit V-prefixed ops with simd_packed=true attribute.
  """
  loop_classes = record.get("loop_classes", [])
  mac_pairs = record.get("mac_pairs", 0)
  stencil_pairs = record.get("stencil_pairs", 0)

  # --dataflow overrides the auto-picked template family. When set to
  # output_stationary or weight_stationary, we ignore the streaming heuristic.
  if args.dataflow == "output_stationary":
    tname = "gemm_output_stationary"
  elif args.dataflow == "weight_stationary":
    tname = "gemm_weight_stationary"
  else:
    tname = args.template or pick_template(loop_classes, mac_pairs, stencil_pairs)
    if tname == "matvec_row" and args.prefer_parallel_matvec:
      tname = "matvec_parallel"
  if tname is None:
    return None, {
        "template": "-",
        "skip_reason": "not MAC-shape (mac_pairs=%d, stencil_pairs=%d)" %
                       (mac_pairs, stencil_pairs),
        "dim_x": args.dim_x,
        "dim_y": args.dim_y,
        "useful_cells": 0,
        "iter_max": 0,
        "op_kind": args.op_kind,
    }
  fn = TEMPLATES[tname]
  iter_max = compute_iter_max(record, args.default_trip)

  # Resolve op_kind from 'auto': MAC when candidate has mac_pairs > 0.
  if args.op_kind == "auto":
    op_kind = "MAC" if mac_pairs > 0 else "ADD"
  else:
    op_kind = args.op_kind

  # When --useful-cells is not given, ask the template how many compute
  # cells it emits by default. gemm_fanout / gemm_chain use the full
  # dim_x*dim_y grid; matvec_row only uses a single row.
  default_cell_count = TEMPLATE_DEFAULT_CELLS[tname](args.dim_x, args.dim_y)
  # Resolve fill_factor: explicit user value, else auto from JSON's
  # kernel_shape_factor (triangular kernels carry 0.5).
  if args.fill_factor >= 0.0:
    fill_factor = args.fill_factor
  else:
    fill_factor = float(record.get("kernel_shape_factor", 1.0))
  if args.useful_cells > 0:
    useful_cells = args.useful_cells
  elif fill_factor < 1.0:
    useful_cells = max(1, int(round(default_cell_count * fill_factor)))
  else:
    useful_cells = default_cell_count
  useful_cells = max(1, min(useful_cells, default_cell_count))
  xml = fn(args.dim_x, args.dim_y, iter_max,
           op_kind=op_kind, useful_cells=useful_cells,
           simd_lanes=simd_lanes)
  meta = {
      "template": tname,
      "iter_max": iter_max,
      "dim_x": args.dim_x,
      "dim_y": args.dim_y,
      "op_kind": op_kind,
      "useful_cells": useful_cells,
      "template_max_cells": default_cell_count,
      "nops_in_dfg": max(0, default_cell_count - useful_cells),
      "fill_factor": fill_factor,
      "triangular_loops": record.get("triangular_loops", 0),
      "simd_lanes": simd_lanes,
  }
  return xml, meta


def kernel_name(record: dict, idx: int) -> str:
  fn = record.get("function") or "unknown"
  # sanitize: alphanum + underscore
  fn = re.sub(r"[^A-Za-z0-9_]", "_", fn)
  return "%s_synth%d" % (fn, idx)


def write_xml(path: Path, xml: str) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with open(path, "w", encoding="utf-8") as f:
    f.write(xml)


def write_mem_alloc(path: Path) -> None:
  """Write a minimal mem_alloc.txt with three distinct bases for A, B, C.
  The mapper's auto-memory step needs one entry per accessed variable; our
  synthesized DFGs use anonymous loads/stores so we provide generic bases at
  well-spaced offsets.
  """
  path.parent.mkdir(parents=True, exist_ok=True)
  with open(path, "w", encoding="utf-8") as f:
    f.write("var_a,0\nvar_b,1024\nvar_c,2048\n")


def invoke_mapper(xml_path: Path, kernel: str, config: Path,
                  output_dir: Path) -> tuple[int | None, str | None]:
  """Run run_morpher.py in XML mode and parse achieved II.

  Runs from REPO_ROOT so the mapper's arch-template lookup resolves; pipes
  artifacts into output_dir via run_morpher.py's --output-root flag.
  A minimal mem_alloc.txt is auto-staged next to the XML (needed for the
  default memory_mode=auto configs).
  """
  mem_alloc = xml_path.with_name(kernel + "_mem_alloc.txt")
  if not mem_alloc.exists():
    write_mem_alloc(mem_alloc)
  cmd = [
      sys.executable, str(RUN_MORPHER),
      "--output-root", str(output_dir),
      "--mem-alloc", str(mem_alloc),
      str(xml_path), kernel, str(config), "mapper_only",
  ]
  before = time_now()
  proc = subprocess.run(cmd, cwd=str(REPO_ROOT),
                        capture_output=True, text=True)
  stdout = proc.stdout
  stderr = proc.stderr

  # II appears in result.txt under the staged mapper kernel dir. The runner
  # logs the staged path. Easiest: scan stdout for "II = <N>" lines from the
  # mapper itself.
  ii = None
  status = None
  for ln in stdout.splitlines():
    if "Map Success with II" in ln:
      m = re.search(r"II = (\d+)", ln)
      if m:
        ii = int(m.group(1))
        status = "success"
        break
    if "Mapping failed at II" in ln:
      m = re.search(r"II = (\d+)", ln)
      if m:
        ii = int(m.group(1))
        status = "failed"
        break

  # Fallback: scan the workspace dir for the freshly-written result.txt
  # belonging to this kernel. run_morpher.py writes artifacts under
  # <repo>/workspace/morpher_runs/<timestamp>/mapper/<subfolder>/<kernel-path>.
  if ii is None:
    candidates = []
    if WORKSPACE_ROOT.exists():
      for rt in WORKSPACE_ROOT.glob("**/result.txt"):
        try:
          if rt.stat().st_mtime < before:
            continue
        except OSError:
          continue
        if kernel not in str(rt):
          continue
        candidates.append(rt)
    for rt in sorted(candidates, key=lambda p: p.stat().st_mtime,
                     reverse=True):
      with open(rt, "r", encoding="utf-8", errors="ignore") as f:
        last = [l.strip() for l in f if l.strip()]
      if not last:
        continue
      m = _RESULT_LINE_II_RE.search(last[-1])
      if m:
        ii = int(m.group(1))
        status = m.group(2)
        break

  if proc.returncode != 0 and status is None:
    # Mapper aborted (e.g. assertion) before printing a result line. Mark
    # this as 'crashed' so the caller can fall back to a non-pinned retry.
    status = "crashed"
    sys.stderr.write("mapper stdout for %s:\n%s\n" % (kernel, stdout[-2000:]))
    sys.stderr.write("mapper stderr for %s:\n%s\n" % (kernel, stderr[-2000:]))

  return ii, status


def _kernel_dir_matches(path: Path, kernel: str) -> bool:
  """The workspace path always nests artifacts under a directory equal to the
  kernel name (run_morpher.py uses `context.mapper_kernel`). Match on the
  exact directory component to avoid `mvt_synth1` mis-matching `mvt_synth0`
  or vice versa from substring-glob aliasing.
  """
  return kernel in path.parts


def find_mapping_file(kernel: str) -> Path | None:
  if not WORKSPACE_ROOT.exists():
    return None
  # Some mapper runs emit a 0-byte *mappingwithlatency.txt and a sibling
  # *mappingwithlatency_i.txtmapping.json — the simulator's C++ parser
  # handles the JSON fallback when given the empty .txt path. So we accept
  # zero-byte files here as long as we can find one tagged with the kernel.
  candidates = []
  for path in WORKSPACE_ROOT.glob("**/*mappingwithlatency.txt"):
    if _kernel_dir_matches(path, kernel):
      candidates.append(path)
  if not candidates:
    return None
  return max(candidates, key=lambda p: p.stat().st_mtime)


def find_staged_dfg(kernel: str) -> Path | None:
  if not WORKSPACE_ROOT.exists():
    return None
  candidates = []
  for path in WORKSPACE_ROOT.glob("**/%s_PartPredDFG.xml" % kernel):
    if _kernel_dir_matches(path, kernel):
      candidates.append(path)
  if not candidates:
    return None
  return max(candidates, key=lambda p: p.stat().st_mtime)


def run_simulator(kernel: str, xml_path: Path, output_dir: Path
                  ) -> tuple[int | None, int | None]:
  """Generate a reference trace from the synthesized DFG semantics, then
  invoke artifact_simulator. Returns (matches, mismatches) or (None, None)
  on invocation failure.
  """
  sim_bin = REPO_ROOT / "simulator" / "artifact_simulator" / "build" / "artifact_simulator"
  if not sim_bin.is_file():
    sim_bin = REPO_ROOT / "simulator" / "artifact_simulator" / "build" / "src" / "artifact_simulator"
  if not sim_bin.is_file():
    return None, None

  staged_dfg = find_staged_dfg(kernel)
  mapping = find_mapping_file(kernel)
  if staged_dfg is None or mapping is None:
    return None, None
  # Generate reference trace from the *synthesized* XML semantics.
  sim_dir = output_dir / "sim" / kernel
  sim_dir.mkdir(parents=True, exist_ok=True)
  artifacts = emit_artifacts(str(xml_path), str(sim_dir), kernel)

  # Use the functional (non-arch-aware) sim path: it executes the DFG given
  # the mapping and trace, computes matches/mismatches against the reference
  # post values. The arch-aware path additionally validates inter-PE hop
  # timing against the mapping schedule and diverges from the mapper's own
  # accounting on multi-side memory archs (we hit "needs N hops but only M
  # cycles available" false positives). Functional path is what we want.
  cmd = [
      str(sim_bin),
      "--dfg", str(staged_dfg),
      "--mapping", str(mapping),
      "--mem-alloc", artifacts.mem_alloc_path,
      "--trace", artifacts.trace_path,
      "--addr-trace", artifacts.addr_trace_path,
  ]
  proc = subprocess.run(cmd, capture_output=True, text=True)
  m = re.search(r"Simulation Result:\s*Matches::\s*(\d+)\s*,\s*Mismatches::\s*(\d+)",
                proc.stdout)
  if not m:
    sys.stderr.write("artifact_simulator stdout for %s:\n%s\n%s\n"
                     % (kernel, proc.stdout[-2000:], proc.stderr[-1500:]))
    return None, None
  return int(m.group(1)), int(m.group(2))


def utilization(meta: dict, ii: int | None) -> float | None:
  """Compute-grid utilization: fraction of the dim_x*dim_y array doing useful
  MAC work each cycle. Excludes edge memory PEs (LOADs/STOREs live there but
  are infrastructure, not compute).
  - 1.000 means every compute PE does a MAC every II cycles.
  - matvec_row on 4x4 with 4 useful cells → 4 / (16 * 1) = 0.25.
  """
  if ii is None or ii <= 0:
    return None
  pe_total = meta["dim_x"] * meta["dim_y"]
  return float(meta["useful_cells"]) / float(pe_total * ii)


def _override_config_fields(src: str, overrides: dict) -> str:
  """Rewrite a YAML config to replace top-level scalar fields. `overrides` is
  {key: value} where value will be emitted as `key: "value"`. Keys not present
  in the source are appended at the bottom.
  """
  lines = []
  remaining = dict(overrides)
  for ln in src.splitlines():
    stripped = ln.lstrip()
    matched_key = None
    for k in remaining:
      if stripped.startswith(k + ":") or stripped.startswith(k + " "):
        matched_key = k
        break
    if matched_key is not None:
      indent = ln[: len(ln) - len(ln.lstrip())]
      lines.append('%s%s: "%s"' % (indent, matched_key, remaining[matched_key]))
      del remaining[matched_key]
    else:
      lines.append(ln)
  for k, v in remaining.items():
    lines.append('%s: "%s"' % (k, v))
  return "\n".join(lines)


def resolve_config(args: argparse.Namespace,
                   extra_overrides: dict | None = None) -> Path:
  """Write a temp config with --arch and any per-record overrides (e.g.
  node_pin) merged in. Returns the path to the temp config. If nothing to
  override and no arch flag, returns args.config unchanged.
  """
  extra_overrides = extra_overrides or {}
  if not args.arch and not extra_overrides:
    return args.config
  with open(args.config, "r", encoding="utf-8") as f:
    src = f.read()
  overrides = {}
  if args.arch:
    overrides["arch_template_json"] = args.arch
  overrides.update(extra_overrides)
  text = _override_config_fields(src, overrides)
  tmp = tempfile.NamedTemporaryFile(
      mode="w", suffix=".yaml", delete=False,
      dir=str(args.output_dir), prefix="config_override_")
  tmp.write(text)
  tmp.flush()
  tmp.close()
  return Path(tmp.name)


# ---------------------------------------------------------------------------
# LOAD pinning helpers
# ---------------------------------------------------------------------------


def _resolve_arch_json(args: argparse.Namespace) -> Path | None:
  """Resolve the absolute path of the arch JSON we'll map against. Mirrors the
  precedence in morpher_runner.context.resolve_arch_template_path: explicit
  --arch wins, else read arch_template_json from args.config. Falls back to
  the MORPHER_HOME / Morpher_CGRA_Mapper / json_arch / <relative_path> layout.
  """
  rel = args.arch
  if rel is None:
    try:
      with open(args.config, "r", encoding="utf-8") as f:
        for ln in f:
          s = ln.lstrip()
          if s.startswith("arch_template_json"):
            after = s.split(":", 1)[1].strip()
            rel = after.strip().strip('"').strip("'")
            break
    except OSError:
      return None
  if not rel:
    return None
  candidates = [
      REPO_ROOT / rel,
      REPO_ROOT / "Morpher_CGRA_Mapper" / "json_arch" / rel,
  ]
  for c in candidates:
    if c.is_file():
      return c
  return None


def _arch_has_perpendicular_mem_sides(mems: list[tuple[int, int]]) -> bool:
  """Returns True if the arch's PE_MEMs cover at least two perpendicular
  edges (e.g. left + top, left + right is parallel only — that's NOT
  perpendicular but is still two sides). Per the user's directive: if the
  arch has fewer than two perpendicular mem sides (e.g. left-only with no
  top/bottom row of mem PEs), don't attempt a systolic pinning — fall back
  to traditional non-pinned mapping.

  Definition used here: count distinct edges (left, right, top, bottom) that
  carry PE_MEMs. Return True iff at least 2 edges have PE_MEMs AND at least
  one pair is perpendicular (i.e. not both vertical or both horizontal).
  """
  if not mems:
    return False
  xs = sorted({x for x, _ in mems})
  ys = sorted({y for _, y in mems})
  xmin, xmax = xs[0], xs[-1]
  ymin, ymax = ys[0], ys[-1]
  left = any(x == xmin and y not in (ymin, ymax) or x == xmin for x, y in mems)
  right = (xmin != xmax) and any(x == xmax for x, y in mems)
  bottom = any(y == ymin for x, y in mems)
  top = (ymin != ymax) and any(y == ymax for x, y in mems)
  sides = {"left": left, "right": right, "bottom": bottom, "top": top}
  active = [k for k, v in sides.items() if v]
  if len(active) < 2:
    return False
  vert = {"left", "right"}
  horiz = {"top", "bottom"}
  has_vert = any(s in vert for s in active)
  has_horiz = any(s in horiz for s in active)
  return has_vert and has_horiz


def _read_simd_width(arch_json: Path | None) -> int:
  """Return the arch's SIMD_WIDTH from any FU/PE block that declares it
  (vec archs declare it on both PE and PE_MEM). 1 for scalar archs.
  """
  if arch_json is None or not arch_json.is_file():
    return 1
  try:
    with open(arch_json, "r", encoding="utf-8") as f:
      d = json.load(f)
  except (OSError, json.JSONDecodeError):
    return 1
  best = 1

  def walk(node):
    nonlocal best
    if isinstance(node, dict):
      if "SIMD_WIDTH" in node:
        try:
          v = int(node["SIMD_WIDTH"])
          if v > best:
            best = v
        except (TypeError, ValueError):
          pass
      for v in node.values():
        walk(v)
    elif isinstance(node, list):
      for v in node:
        walk(v)

  walk(d)
  return best


def _read_pe_mems(arch_json: Path) -> list[tuple[int, int]]:
  """Return PE_MEM coordinates with perpendicular injection sides first.

  One-stream templates consume the first vertical edge. Two-stream systolic
  templates then consume a horizontal edge, so A and B do not enter from
  opposite parallel sides.
  """
  try:
    with open(arch_json, "r", encoding="utf-8") as f:
      d = json.load(f)
  except (OSError, json.JSONDecodeError):
    return []
  submods = d.get("ARCH", {}).get("CGRA", {}).get("SUBMODS", [])
  mems = []
  for sub in submods:
    for m in sub.get("MODS", []):
      if m.get("MOD") == "PE_MEM":
        mems.append((int(m["X"]), int(m["Y"])))
  if not mems:
    return []
  xs = sorted({x for x, _ in mems})
  ys = sorted({y for _, y in mems})
  xmin, xmax = xs[0], xs[-1]
  ymin, ymax = ys[0], ys[-1]
  # Bucket by side: left then top, followed by right then bottom. This keeps
  # the first two populated sides perpendicular on 3/4-side architectures.
  ordered = []
  seen = set()

  def add(coord):
    if coord in seen or coord not in mems_set:
      return
    seen.add(coord)
    ordered.append(coord)

  mems_set = set(mems)
  for y in sorted(ys):
    add((xmin, y))
  for x in sorted(xs):
    add((x, ymax))
  for y in sorted(ys):
    add((xmax, y))
  for x in sorted(xs):
    add((x, ymin))
  # Anything not yet picked up (interior PE_MEM, unusual layouts).
  for coord in mems:
    add(coord)
  return ordered


def _collect_load_indices(xml: str) -> list[int]:
  """Return idxs of <Node><OP>LOAD</OP> in source order from a synthesized
  template XML. The template emits a top-level <DFG> wrapper inside a
  pseudo-root (no XML declaration); parse with a wrapping root tag.
  """
  try:
    root = ET.fromstring("<Root>\n%s\n</Root>" % xml)
  except ET.ParseError:
    return []
  loads = []
  for node in root.findall(".//Node"):
    op_elem = node.find("OP")
    if op_elem is None:
      continue
    op_text = (op_elem.text or "").strip()
    if op_text == "LOAD" or op_text == "VLOAD":
      try:
        loads.append(int(node.attrib["idx"]))
      except (KeyError, ValueError):
        continue
  return loads


def build_load_pin_string(xml: str, arch_json: Path | None,
                          strategy: str) -> str:
  """Compute a comma-separated `--node_pin` string for the synthesized DFG's
  LOAD nodes. `strategy` is one of "none", "first", "all".

  - "first": pin only LOAD_0 to the first PE_MEM (top-left in scan order).
    Acts as a placement seed — reduces SA run-to-run variance without
    over-constraining cell placement.
  - "all": pin every LOAD to a PE_MEM in the arch's natural side order. This
    is the honest "real systolic layout" pin but often makes the mapper
    assert on edges-not-routed for fanout-heavy templates because it forces
    a layout the SA otherwise wouldn't choose.

  Returns "" when strategy=="none", or when pinning isn't possible (no arch
  resolved, fewer PE_MEMs than required pins).
  """
  if strategy == "none" or arch_json is None:
    return ""
  # Read PE_MEMs without the side-bucketed reorder, so we can independently
  # decide perpendicular-side eligibility on the raw set.
  try:
    with open(arch_json, "r", encoding="utf-8") as f:
      d = json.load(f)
  except (OSError, json.JSONDecodeError):
    return ""
  raw_mems = []
  for sub in d.get("ARCH", {}).get("CGRA", {}).get("SUBMODS", []):
    for m in sub.get("MODS", []):
      if m.get("MOD") == "PE_MEM":
        raw_mems.append((int(m["X"]), int(m["Y"])))
  if not raw_mems:
    return ""
  if not _arch_has_perpendicular_mem_sides(raw_mems):
    # User directive: skip pinning on archs lacking ≥2 perpendicular mem
    # sides; the caller will fall back to traditional non-pinned mapping.
    return ""
  mems = _read_pe_mems(arch_json)
  loads = _collect_load_indices(xml)
  if not loads or not mems:
    return ""
  if strategy == "first":
    return "%d:%d:%d" % (loads[0], mems[0][0], mems[0][1])
  # strategy == "all": one input stream enters from the left edge; a second
  # stream continues on the perpendicular top edge when more pins are needed.
  if len(loads) > len(mems):
    return ""
  pins = ["%d:%d:%d" % (loads[i], mems[i][0], mems[i][1])
          for i in range(len(loads))]
  return ",".join(pins)


def main() -> int:
  args = parse_args()
  args.output_dir.mkdir(parents=True, exist_ok=True)
  args.report = args.report.resolve()
  args.config = args.config.resolve()
  # Base config (no pin overrides yet). Per-record overrides are applied
  # below when --pin-loads != "none".
  base_config = resolve_config(args)
  # Resolve arch JSON (used by pinning + SIMD width detection).
  effective_arch_json = _resolve_arch_json(args)
  arch_json_path = (effective_arch_json
                    if args.pin_loads != "none" else None)
  # Decide SIMD lane width: explicit --simd-lanes overrides; else
  # auto-detect from the arch JSON's SIMD_WIDTH.
  if args.simd_lanes > 0:
    simd_lanes = args.simd_lanes
  else:
    simd_lanes = _read_simd_width(effective_arch_json)

  records = load_report(args.report)
  if not records:
    sys.stderr.write("no candidate records in %s\n" % args.report)
    return 1

  allowed = set(s.strip() for s in args.only_verdict.split(","))

  print("=== synthesize_systolic ===")
  print("report=%s records=%d allowed_verdicts=%s" %
        (args.report, len(records), sorted(allowed)))
  arch_disp = effective_arch_json.name if effective_arch_json else "(unresolved)"
  print("arch=%s simd_lanes=%d pin_loads=%s" %
        (arch_disp, simd_lanes, args.pin_loads))
  print()
  print("%-30s %-10s %-12s %-8s %-8s %-6s %-8s %-9s"
        % ("kernel", "verdict", "template", "cells",
           "nops", "II", "status", "comp_util"))
  print("-" * 110)

  any_failure = False
  for idx, rec in enumerate(records):
    verdict = rec.get("verdict", "no")
    if verdict not in allowed:
      continue
    kernel = kernel_name(rec, idx)
    xml, meta = synthesize(rec, args, simd_lanes=simd_lanes)
    if xml is None:
      print("%-30s %-10s %-12s %-8s %-8s %-6s %-8s %-9s"
            % (kernel, verdict, meta["template"], "-", "-",
               "-", "skip", "-"))
      print("  skip_reason: %s" % meta.get("skip_reason", "-"))
      continue
    xml_path = args.output_dir / ("%s_systolic.xml" % kernel)
    write_xml(xml_path, xml)

    if args.dry_run:
      print("%-30s %-10s %-12s %-8d %-8d %-6s %-8s %-9s"
            % (kernel, verdict, meta["template"], meta["useful_cells"],
               meta["nops_in_dfg"], "-", "dry-run", "-"))
      continue

    # Per-record effective config: optional LOAD pinning depends on the
    # synthesized DFG, so we rebuild it here when --pin-loads != "none".
    pin_str = ""
    if args.pin_loads != "none":
      pin_str = build_load_pin_string(xml, arch_json_path, args.pin_loads)
      if pin_str:
        record_config = resolve_config(args, {"node_pin": pin_str})
        meta["node_pin"] = pin_str
      else:
        record_config = base_config
        sys.stderr.write(
            "warn: --pin-loads=%s requested for %s but pinning not possible "
            "(arch_json=%s, loads_or_mems_insufficient)\n"
            % (args.pin_loads, kernel, arch_json_path))
    else:
      record_config = base_config

    ii, status = invoke_mapper(xml_path, kernel, record_config, args.output_dir)
    # Graceful fallback: if pinning was requested AND the mapper crashed
    # (status is None or 'crashed'), retry without pinning. Matches the
    # user's directive: fall back to traditional non-pinned mapping when
    # the arch can't support a clean systolic layout.
    if pin_str and status in (None, "crashed"):
      sys.stderr.write(
          "info: %s: pinned mapping failed (status=%s); retrying without pins\n"
          % (kernel, status))
      ii, status = invoke_mapper(xml_path, kernel, base_config, args.output_dir)
      meta["pin_fallback"] = True
    util = utilization(meta, ii)
    ii_s = str(ii) if ii is not None else "-"
    util_s = "%.3f" % util if util is not None else "-"
    print("%-30s %-10s %-12s %-8d %-8d %-6s %-8s %-9s"
          % (kernel, verdict, meta["template"], meta["useful_cells"],
             meta["nops_in_dfg"], ii_s, status or "?", util_s))
    if meta.get("pin_fallback"):
      print("  pin: fallback (no pinning) after pinned attempt failed")
    elif meta.get("node_pin"):
      print("  pin: %s" % meta["node_pin"])
    if status != "success":
      any_failure = True
      continue

    # Per-phase helper: synthesize a P/E DFG and run the mapper on it.
    # Per-DFG stats are emitted via EmitMappingStats (Phase 1) so users can
    # aggregate via scripts/aggregate_mapping_stats.py.
    def _emit_phase(phase_label: str, phase_tname: str | None) -> None:
      nonlocal any_failure
      if phase_tname is None:
        print("  %s: skipped (no %s mapping for template '%s')"
              % (phase_label, phase_label, meta["template"]))
        return
      phase_xml = TEMPLATES[phase_tname](
          args.dim_x, args.dim_y, meta["iter_max"],
          op_kind=meta["op_kind"],
          useful_cells=meta["useful_cells"],
          simd_lanes=simd_lanes,
      )
      phase_kernel = "%s_%s" % (kernel, phase_label)
      phase_path = args.output_dir / ("%s_systolic.xml" % phase_kernel)
      write_xml(phase_path, phase_xml)
      ph_ii, ph_status = invoke_mapper(phase_path, phase_kernel,
                                        base_config, args.output_dir)
      ph_ii_s = str(ph_ii) if ph_ii is not None else "-"
      print("  %s: template=%s useful_cells=%d ii=%s status=%s"
            % (phase_label, phase_tname, meta["useful_cells"],
               ph_ii_s, ph_status or "?"))
      if ph_status != "success":
        any_failure = True

    if args.emit_prologue:
      _emit_phase("prologue", PROLOGUE_FOR_TEMPLATE.get(meta["template"]))
    if args.emit_epilogue:
      _emit_phase("epilogue", EPILOGUE_FOR_TEMPLATE.get(meta["template"]))

    if args.run_sim:
      if simd_lanes > 1:
        # Ref-sim produces scalar (1-lane) values; the C++ simulator reads
        # simd_packed=true and expects N-lane outputs. Without a lane-wise
        # reference trace, mismatches are spurious. Skip cleanly and tag
        # the row.
        print("  sim: skipped (simd_lanes=%d > 1; lane-wise ref-trace not "
              "implemented — see REFERENCE.md SIMD-systolic limitation)"
              % simd_lanes)
      else:
        sim_matches, sim_mismatches = run_simulator(kernel, xml_path, args.output_dir)
        if sim_matches is None:
          print("  sim: unable to invoke artifact_simulator")
          any_failure = True
        else:
          print("  sim: matches=%d mismatches=%d %s" %
                (sim_matches, sim_mismatches,
                 "OK" if sim_mismatches == 0 else "MISMATCH"))
          if sim_mismatches != 0:
            any_failure = True

  return 1 if any_failure else 0


if __name__ == "__main__":
  sys.exit(main())
