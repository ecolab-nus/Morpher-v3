#!/usr/bin/env python3
"""Enumerate and optionally map source-to-systolic-template bindings.

The output distinguishes semantic binding, template occupancy, architecture
resource bounds, and actual mapper success.  It never treats a legacy
shape-only template map as source-equivalence.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from morpher_runner.systolic_binding import (  # noqa: E402
    MAIN_TEMPLATES,
    SOURCE_BOUND_TEMPLATES,
    Binding,
    analyze_binding,
    read_arch_facts,
    source_model,
    with_mapper_result,
)
from morpher_runner.systolic_templates import TEMPLATES  # noqa: E402
from scripts.lower_source_systolic import (  # noqa: E402
    bind_template_bases,
    invoke_mapper,
    write_mem_alloc,
)
from scripts.synthesize_systolic import (  # noqa: E402
    _override_config_fields,
    _resolve_arch_json,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKSPACE_ROOT = REPO_ROOT / "workspace" / "morpher_runs"


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--report", action="append", type=Path, default=[])
  parser.add_argument(
      "--report-root",
      action="append",
      type=Path,
      default=[],
      help="Recursively include candidate-report JSON files below this directory",
  )
  parser.add_argument("--output-dir", required=True, type=Path)
  parser.add_argument("--config", type=Path, default=REPO_ROOT / "config" / "scalar.yaml")
  parser.add_argument("--arch", default=None)
  parser.add_argument("--dim-x", type=int, default=4)
  parser.add_argument("--dim-y", type=int, default=4)
  parser.add_argument(
      "--templates",
      default=",".join(MAIN_TEMPLATES),
      help="Comma-separated main templates, or 'all'",
  )
  parser.add_argument(
      "--batched-active-cells",
      default="auto",
      help=(
          "Comma-separated active-cell counts to sweep for "
          "batched_dot_output_stationary; default 'auto' uses the full grid"
      ),
  )
  parser.add_argument("--map", action="store_true", help="Run mapper for supported source-bound rows")
  parser.add_argument(
      "--map-conditional",
      action="store_true",
      help="Also map rows whose semantic binding lacks some source evidence",
  )
  parser.add_argument(
      "--function",
      action="append",
      default=[],
      help="Only analyze exact function name(s); may be repeated",
  )
  parser.add_argument(
      "--only-verdict",
      default="yes,maybe",
      help="Comma-separated candidate verdicts to include; use 'all' for every record",
  )
  parser.add_argument(
      "--report-path-contains",
      default="",
      help="Only include report paths containing this substring",
  )
  parser.add_argument("--mapper-timeout-sec", type=int, default=120)
  parser.add_argument(
      "--multihop", type=int, choices=(1, 4), default=4,
      help="Mapper routing horizon; systolic comparisons intentionally use only 1 or 4",
  )
  return parser.parse_args()


def load_records(path: Path) -> list[dict]:
  records = []
  for line in path.read_text(encoding="utf-8").splitlines():
    line = line.strip()
    if line:
      records.append(json.loads(line))
  return records


def report_paths(args: argparse.Namespace) -> list[Path]:
  paths = list(args.report)
  for root in args.report_root:
    paths.extend(root.rglob("*.json"))
  unique = sorted({
      path.resolve() for path in paths
      if path.is_file() and (
          not args.report_path_contains
          or args.report_path_contains in str(path)
      )
  })
  if not unique:
    raise SystemExit("provide --report or --report-root with candidate JSON files")
  return unique


def resolve_arch(args: argparse.Namespace) -> Path:
  probe = argparse.Namespace(arch=args.arch, config=args.config)
  path = _resolve_arch_json(probe)
  if path is None:
    raise SystemExit("could not resolve architecture JSON from --arch/--config")
  return path


def mapper_config(args: argparse.Namespace) -> Path:
  if not args.arch and args.multihop == 4:
    return args.config.resolve()
  target = args.output_dir / "binding_config.yaml"
  source = args.config.read_text(encoding="utf-8")
  overrides = {"multihop": args.multihop}
  if args.arch:
    overrides["arch_template_json"] = args.arch
  target.write_text(
      _override_config_fields(source, overrides),
      encoding="utf-8",
  )
  return target


def latest_stats(kernel: str, started: float) -> dict:
  if not WORKSPACE_ROOT.exists():
    return {}
  candidates = []
  for path in WORKSPACE_ROOT.glob("**/*_stats.json"):
    try:
      if path.stat().st_mtime < started or kernel not in path.parts:
        continue
    except OSError:
      continue
    candidates.append(path)
  if not candidates:
    return {}
  path = max(candidates, key=lambda item: item.stat().st_mtime)
  try:
    return json.loads(path.read_text(encoding="utf-8"))
  except (OSError, json.JSONDecodeError):
    return {}


def safe_name(value: str) -> str:
  return re.sub(r"[^A-Za-z0-9_]", "_", value)


def template_bases(binding: Binding) -> tuple[list[str], list[str]]:
  inputs = list(binding.source_input_bases)
  if binding.coefficient_base:
    if len(inputs) < 2:
      inputs.append(binding.coefficient_base)
    else:
      inputs[1] = binding.coefficient_base
  while len(inputs) < 2:
    inputs.append(f"__systolic_input{len(inputs)}")
  outputs = list(binding.source_output_bases) or ["__systolic_output"]
  return inputs, outputs


def map_binding(
    binding: Binding,
    record: dict,
    args: argparse.Namespace,
    config: Path,
) -> Binding:
  if binding.template not in SOURCE_BOUND_TEMPLATES:
    return with_mapper_result(
        binding, "not_run", None, None, "",
        "template implementation is shape-only",
        mapper_multihop=args.multihop,
    )
  if binding.binding_status == "incompatible":
    return binding
  if binding.binding_status == "conditional" and not args.map_conditional:
    return with_mapper_result(
        binding, "not_run", None, None, "",
        "use --map-conditional to map a binding with incomplete source evidence",
        mapper_multihop=args.multihop,
    )

  terms = binding.terms_per_output or 1
  useful = binding.active_cells
  xml = TEMPLATES[binding.template](
      binding.generated_dim_x,
      binding.generated_dim_y,
      terms,
      op_kind="MAC",
      useful_cells=useful,
      simd_lanes=binding.simd_width,
  )
  inputs, outputs = template_bases(binding)
  xml = bind_template_bases(xml, inputs, outputs)

  report_tag = hashlib.sha1(
      binding.source_report.encode("utf-8")
  ).hexdigest()[:8]
  kernel = safe_name(
      f"{binding.function}_{report_tag}_r{binding.record_index}_{binding.template}"
      f"_u{binding.active_cells}"
  )
  kernel_dir = args.output_dir / "artifacts" / kernel
  kernel_dir.mkdir(parents=True, exist_ok=True)
  xml_path = kernel_dir / f"{kernel}_systolic.xml"
  xml_path.write_text(xml, encoding="utf-8")
  write_mem_alloc(
      xml_path.with_name(kernel + "_mem_alloc.txt"),
      inputs + outputs,
  )

  started = time.time()
  ii, status = invoke_mapper(
      xml_path, kernel, config, kernel_dir, args.mapper_timeout_sec
  )
  stats = latest_stats(kernel, started) if status == "success" else {}
  util = stats.get("util_phase")
  return with_mapper_result(
      binding,
      status or "unknown",
      ii,
      float(util) if util is not None else None,
      str(xml_path),
      "",
      mapper_multihop=args.multihop,
  )


def write_outputs(rows: list[Binding], output_dir: Path) -> None:
  output_dir.mkdir(parents=True, exist_ok=True)
  json_rows = [row.to_dict() for row in rows]
  (output_dir / "bindings.json").write_text(
      json.dumps(json_rows, indent=2) + "\n",
      encoding="utf-8",
  )
  fieldnames = list(json_rows[0].keys()) if json_rows else []
  with (output_dir / "bindings.csv").open("w", encoding="utf-8", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=fieldnames)
    if fieldnames:
      writer.writeheader()
      writer.writerows(json_rows)


def print_summary(rows: list[Binding]) -> None:
  print(
      "source function template transform bind active/cells spatial mem_lb "
      "map II main end_to_end reason"
  )
  for row in rows:
    report_path = Path(row.source_report) if row.source_report else None
    report_parent = report_path.parent.name if report_path else ""
    source = (
        report_path.stem
        if report_path and report_parent in {"tmp", "candidate_reports"}
        else report_parent or row.function
    )
    print(
        f"{source} {row.function} {row.template} {row.transform} {row.binding_status} "
        f"{row.active_cells}/{row.template_cells} "
        f"{row.template_spatial_utilization:.3f} "
        f"{row.memory_ii_lower_bound if row.memory_ii_lower_bound is not None else '-'} "
        f"{row.map_status} {row.mapped_ii if row.mapped_ii is not None else '-'} "
        f"{row.main_feasibility} {row.end_to_end_feasibility} "
        f"{json.dumps(row.binding_reason)}"
    )


def main() -> int:
  args = parse_args()
  if args.dim_x < 1 or args.dim_y < 1:
    raise SystemExit("--dim-x and --dim-y must be positive")
  args.output_dir.mkdir(parents=True, exist_ok=True)
  arch_path = resolve_arch(args)
  arch = read_arch_facts(arch_path)
  config = mapper_config(args)

  templates = list(MAIN_TEMPLATES) if args.templates == "all" else [
      item.strip() for item in args.templates.split(",") if item.strip()
  ]
  unknown = sorted(set(templates) - set(MAIN_TEMPLATES))
  if unknown:
    raise SystemExit("unsupported main template(s): " + ", ".join(unknown))
  if args.batched_active_cells == "auto":
    batched_active_cells: list[int | None] = [None]
  else:
    try:
      batched_active_cells = sorted({
          int(item) for item in args.batched_active_cells.split(",")
          if item.strip()
      })
    except ValueError as exc:
      raise SystemExit("--batched-active-cells must contain integers") from exc
    if not batched_active_cells or any(
        item < 1 or item > args.dim_x * args.dim_y
        for item in batched_active_cells
    ):
      raise SystemExit("--batched-active-cells values must fit the template grid")

  rows: list[Binding] = []
  functions = set(args.function)
  verdicts = None if args.only_verdict == "all" else {
      item.strip() for item in args.only_verdict.split(",") if item.strip()
  }
  for report in report_paths(args):
    records = load_records(report)
    for index, record in enumerate(records):
      if functions and str(record.get("function") or "") not in functions:
        continue
      if verdicts is not None and str(record.get("verdict") or "") not in verdicts:
        continue
      model = source_model(record)
      for template in templates:
        active_limits = (
            batched_active_cells
            if template == "batched_dot_output_stationary"
            else [None]
        )
        for active_limit in active_limits:
          binding = analyze_binding(
              model, index, template, args.dim_x, args.dim_y, arch,
              source_report=str(report), active_cell_limit=active_limit,
          )
          if args.map:
            binding = map_binding(binding, record, args, config)
          rows.append(binding)

  write_outputs(rows, args.output_dir)
  print_summary(rows)
  print(f"wrote {args.output_dir / 'bindings.csv'}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
