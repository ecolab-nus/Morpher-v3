#!/usr/bin/env python
import csv
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import yaml

from morpher_runner.context import build_context
from morpher_runner.context import resolve_arch_template_path
from morpher_runner.artifact_simulator import run_artifact_simulator
from morpher_runner.dfg_generator import build_ir_generation_commands
from morpher_runner.dfg_generator import run_dfg_generator
from morpher_runner.dfg_generator import simd_packing_active_for_factor
from morpher_runner.exec import Command
from morpher_runner.exec import run_commands
from morpher_runner.mapper import run_mapper
from morpher_runner.systolic_binding import read_arch_facts
from morpher_runner.template_lowering import lower_template_record
from morpher_runner.template_lowering import write_lowering_artifacts
from morpher_runner.utils import build_llvm_tool_env
from morpher_runner.utils import resolve_llvm_tool

DEFAULT_CONFIG_PATH = "config/default.yaml"
RUNMODES = {"runall", "dfg_gen_only", "mapper_only", "sim_only", "artifact_sim_only", "runall_artifact_sim"}
_RESULT_LINE_II_RE = re.compile(r"method:\S+\s+(\d+)\s+status:(success|failed)\s+\d+\s*$")


def _derive_kernel_from_xml(xml_path):
  stem = os.path.splitext(os.path.basename(xml_path))[0]
  for suffix in ("_PartPred_DFG", "_FullPred_DFG", "_PartPredDFG", "_FullPredDFG", "_DFG"):
    if stem.endswith(suffix):
      stem = stem[:-len(suffix)]
      break
  if not stem:
    raise RuntimeError("Could not derive kernel name from XML path: %s" % xml_path)
  return stem


def _usage():
  return (
    "Usage:\n"
    "  python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]\n"
    "  python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]\n\n"
    "Options:\n"
    "  --run-tag <tag>        Write DFG/mapper/simulator artifacts under tagged output directories.\n"
    "  --output-root <path>   Root for structured run output (default: generated).\n"
    "                         Each run writes to <root>/<suite>/<kernel>/<timestamp>/.\n"
    "                         Ignored when --run-tag is provided (batch mode).\n"
    "  --mem-alloc <path>     Mem-alloc file for XML input mode.\n\n"
    "  --source-template-report <path>\n"
    "                         Try source-bound template lowering from a SystolicCandidatePass JSONL report.\n"
    "                         Use 'auto' to generate the report first in the run directory.\n"
    "                         Accepted records stage XML for the mapper; conditional/rejected records fall back.\n"
    "  --source-template-record <index>\n"
    "                         Report record index to try first (default: first record for the function, else 0).\n"
    "  --source-template-active-cell-limit <n>\n"
    "                         Optional active-cell cap for source-bound template lowering.\n\n"
    "Run modes: runall, dfg_gen_only, mapper_only, sim_only, artifact_sim_only, runall_artifact_sim\n"
    "XML mode defaults: function=<derived-from-xml>, config_path=%s, runmode=mapper_only\n"
    % DEFAULT_CONFIG_PATH
  )


def _parse_cli(argv):
  positional = []
  mem_alloc_path = None
  run_tag = ""
  output_root = "generated"
  source_template_report = None
  source_template_record = None
  source_template_active_cell_limit = None

  i = 0
  while i < len(argv):
    token = argv[i]
    if token == "--run-tag":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --run-tag.\n\n%s" % _usage())
      run_tag = argv[i + 1]
      i += 2
      continue
    if token == "--output-root":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --output-root.\n\n%s" % _usage())
      output_root = argv[i + 1]
      i += 2
      continue
    if token == "--mem-alloc":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --mem-alloc.\n\n%s" % _usage())
      mem_alloc_path = argv[i + 1]
      i += 2
      continue
    if token == "--source-template-report":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --source-template-report.\n\n%s" % _usage())
      source_template_report = argv[i + 1]
      i += 2
      continue
    if token == "--source-template-record":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --source-template-record.\n\n%s" % _usage())
      try:
        source_template_record = int(argv[i + 1])
      except ValueError:
        raise RuntimeError("--source-template-record must be an integer.")
      if source_template_record < 0:
        raise RuntimeError("--source-template-record must be non-negative.")
      i += 2
      continue
    if token == "--source-template-active-cell-limit":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --source-template-active-cell-limit.\n\n%s" % _usage())
      try:
        source_template_active_cell_limit = int(argv[i + 1])
      except ValueError:
        raise RuntimeError("--source-template-active-cell-limit must be an integer.")
      if source_template_active_cell_limit <= 0:
        raise RuntimeError("--source-template-active-cell-limit must be positive.")
      i += 2
      continue
    if token.startswith("--"):
      raise RuntimeError("Unknown option: %s\n\n%s" % (token, _usage()))
    positional.append(token)
    i += 1

  if not positional:
    raise RuntimeError(_usage())

  first = positional[0]
  is_xml_mode = first.lower().endswith(".xml")

  if is_xml_mode:
    if source_template_report:
      raise RuntimeError("--source-template-report cannot be combined with XML input mode.")
    if len(positional) > 4:
      raise RuntimeError("Too many positional arguments for XML mode.\n\n%s" % _usage())

    xml_path = first
    function = positional[1] if len(positional) >= 2 else _derive_kernel_from_xml(xml_path)
    config = positional[2] if len(positional) >= 3 else DEFAULT_CONFIG_PATH
    runmode = positional[3] if len(positional) >= 4 else "mapper_only"

    if runmode not in ("mapper_only", "runall", "artifact_sim_only"):
      raise RuntimeError(
        "XML mode supports runmode 'mapper_only', 'runall', or 'artifact_sim_only' (got: %s)." % runmode
      )

    csource = "xml_inputs/%s/%s.c" % (function, function)
    return (
      csource, function, config, runmode, xml_path, mem_alloc_path, run_tag,
      output_root, source_template_report, source_template_record,
      source_template_active_cell_limit,
    )

  if len(positional) < 2 or len(positional) > 4:
    raise RuntimeError("Invalid arguments.\n\n%s" % _usage())
  if mem_alloc_path:
    raise RuntimeError("--mem-alloc can only be used when the first argument is an XML file.")

  csource = positional[0]
  function = positional[1]
  config = positional[2] if len(positional) >= 3 else DEFAULT_CONFIG_PATH
  runmode = positional[3] if len(positional) >= 4 else "runall"
  return (
    csource, function, config, runmode, None, None, run_tag, output_root,
    source_template_report, source_template_record,
    source_template_active_cell_limit,
  )


def _resolve_mem_alloc_for_xml(xml_path, kernel, explicit_mem_alloc):
  if explicit_mem_alloc:
    explicit = os.path.abspath(explicit_mem_alloc)
    if not os.path.isfile(explicit):
      raise RuntimeError("Provided mem-alloc file not found: %s" % explicit_mem_alloc)
    return explicit

  xml_dir = os.path.dirname(os.path.abspath(xml_path))
  xml_stem = os.path.splitext(os.path.basename(xml_path))[0]
  candidates = [
    os.path.join(xml_dir, "%s_mem_alloc.txt" % kernel),
    os.path.join(xml_dir, "%s_mem_alloc.txt" % xml_stem),
  ]
  for candidate in candidates:
    if os.path.isfile(candidate):
      return candidate
  return None


def _stage_mapper_inputs_from_xml(context, xml_path, mem_alloc_path=None):
  xml_src = os.path.abspath(xml_path)
  if not os.path.isfile(xml_src):
    raise RuntimeError("DFG XML not found: %s" % xml_path)

  xml_dst = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % context.kernel)
  shutil.copy2(xml_src, xml_dst)
  print("Using existing DFG XML: %s" % xml_src)
  print("Staged mapper DFG XML: %s" % xml_dst)

  if context.config.memory_mode != "auto":
    return

  mem_src = _resolve_mem_alloc_for_xml(xml_src, context.kernel, mem_alloc_path)
  if not mem_src:
    raise RuntimeError(
      "memory_mode=auto requires a mem-alloc file named '%s_mem_alloc.txt' (or '<xml-stem>_mem_alloc.txt') "
      "next to the XML, or pass one with --mem-alloc. "
      "Alternatively, use a config with memory_mode=static."
      % context.kernel
    )

  mem_dst = os.path.join(context.mapper_kernel, "%s_mem_alloc.txt" % context.kernel)
  shutil.copy2(mem_src, mem_dst)
  print("Staged mapper mem-alloc: %s" % mem_dst)


def _load_template_report_records(report_path):
  path = Path(report_path)
  if not path.is_file():
    raise RuntimeError("Source-template report not found: %s" % report_path)
  records = []
  with path.open("r", encoding="utf-8") as report_file:
    for line_no, line in enumerate(report_file, 1):
      line = line.strip()
      if not line:
        continue
      try:
        records.append((len(records), json.loads(line)))
      except json.JSONDecodeError as exc:
        raise RuntimeError(
          "Invalid JSON object at %s:%d: %s" % (report_path, line_no, exc)
        )
  return [
    (idx, record) for idx, record in records
    if isinstance(record, dict)
  ]


def _select_template_record(records, function, record_index):
  if record_index is not None:
    for idx, record in records:
      if idx == record_index:
        return idx, record
    raise RuntimeError("Source-template report has no record index %d." % record_index)

  for idx, record in records:
    if str(record.get("function") or "") == function:
      return idx, record
  if records:
    return records[0]
  raise RuntimeError("Source-template report contains no candidate records.")


def _candidate_template_records(records, function, record_index):
  if record_index is not None:
    return [_select_template_record(records, function, record_index)]
  matches = [
    (idx, record) for idx, record in records
    if str(record.get("function") or "") == function
  ]
  return matches if matches else records


def _generate_source_template_report(context):
  report_path = Path(context.dfg_gen_kernel) / ("%s_systolic_candidates.jsonl" % context.kernel)
  report_path.parent.mkdir(parents=True, exist_ok=True)
  if report_path.exists():
    report_path.unlink()

  print("Generating SystolicCandidatePass report: %s\n" % report_path)
  run_commands(build_ir_generation_commands(context))
  run_commands([
    Command(
      argv=[
        resolve_llvm_tool("opt"),
        "-load-pass-plugin",
        "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
        "-passes=morpher-systolic-candidate",
        "-morpher-systolic-report=%s" % report_path,
        "%s_opt.ll" % context.kernel,
        "-disable-output",
      ],
      cwd=context.dfg_gen_kernel,
      env=build_llvm_tool_env(),
    )
  ])
  return str(report_path)


def _source_template_dims(context, arch):
  compute_pes = getattr(arch, "mac_pes", 0) or 0
  side = int(compute_pes ** 0.5)
  if side > 0 and side * side == compute_pes:
    return side, side
  return context.config.xdim, context.config.ydim


def _write_template_fallback_metadata(context, result, report_path, record_index):
  meta = dict(result.metadata)
  meta["source_report"] = os.path.abspath(report_path)
  meta["record_index"] = record_index
  meta["runner_action"] = "normal_dfg_fallback"
  path = Path(context.dfg_gen_kernel) / ("%s_source_template_metadata.json" % context.kernel)
  path.parent.mkdir(parents=True, exist_ok=True)
  path.write_text(
    json.dumps(meta, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
  )
  return path


def _try_stage_source_template(
  context,
  report_path,
  record_index=None,
  active_cell_limit=None,
  allow_empty_fallback=False,
):
  records = _load_template_report_records(report_path)
  if not records and allow_empty_fallback:
    print(
      "Source-template report contains no candidate records; "
      "continuing through normal DFG generation.\n"
    )
    return False
  candidates = _candidate_template_records(records, context.kernel, record_index)
  arch_path = resolve_arch_template_path(
    context.morpher_home,
    context.mapper_home,
    context.config.arch_template_json,
  )
  arch = read_arch_facts(Path(arch_path))
  template_dim_x, template_dim_y = _source_template_dims(context, arch)

  first_result = None
  first_index = None
  selected_index = None
  result = None
  for candidate_index, record in candidates:
    candidate_result = lower_template_record(
      record,
      candidate_index,
      dim_x=template_dim_x,
      dim_y=template_dim_y,
      arch=arch,
      source_report=os.path.abspath(report_path),
      active_cell_limit=active_cell_limit,
    )
    if first_result is None:
      first_result = candidate_result
      first_index = candidate_index
    if candidate_result.accepted:
      result = candidate_result
      selected_index = candidate_index
      break

  if result is None:
    result = first_result
    selected_index = first_index
  if result is None or selected_index is None:
    if allow_empty_fallback:
      print(
        "Source-template report has no selectable records; "
        "continuing through normal DFG generation.\n"
      )
      return False
    raise RuntimeError("Source-template report contains no candidate records.")

  template_dir = Path(context.dfg_gen_kernel) / "source_template"
  if not result.accepted:
    meta_path = _write_template_fallback_metadata(context, result, report_path, selected_index)
    print(
      "Source-template lowering %s for record %d: %s" %
      (result.status, selected_index, result.reason)
    )
    print("Fallback metadata: %s" % meta_path)
    print("Continuing through normal DFG generation.\n")
    return False

  xml_path, mem_path, meta_path = write_lowering_artifacts(
    result,
    template_dir,
    context.kernel,
  )
  _stage_mapper_inputs_from_xml(context, str(xml_path), str(mem_path))
  print(
    "Source-template lowering accepted for record %d: %s" %
    (selected_index, result.reason)
  )
  template_name = result.metadata.get("template", "unknown")
  print(
    "Template mapping found and used: template=%s, record=%d, mapper_xml=%s\n"
    % (
      template_name,
      selected_index,
      os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % context.kernel),
    )
  )
  print("Source-template metadata: %s" % meta_path)
  return True


def _latest_mapper_result(context, start_time):
  result_path = os.path.join(context.mapper_kernel, "result.txt")
  if not os.path.isfile(result_path):
    return None, None
  if os.path.getmtime(result_path) < start_time:
    return None, None

  with open(result_path, "r", encoding="utf-8", errors="ignore") as result_file:
    lines = [line.strip() for line in result_file if line.strip()]
  if not lines:
    return None, None

  match = _RESULT_LINE_II_RE.search(lines[-1])
  if not match:
    return None, None

  return int(match.group(1)), match.group(2)


def _auto_unroll_score(ii, factor):
  return float(ii) / float(factor)


def _auto_unroll_dirs(context):
  return {
    "dfg": context.dfg_gen_kernel.rstrip("/"),
    "mapper": context.mapper_kernel.rstrip("/"),
    "simulator": context.simulator_kernel.rstrip("/"),
  }


def _set_auto_unroll_dirs(context, dirs):
  context.dfg_gen_kernel = dirs["dfg"].rstrip("/") + "/"
  context.mapper_kernel = dirs["mapper"].rstrip("/") + "/"
  context.simulator_kernel = dirs["simulator"].rstrip("/") + "/"
  context.mem_trace = os.path.join(dirs["dfg"].rstrip("/"), "memtraces")
  os.makedirs(context.dfg_gen_kernel, exist_ok=True)
  os.makedirs(context.mapper_kernel, exist_ok=True)
  os.makedirs(context.simulator_kernel, exist_ok=True)
  os.makedirs(context.mem_trace, exist_ok=True)


def _candidate_auto_unroll_dirs(golden_dirs, factor):
  return {
    label: "%s_uf%d" % (path.rstrip("/"), factor)
    for label, path in golden_dirs.items()
  }


def _replace_dir(src, dst):
  if os.path.exists(dst):
    shutil.rmtree(dst)
  if os.path.isdir(src):
    shutil.copytree(src, dst)
  else:
    os.makedirs(dst, exist_ok=True)


def _prepare_auto_unroll_candidate_dirs(source_template, candidate_dirs):
  for path in candidate_dirs.values():
    if os.path.exists(path):
      shutil.rmtree(path)
  shutil.copytree(source_template, candidate_dirs["dfg"])
  os.makedirs(candidate_dirs["mapper"], exist_ok=True)
  os.makedirs(candidate_dirs["simulator"], exist_ok=True)
  os.makedirs(os.path.join(candidate_dirs["dfg"], "memtraces"), exist_ok=True)


def _promote_auto_unroll_candidate(candidate_dirs, golden_dirs):
  for label, dst in golden_dirs.items():
    _replace_dir(candidate_dirs[label], dst)


def _write_auto_unroll_results(golden_dirs, rows, best):
  mapper_dir = golden_dirs["mapper"]
  os.makedirs(mapper_dir, exist_ok=True)
  path = os.path.join(mapper_dir, "auto_unroll_results.csv")
  with open(path, "w", encoding="utf-8", newline="") as outfile:
    writer = csv.DictWriter(
      outfile,
      fieldnames=[
        "factor",
        "status",
        "ii",
        "score",
        "selected",
        "promoted",
        "candidate_mapper_dir",
      ],
    )
    writer.writeheader()
    best_factor = best["factor"] if best else None
    for row in rows:
      out_row = dict(row)
      out_row["selected"] = "1" if row.get("factor") == best_factor else "0"
      writer.writerow(out_row)


def _run_auto_unroll(context):
  config = context.config
  factors = sorted(config.auto_unroll_factors, reverse=True)
  best = None
  rows = []
  golden_dirs = _auto_unroll_dirs(context)

  print("\n-----Running Morpher auto-unroll search-----\n")
  print("Factors, high to low: %s" % ", ".join(str(f) for f in factors))
  if config.auto_unroll_fuse_innermost_loops:
    print("Innermost loop fusion: enabled where LLVM can prove legality")
  if config.arch_simd_enabled:
    print(
      "Architecture-driven SIMD packing activates only for candidates with unroll_factor >= SIMD_WIDTH=%d."
      % config.arch_simd_width
    )
  if config.auto_unroll_scalar_reduction_policy != "none":
    print(
      "Scalar reduction lowering: policy=%s, terms=%d (opt-in)"
      % (
        config.auto_unroll_scalar_reduction_policy,
        config.auto_unroll_scalar_reduction_terms,
      )
    )
  print("Remainder policy: preserve (fixed)")
  print(
    "Score: II / unroll_factor. Lower is better; this approximates normalized mapped work.\n"
  )

  with tempfile.TemporaryDirectory(prefix="morpher_auto_unroll_") as temp_root:
    source_template = os.path.join(temp_root, "source_template")
    shutil.copytree(golden_dirs["dfg"], source_template)

    for factor in factors:
      print("== Auto-unroll candidate factor %d ==\n" % factor)
      if config.arch_simd_enabled and not simd_packing_active_for_factor(config, factor):
        print(
          "Candidate factor %d will map as scalar on the SIMD-capable architecture.\n"
          % factor
        )
      candidate_dirs = _candidate_auto_unroll_dirs(golden_dirs, factor)
      _prepare_auto_unroll_candidate_dirs(source_template, candidate_dirs)
      _set_auto_unroll_dirs(context, candidate_dirs)
      start_time = time.time()
      try:
        run_dfg_generator(
          context,
          unroll_factor=factor,
        )
        run_mapper(context)
      except subprocess.CalledProcessError as exc:
        print(
          "Candidate factor %d failed with exit code %d; trying the next lower factor.\n"
          % (factor, exc.returncode)
        )
        rows.append({
          "factor": factor,
          "status": "failed_exit_%d" % exc.returncode,
          "ii": "",
          "score": "",
          "promoted": "0",
          "candidate_mapper_dir": candidate_dirs["mapper"],
        })
        _set_auto_unroll_dirs(context, golden_dirs)
        _write_auto_unroll_results(golden_dirs, rows, best)
        continue

      ii, status = _latest_mapper_result(context, start_time)
      if status != "success" or ii is None:
        print(
          "Candidate factor %d did not produce a successful mapper result; trying the next lower factor.\n"
          % factor
        )
        rows.append({
          "factor": factor,
          "status": status or "no_result",
          "ii": ii if ii is not None else "",
          "score": "",
          "promoted": "0",
          "candidate_mapper_dir": candidate_dirs["mapper"],
        })
        _set_auto_unroll_dirs(context, golden_dirs)
        _write_auto_unroll_results(golden_dirs, rows, best)
        continue

      score = _auto_unroll_score(ii, factor)
      print("Candidate factor %d: II=%d, score=%.4f\n" % (factor, ii, score))

      candidate = {"factor": factor, "ii": ii, "score": score}
      if best is None or score < best["score"] or (
        score == best["score"] and factor > best["factor"]
      ):
        best = candidate
        _promote_auto_unroll_candidate(candidate_dirs, golden_dirs)
        rows.append({
          "factor": factor,
          "status": "success",
          "ii": ii,
          "score": "%.8f" % score,
          "promoted": "1",
          "candidate_mapper_dir": candidate_dirs["mapper"],
        })
        _set_auto_unroll_dirs(context, golden_dirs)
        _write_auto_unroll_results(golden_dirs, rows, best)
        print(
          "Candidate factor %d is the new best; promoted to golden artifacts.\n"
          % factor
        )
        continue

      rows.append({
        "factor": factor,
        "status": "success_degraded",
        "ii": ii,
        "score": "%.8f" % score,
        "promoted": "0",
        "candidate_mapper_dir": candidate_dirs["mapper"],
      })
      _set_auto_unroll_dirs(context, golden_dirs)
      _write_auto_unroll_results(golden_dirs, rows, best)
      if config.auto_unroll_stop_on_degradation:
        print(
          "Factor %d is worse than best factor %d; stopping lower-factor search. "
          "Golden artifacts remain at the best factor.\n"
          % (factor, best["factor"])
        )
        break

    _set_auto_unroll_dirs(context, golden_dirs)

  if best is None:
    raise RuntimeError("auto_unroll failed: no candidate factor mapped successfully.")

  print(
    "Selected auto-unroll factor %d (II=%d, score=%.4f).\n"
    % (best["factor"], best["ii"], best["score"])
  )

  if context.config.enable_simulation:
    run_artifact_simulator(context, context.config.validation_mode)
  else:
    print("\nSimulation disabled by config (enable_simulation=false); skipping validation stage.\n")


def main(
  csource,
  function,
  config=DEFAULT_CONFIG_PATH,
  runmode="runall_artifact_sim",
  dfg_xml_path=None,
  mem_alloc_path=None,
  run_tag="",
  source_template_report=None,
  source_template_record=None,
  source_template_active_cell_limit=None,
):
  print(r"""
    __  ___                 __                 ________________  ___       ____            _                ______                                             __  
   /  |/  /___  _________  / /_  ___  _____   / ____/ ____/ __ \/   |     / __ \___  _____(_)___ _____     / ____/________ _____ ___  ___ _      ______  _____/ /__
  / /|_/ / __ \/ ___/ __ \/ __ \/ _ \/ ___/  / /   / / __/ /_/ / /| |    / / / / _ \/ ___/ / __ `/ __ \   / /_  / ___/ __ `/ __ `__ \/ _ \ | /| / / __ \/ ___/ //_/
 / /  / / /_/ / /  / /_/ / / / /  __/ /     / /___/ /_/ / _, _/ ___ |   / /_/ /  __(__  ) / /_/ / / / /  / __/ / /  / /_/ / / / / / /  __/ |/ |/ / /_/ / /  / ,<   
/_/  /_/\____/_/  / .___/_/ /_/\___/_/      \____/\____/_/ |_/_/  |_|  /_____/\___/____/_/\__, /_/ /_/  /_/   /_/   \__,_/_/ /_/ /_/\___/|__/|__/\____/_/  /_/|_|  
                 /_/                                                                     /____/                                                                    
    """)

  context = build_context(csource, function, config, runmode, run_tag)

  print(
    "\n Kernel: %s \n C source: %s/benchmarks/%s \n Memory mode: %s \n Arch template: %s"
    "\n Mapper preset: %s\n Validation mode: %s\n Config: %s\n Run mode: %s\n"
    % (
      context.kernel,
      context.dfg_gen_home,
      context.csource,
      context.config.memory_mode,
      context.config.arch_template_json,
      context.config.mapper_preset,
      context.config.validation_mode,
      context.config_path,
      context.runmode,
    )
  )

  if runmode not in RUNMODES:
    raise RuntimeError("Invalid run mode '%s'. Valid values: %s" % (runmode, ", ".join(sorted(RUNMODES))))

  if dfg_xml_path:
    _stage_mapper_inputs_from_xml(context, dfg_xml_path, mem_alloc_path)
    print("Skipping DFG generation stage (XML input mode).\n")

  source_template_staged = False
  source_template_from_config = False
  if (
    source_template_report is None
    and not dfg_xml_path
    and runmode in ("runall", "dfg_gen_only")
    and context.config.source_template_mode == "auto"
  ):
    source_template_report = "auto"
    source_template_from_config = True

  if source_template_report:
    if runmode not in ("runall", "dfg_gen_only"):
      raise RuntimeError(
        "--source-template-report supports runmode runall or dfg_gen_only (got: %s)."
        % runmode
      )
    resolved_template_report = (
      _generate_source_template_report(context)
      if source_template_report == "auto" else source_template_report
    )
    source_template_staged = _try_stage_source_template(
      context,
      resolved_template_report,
      source_template_record,
      source_template_active_cell_limit,
      allow_empty_fallback=source_template_from_config,
    )

  if (
    context.config.auto_unroll_enabled
    and runmode == "runall"
    and not dfg_xml_path
    and not source_template_staged
  ):
    _run_auto_unroll(context)
    return

  if runmode == "runall_artifact_sim":
    original_enable_sim = context.config.enable_simulation
    try:
      context.config.enable_simulation = True
      run_dfg_generator(context)
      context.config.enable_simulation = False
      run_mapper(context)
    finally:
      context.config.enable_simulation = original_enable_sim
    run_artifact_simulator(context, context.config.validation_mode)
    return

  if (
    (runmode == "runall" or runmode == "dfg_gen_only")
    and not dfg_xml_path
    and not source_template_staged
  ):
    run_dfg_generator(context)

  if runmode == "runall" or runmode == "mapper_only":
    run_mapper(context)

  if runmode == "artifact_sim_only":
    run_artifact_simulator(context, context.config.validation_mode)

  if runmode == "runall" or runmode == "sim_only":
    if context.config.enable_simulation:
      run_artifact_simulator(context, context.config.validation_mode)
    else:
      print("\nSimulation disabled by config (enable_simulation=false); skipping validation stage.\n")


def _maybe_setup_kernel_run_dir(csource, is_direct, output_root):
  """Return (kernel_run_dir, timestamp) for direct invocations of structured benchmarks, else (None, None)."""
  if not is_direct:
    return None, None
  parts = csource.replace("\\", "/").split("/")
  if len(parts) != 3 or parts[2] != parts[1] + ".c":
    return None, None
  suite, kernel = parts[0], parts[1]
  timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
  root = Path(output_root)
  if not root.is_absolute():
    root = Path.cwd() / root
  kernel_run_dir = root / suite / kernel / timestamp
  kernel_run_dir.mkdir(parents=True, exist_ok=True)
  return kernel_run_dir, timestamp


def _write_run_artifacts(kernel_run_dir, csource, function, config_path, run_tag, status, rc, elapsed):
  """Write result.csv and copy config.yaml into kernel_run_dir."""
  achieved_ii = None
  ii_mapper_status = None
  try:
    with open(config_path, "r", encoding="utf-8") as f:
      cfg = yaml.safe_load(f)
    mapper_subfolder = cfg.get("mapper_subfolder", "")
    parts = csource.replace("\\", "/").split("/")
    appfolder = "/".join(parts[:2])
    result_txt = (
      Path.cwd() / "workspace" / "morpher_runs" / run_tag
      / "mapper" / mapper_subfolder / appfolder / "result.txt"
    )
    if result_txt.exists():
      lines = [l.strip() for l in result_txt.read_text(encoding="utf-8", errors="ignore").splitlines() if l.strip()]
      if lines:
        m = _RESULT_LINE_II_RE.search(lines[-1])
        if m:
          achieved_ii = int(m.group(1))
          ii_mapper_status = m.group(2)
  except Exception:
    pass

  fields = ["benchmark", "function", "status", "return_code", "achieved_ii", "ii_mapper_status", "elapsed_sec", "run_dir"]
  with (kernel_run_dir / "result.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerow({
      "benchmark": csource,
      "function": function,
      "status": status,
      "return_code": rc,
      "achieved_ii": achieved_ii if achieved_ii is not None else "",
      "ii_mapper_status": ii_mapper_status if ii_mapper_status is not None else "",
      "elapsed_sec": round(elapsed, 2),
      "run_dir": str(kernel_run_dir),
    })

  try:
    shutil.copy2(config_path, kernel_run_dir / "config.yaml")
  except OSError:
    pass


if __name__ == "__main__":
  _kernel_run_dir = None
  _csource = _function = _config = _run_tag = None
  _start = None
  _status = "ok"
  _rc = 0
  _inner_exc = None

  try:
    (
      csource, function, config, runmode, dfg_xml_path, mem_alloc_path,
      run_tag, output_root, source_template_report, source_template_record,
      source_template_active_cell_limit,
    ) = _parse_cli(sys.argv[1:])
    _csource, _function, _config = csource, function, config

    is_direct = not run_tag
    _kernel_run_dir, auto_timestamp = _maybe_setup_kernel_run_dir(csource, is_direct, output_root)
    if auto_timestamp:
      run_tag = auto_timestamp
    _run_tag = run_tag

    _start = time.time()
    main(
      csource,
      function,
      config,
      runmode,
      dfg_xml_path,
      mem_alloc_path,
      run_tag,
      source_template_report,
      source_template_record,
      source_template_active_cell_limit,
    )

  except subprocess.CalledProcessError as e:
    _status = "failed"
    _rc = e.returncode if e.returncode else 1
    _inner_exc = e
  except RuntimeError as e:
    _status = "failed"
    _rc = 1
    _inner_exc = e
  finally:
    if _kernel_run_dir is not None and _csource is not None:
      _elapsed = (time.time() - _start) if _start else 0.0
      _write_run_artifacts(_kernel_run_dir, _csource, _function, _config, _run_tag, _status, _rc, _elapsed)
      print("\nRun artifacts: %s" % _kernel_run_dir)

  if _inner_exc is not None:
    if isinstance(_inner_exc, subprocess.CalledProcessError):
      cmd = _inner_exc.cmd if isinstance(_inner_exc.cmd, str) else " ".join(_inner_exc.cmd)
      print("Error: external command failed with exit code %d." % _inner_exc.returncode)
      print("Failed command: %s" % cmd)
    else:
      print("Error: %s" % _inner_exc)
    sys.exit(_rc)
