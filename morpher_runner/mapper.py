import os
import shlex
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from morpher_runner.context import resolve_arch_template_path as resolve_shared_arch_template_path
from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.fs_ops import copy_glob
from morpher_runner.fs_ops import remove_glob
from morpher_runner.trace_partition import remap_addr_traces


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


def _staged_dfg_contains_vector_ops(context):
  dfg_path = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % context.kernel)
  if not os.path.isfile(dfg_path):
    return False
  root = ET.fromstring("<Root>\n%s\n</Root>" % Path(dfg_path).read_text(encoding="utf-8"))
  for op_elem in root.findall(".//OP"):
    op_text = (op_elem.text or "").strip()
    if op_text.startswith("V"):
      return True
  return False


def build_mapper_commands(context):
  config = context.config
  kernel = context.kernel
  mode = config.memory_mode
  if mode not in ("auto", "static"):
    raise RuntimeError("Unsupported memory mode: %s" % mode)

  template_arch = resolve_arch_template_path(context)
  mem_alloc = mem_alloc_path(context)
  context.selected_simd_artifact_only = (
    getattr(context, "selected_simd_artifact_only", False)
    or _staged_dfg_contains_vector_ops(context)
  )
  emit_binary = config.enable_simulation and not context.selected_simd_artifact_only
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
  if getattr(context, "selected_simd_artifact_only", False):
    print("Vectorized DFG selected: skipping legacy binary emission for this mapper run.\n")
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

  if config.enable_simulation and not getattr(context, "selected_simd_artifact_only", False):
    remove_glob(context.simulator_kernel, "*.bin")
    copy_glob(context.mapper_kernel, "*.bin", context.simulator_kernel, required=True)

  if getattr(config, "layout_solver", False):
    # Layout solver may have rewritten the SPM placement and re-baked the
    # binary's LD/ST constants. The addr_trace was recorded against the
    # original layout; shift each row by the per-node delta the solver emitted.
    memtraces = Path(context.mapper_kernel) / "memtraces"
    if not memtraces.exists():
      memtraces = Path(context.dfg_gen_kernel) / "memtraces"
    rewritten = remap_addr_traces(memtraces)
    if rewritten:
      print("[layout_solver] addr_trace rewrite: %d rows updated\n" % rewritten)
    if config.enable_simulation and not getattr(context, "selected_simd_artifact_only", False):
      remove_glob(context.simulator_kernel, "*_mem_alloc.txt")
      copy_glob(context.mapper_kernel, "*_mem_alloc.txt", context.simulator_kernel)
