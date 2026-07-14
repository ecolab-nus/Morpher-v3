import os
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from morpher_runner.context import resolve_arch_template_path as resolve_shared_arch_template_path
from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.trace_partition import remap_addr_traces
from morpher_runner.utilization import estimate_utilization


def resolve_arch_template_path(context):
  return resolve_shared_arch_template_path(
    context.morpher_home,
    context.mapper_home,
    context.config.arch_template_json,
  )


def runtime_arch_path(context):
  return os.path.join(context.mapper_kernel, "%s_arch_runtime.json" % context.kernel)


def mem_alloc_path(context):
  return os.path.join(context.mapper_kernel, "%s_mem_alloc.txt" % context.kernel)


def _is_mapper_memory_op(op_text):
  op = (op_text or "").strip()
  if "OLOAD" in op or "OSTORE" in op:
    return False
  return "LOAD" in op or "STORE" in op


def _count_staged_dfg_ops(context):
  dfg_path = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % context.kernel)
  if not os.path.isfile(dfg_path):
    return 0, 0
  root = ET.fromstring("<Root>\n%s\n</Root>" % Path(dfg_path).read_text(encoding="utf-8"))
  mem_ops = 0
  compute_ops = 0
  for op_elem in root.findall(".//OP"):
    op_text = (op_elem.text or "").strip()
    if not op_text:
      continue
    if _is_mapper_memory_op(op_text):
      mem_ops += 1
    else:
      compute_ops += 1
  return mem_ops, compute_ops


def _parse_result_ii(result_path):
  if not result_path.is_file():
    return None
  for line in reversed(result_path.read_text(encoding="utf-8", errors="ignore").splitlines()):
    match = re.search(r"method:\S+\s+(\d+)\s+status:success\b", line.strip())
    if match:
      return int(match.group(1))
  return None


def _print_mapper_utilization(context, mapper_arch):
  ii = _parse_result_ii(Path(context.mapper_kernel) / "result.txt")
  if ii is None:
    return
  estimate = estimate_utilization(
      Path(context.mapper_kernel) / ("%s_PartPredDFG.xml" % context.kernel),
      Path(mapper_arch),
      ii,
  )
  print(
      "Utilization: compute=%s%% (%d nodes / %d PEs / II=%d), "
      "memory=%s%% (%d nodes / %d PEs / II=%d), "
      "total=%s%% (%d nodes / %d PEs / II=%d)\n"
      % (
          estimate.csv_fields()["compute_util_percent"] or "n/a",
          estimate.compute_nodes,
          estimate.compute_pes,
          ii,
          estimate.csv_fields()["memory_util_percent"] or "n/a",
          estimate.memory_nodes,
          estimate.memory_pes,
          ii,
          estimate.csv_fields()["total_util_percent"] or "n/a",
          estimate.compute_nodes + estimate.memory_nodes,
          estimate.total_pes,
          ii,
      )
  )


def resolve_pf_node_order(context):
  mode = getattr(context.config, "membound_mapping", "off")
  if mode == "off":
    return "default"
  if mode == "force":
    print("membound_mapping=force: using PF mem_first node order.")
    return "mem_first"
  if mode != "auto":
    raise RuntimeError("Unsupported membound_mapping mode: %s" % mode)

  mem_ops, compute_ops = _count_staged_dfg_ops(context)
  mem_pes = int(getattr(context.config, "arch_mem_pe_count", 0) or 0)
  total_pes = int(context.config.xdim) * int(context.config.ydim)
  compute_pes = total_pes - mem_pes
  if compute_pes <= 0:
    compute_pes = total_pes

  if mem_ops <= 0 or mem_pes <= 0 or compute_ops <= 0 or compute_pes <= 0:
    print(
      "membound_mapping=auto: using default PF node order "
      "(mem_ops=%d, mem_PEs=%d, compute_ops=%d, compute_PEs=%d)."
      % (mem_ops, mem_pes, compute_ops, compute_pes)
    )
    return "default"

  mem_ratio = float(mem_ops) / float(mem_pes)
  compute_ratio = float(compute_ops) / float(compute_pes)
  use_mem_first = mem_ratio > compute_ratio
  print(
    "membound_mapping=auto: mem_ops/mem_PEs=%.3f (%d/%d), "
    "compute_ops/compute_PEs=%.3f (%d/%d) -> pf_node_order=%s."
    % (
      mem_ratio,
      mem_ops,
      mem_pes,
      compute_ratio,
      compute_ops,
      compute_pes,
      "mem_first" if use_mem_first else "default",
    )
  )
  return "mem_first" if use_mem_first else "default"


def build_mapper_commands(context):
  config = context.config
  kernel = context.kernel
  mode = config.memory_mode
  if mode not in ("auto", "static"):
    raise RuntimeError("Unsupported memory mode: %s" % mode)

  template_arch = resolve_arch_template_path(context)
  mem_alloc = mem_alloc_path(context)
  emit_binary = False
  pf_node_order = resolve_pf_node_order(context)
  mapper_common_args = [
    "-d",
    "%s_PartPredDFG.xml" % kernel,
    "-x",
    str(config.xdim),
    "-y",
    str(config.ydim),
    "-i",
    str(config.init_II),
    "--hops",
    str(config.multihop),
    "-m",
    str(config.mapping_method),
    "-b",
    str(config.backtrack_limit),
    "-r",
    str(config.max_mapper_iter),
    "--max_II",
    str(config.max_II),
    "--ii_restarts",
    str(config.ii_restarts),
    "--skip_ii_estimation",
    "1" if config.skip_ii_estimation else "0",
    "--print_dfg",
    "1" if config.print_dfg else "0",
    "--emit_binary",
    "1" if emit_binary else "0",
    "--sa_seed",
    str(config.sa_seed),
    "--sa_restarts",
    str(config.sa_restarts),
    "--sa_moves_per_temp",
    str(config.sa_moves_per_temp),
    "--sa_temp_max",
    str(config.sa_temp_max),
    "--sa_temp_min",
    str(config.sa_temp_min),
    "--sa_candidate_topk",
    str(config.sa_candidate_topk),
    "--sa_stall_temp_steps",
    str(config.sa_stall_temp_steps),
    "--sa_stall_min_unmapped",
    str(config.sa_stall_min_unmapped),
    "--off_spm_mem_candidate_topk",
    str(config.off_spm_mem_candidate_topk),
    "--pf_candidate_topk",
    str(config.pf_candidate_topk),
    "--pf_frontier_stall_repeats",
    str(config.pf_frontier_stall_repeats),
    "--pf_frontier_stall_min_mapped",
    str(config.pf_frontier_stall_min_mapped),
    "--pf_guard_mode",
    (
      config.mapper_preset
      if config.mapper_preset in ("fast", "quality", "exhaust")
      else "balanced"
    ),
    "--rut_recovery_seed",
    str(config.rut_recovery_seed),
    "--pf_node_order",
    pf_node_order,
    "--spm_mode",
    str(config.spm_mode),
    "--proximity_sort",
    "1" if getattr(config, "proximity_sort", True) else "0",
  ]

  node_pin = getattr(config, "node_pin", "")
  if node_pin:
    mapper_common_args += ["--node_pin", str(node_pin)]

  if getattr(config, "layout_solver", False):
    mapper_common_args += ["--layout_solver", "1"]
  if getattr(config, "replicate_data", False):
    mapper_common_args += ["--replicate_data", "1"]
  bank_affinity_cost = int(getattr(config, "bank_affinity_cost", 0) or 0)
  if bank_affinity_cost:
    mapper_common_args += ["--bank_affinity_cost", str(bank_affinity_cost)]

  commands = []
  mapper_arch = template_arch
  if mode == "auto":
    mapper_arch = runtime_arch_path(context)
    commands.append(
      Command(
        argv=[
          sys.executable,
          "%s/update_mem_alloc.py" % context.mapper_home,
          template_arch,
          mem_alloc,
          str(config.banksize),
          str(config.numberofbanks),
          mapper_arch,
        ],
        cwd=context.mapper_kernel,
      )
    )
    commands.append(
      Command(
        argv=[
          "%s/build/src/cgra_xml_mapper" % context.mapper_home,
          "-j",
          mapper_arch,
          "-t",
          "HyCUBE_4REG",
        ] + mapper_common_args,
        cwd=context.mapper_kernel,
      )
    )
  else:
    commands.append(
      Command(
        argv=[
          "%s/build/src/cgra_xml_mapper" % context.mapper_home,
          "-j",
          mapper_arch,
          "-t",
          "HyCUBE_4REG",
        ] + mapper_common_args,
        cwd=context.mapper_kernel,
      )
    )

  return commands, mapper_arch


def run_mapper(context):
  config = context.config
  verbose_cmds = os.getenv("MORPHER_VERBOSE_CMDS", "").strip().lower() not in ("", "0", "false", "off", "no")

  print("\n-----Running Morpher_CGRA_Mapper-----\n")
  print("Memory mode: %s" % config.memory_mode)
  if config.memory_mode == "auto":
    print("\nUpdating memory allocation..\n")

  commands, mapper_arch = build_mapper_commands(context)
  print("Arch used by mapper: %s\n" % mapper_arch)
  print("Mapper binary emission is disabled; artifact simulation consumes the mapped DFG.\n")
  total = len(commands)
  for idx, cmd in enumerate(commands, start=1):
    if config.memory_mode == "auto" and total == 2:
      step_label = "Update memory allocation" if idx == 1 else "Run mapper"
    elif total == 1:
      step_label = "Run mapper"
    else:
      step_label = "Run stage"

    print("[%d/%d] %s" % (idx, total, step_label))
    if verbose_cmds:
      print("\n  $ %s" % shlex.join(cmd.argv))
    start = time.time()
    run_command(cmd)
    elapsed = time.time() - start
    print("  done (%.2f sec)\n" % elapsed)

  _print_mapper_utilization(context, mapper_arch)

  if getattr(config, "layout_solver", False):
    # The trace was recorded against the original layout; shift each row by
    # the per-node delta emitted by the mapper-side layout solver.
    memtraces = Path(context.mapper_kernel) / "memtraces"
    if not memtraces.exists():
      memtraces = Path(context.dfg_gen_kernel) / "memtraces"
    decision_files = sorted(Path(context.mapper_kernel).glob("*_layout_decision.json"))
    decision_path = decision_files[0] if decision_files else None
    rewritten = remap_addr_traces(memtraces, decision_path)
    if rewritten:
      print("[layout_solver] addr_trace rewrite: %d rows updated\n" % rewritten)
