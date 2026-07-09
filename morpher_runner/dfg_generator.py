import os
import re
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.exec import run_commands
from morpher_runner.fs_ops import copy_file
from morpher_runner.fs_ops import remove_glob
from morpher_runner.simd_packer import filter_trace_vars
from morpher_runner.simd_packer import pack_simd_xml
from morpher_runner.simd_packer import prune_iter_control_xml
from morpher_runner.simd_packer import reindex_addr_trace
from morpher_runner.simd_packer import remap_addr_trace
from morpher_runner.utils import build_clang_ir_command
from morpher_runner.utils import BankSizeTooSmallError
from morpher_runner.utils import build_llvm_tool_env
from morpher_runner.utils import resolve_llvm_tool
from morpher_runner.utils import run_dfg_opt_with_diagnostics


MEMORY_OP_PREFIXES = (
  "LOAD",
  "STORE",
  "OLOAD",
  "OSTORE",
  "VLOAD",
  "VSTORE",
  "VOLOAD",
  "VOSTORE",
)


def _read_unwrapped_xml(path):
  text = open(path, "r", encoding="utf-8").read()
  return ET.fromstring("<Root>\n%s\n</Root>" % text)


def _classify_staged_dfg_nodes(xml_path):
  root = _read_unwrapped_xml(xml_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("DFG XML missing <DFG> root in %s" % xml_path)

  counts = {
    "total": 0,
    "scalar_compute": 0,
    "vector_compute": 0,
    "memory_ops": 0,
  }
  for node in dfg.findall("Node"):
    counts["total"] += 1
    op_elem = node.find("OP")
    op = (op_elem.text or "").strip() if op_elem is not None and op_elem.text else ""
    if not op:
      continue
    if op.startswith(MEMORY_OP_PREFIXES):
      counts["memory_ops"] += 1
    elif op.startswith("V"):
      counts["vector_compute"] += 1
    else:
      counts["scalar_compute"] += 1
  return counts


def _print_staged_dfg_summary(xml_path):
  counts = _classify_staged_dfg_nodes(xml_path)
  print(
    "Staged DFG summary: total_nodes=%d, scalar_compute=%d, vector_compute=%d, memory_ops=%d\n"
    % (
      counts["total"],
      counts["scalar_compute"],
      counts["vector_compute"],
      counts["memory_ops"],
    )
  )


def count_trace_files(trace_dir, kernel):
  pattern = re.compile(r"^%s_trace_[0-9]+\.txt$" % re.escape(kernel))
  count = 0
  for filename in os.listdir(trace_dir):
    if pattern.match(filename):
      count += 1
  return count


def build_ir_generation_commands(context):
  kernel = context.kernel
  opt_tool = resolve_llvm_tool("opt")
  return [
    build_clang_ir_command(
      context.csourcefile,
      "%s.ll" % kernel,
      define_cgra_compiler=True,
      suppress_frontend_warnings=True,
      cwd=context.dfg_gen_kernel,
    ),
    Command(
      argv=[
        opt_tool,
        "-passes=default<O2>",
        "%s.ll" % kernel,
        "-S",
        "-o",
        "%s_opt.ll" % kernel,
      ],
      cwd=context.dfg_gen_kernel,
      env=build_llvm_tool_env(),
    ),
  ]


def simd_packing_active_for_factor(config, unroll_factor):
  return config.arch_simd_enabled and unroll_factor >= config.arch_simd_width


def _require_experimental_python_semantic_transforms(config, feature):
  if config.experimental_python_semantic_transforms:
    return
  raise RuntimeError(
    "%s is currently implemented as a Python XML semantic transform. "
    "Trusted flows keep XML as a serialization boundary; set "
    "experimental_python_semantic_transforms: true only for unstable experiments."
    % feature
  )


def build_unroll_command(context, unroll_factor):
  kernel = context.kernel
  simd_active = simd_packing_active_for_factor(context.config, unroll_factor)
  if simd_active:
    reduction_lanes = context.config.arch_simd_width
    reduction_policy = "packed-psum"
  elif context.config.auto_unroll_scalar_reduction_policy == "none":
    reduction_lanes = 1
    reduction_policy = "tree"
  else:
    reduction_lanes = context.config.auto_unroll_scalar_reduction_terms
    if unroll_factor > 1 and unroll_factor % reduction_lanes != 0:
      raise RuntimeError(
        "auto_unroll.scalar_reduction_terms=%d must divide unroll factor %d."
        % (reduction_lanes, unroll_factor)
      )
    reduction_policy = context.config.auto_unroll_scalar_reduction_policy
  opt_tool = resolve_llvm_tool("opt")
  command = Command(
    argv=[
      opt_tool,
      "-load-pass-plugin",
      "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
      "-passes=morpher-loop-unroll",
      "--morpher-unroll-fn",
      kernel,
      "--morpher-unroll-factor",
      str(unroll_factor),
      "--morpher-reduction-lanes",
      str(reduction_lanes),
      "--morpher-reduction-policy",
      reduction_policy,
      "%s_opt.ll" % kernel,
      "-S",
      "-o",
      "%s_uf%d_opt.ll" % (kernel, unroll_factor),
    ],
    cwd=context.dfg_gen_kernel,
    env=build_llvm_tool_env(),
  )
  if context.config.auto_unroll_fuse_innermost_loops:
    command.argv.insert(3, "--morpher-fuse-innermost-loops")
  return command


def build_dfg_pass_command(context, skeleton_ir=None):
  config = context.config
  kernel = context.kernel
  if skeleton_ir is None:
    skeleton_ir = "%s_opt.ll" % kernel
  cmd = [
    resolve_llvm_tool("opt"),
    "-load-pass-plugin",
    "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
    "-passes=skeleton",
    "--fn",
    kernel,
    "--nobanks",
    str(config.numberofbanks),
    "--banksize",
    str(config.banksize),
    "-S",
    skeleton_ir,
    "-o",
    "%s_opt_instrument.ll" % kernel,
  ]
  if config.remove_agi:
    cmd.append("--remove-agi")
  if config.suppress_dfg_dot:
    cmd.append("--suppress-dfg-dot")
  return cmd


def _can_auto_grow_banksize(context):
  if context.config.memory_mode == "auto":
    return True
  return str(context.config.spm_mode).strip().lower() != "on"


def _run_dfg_pass_with_banksize_retry(context, skeleton_ir):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel
  original_banksize = config.banksize
  max_attempts = 8
  last_error = None

  for attempt in range(1, max_attempts + 1):
    if attempt > 1:
      print(
        "Retrying DFG generation with larger banksize=%d bytes (attempt %d/%d).\n"
        % (config.banksize, attempt, max_attempts)
      )
    try:
      diagnostics = run_dfg_opt_with_diagnostics(
        build_dfg_pass_command(context, skeleton_ir=skeleton_ir),
        kernel,
        config.banksize,
        config.numberofbanks,
        cwd=dfg_kernel,
      )
      required_banksize = diagnostics.get("required_banksize")
      if required_banksize and required_banksize > config.banksize:
        if not _can_auto_grow_banksize(context):
          raise RuntimeError(
            "Provisional DFG layout requires banksize=%d bytes, but memory_mode=%s with spm_mode=%s "
            "cannot auto-grow the architecture at mapper time."
            % (required_banksize, context.config.memory_mode, context.config.spm_mode)
          )
        config.banksize = required_banksize
      if config.banksize != original_banksize:
        print(
          "DFG bank allocation required banksize=%d bytes; continuing with that effective size.\n"
          % config.banksize
        )
      return
    except BankSizeTooSmallError as exc:
      last_error = exc
      if not _can_auto_grow_banksize(context):
        print(
          "Auto-growing banksize is disabled for memory_mode=%s with spm_mode=%s because the selected "
          "static architecture JSON would become inconsistent.\n"
          % (context.config.memory_mode, context.config.spm_mode)
        )
        raise
      next_banksize = exc.required_banksize or (config.banksize * 2)
      if next_banksize <= config.banksize:
        next_banksize = config.banksize + 1
      config.banksize = next_banksize

  if last_error is not None:
    raise last_error


def validate_dfg_outputs(context):
  kernel = context.kernel
  expected = [
    "%s_PartPredDFG.xml" % kernel,
    "%s_mem_alloc.txt" % kernel,
    "%s_opt_instrument.ll" % kernel,
  ]
  if not context.config.suppress_dfg_dot:
    expected.insert(0, "%s_PartPredDFG.dot" % kernel)

  missing = []
  for filename in expected:
    path = "%s/%s" % (context.dfg_gen_kernel.rstrip("/"), filename)
    try:
      with open(path, "r", encoding="utf-8", errors="ignore"):
        pass
    except OSError:
      missing.append(filename)

  if missing:
    raise RuntimeError(
      "DFG generation step finished without required output files: %s. "
      "This can happen when memory-bank allocation fails; increase banksize/numberofbanks."
      % ", ".join(missing)
    )


def build_instrumentation_commands(context):
  kernel = context.kernel
  clang_tool = resolve_llvm_tool("clang")
  llvm_link_tool = resolve_llvm_tool("llvm-link")
  llc_tool = resolve_llvm_tool("llc")
  clangxx_tool = resolve_llvm_tool("clang++")
  commands = [
    Command(
      argv=[
        clang_tool,
        "-target",
        "i386-unknown-linux-gnu",
        "-c",
        "-emit-llvm",
        "-S",
        "%s/src/instrumentation/instrumentation.cpp" % context.dfg_gen_home,
        "-o",
        "instrumentation.ll",
      ],
      cwd=context.dfg_gen_kernel,
      env=build_llvm_tool_env(),
    )
  ]

  if kernel == "kernel_symm":
    commands.append(
      build_clang_ir_command(
        "polybench.c",
        "polybench.ll",
        define_cgra_compiler=True,
        cwd=context.dfg_gen_kernel,
      )
    )
    commands.append(
      Command(
        argv=[llvm_link_tool, "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "polybench.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
        env=build_llvm_tool_env(),
      )
    )
  else:
    commands.append(
      Command(
        argv=[llvm_link_tool, "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
        env=build_llvm_tool_env(),
      )
    )

  commands.extend(
    [
      Command(argv=[llc_tool, "-filetype=obj", "final.ll", "-o", "final.o"], cwd=context.dfg_gen_kernel, env=build_llvm_tool_env()),
      Command(argv=[clangxx_tool, "-m32", "-no-pie", "final.o", "-o", "final"], cwd=context.dfg_gen_kernel, env=build_llvm_tool_env()),
    ]
  )
  return commands


def _default_unroll_factor(config):
  if config.auto_unroll_enabled:
    return sorted(config.auto_unroll_factors, reverse=True)[0]
  return 1


def _unroll_pass_required(config, unroll_factor):
  return unroll_factor > 1


def run_dfg_generator(context, unroll_factor=None):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel
  if unroll_factor is None:
    unroll_factor = _default_unroll_factor(config)
  simd_active = simd_packing_active_for_factor(config, unroll_factor)
  context.selected_simd_enabled = simd_active
  context.selected_simd_width = config.arch_simd_width if simd_active else 1
  context.selected_simd_artifact_only = simd_active

  print("-----Running Morpher_DFG_Generator-----\n")
  if config.arch_simd_enabled:
    if simd_active:
      print(
        "Architecture-driven SIMD packing enabled for this candidate: width=%d, scalar_broadcast=%s\n"
        % (config.arch_simd_width, "true" if config.arch_scalar_broadcast else "false")
      )
    else:
      print(
        "SIMD hardware enabled, but this candidate is narrower than SIMD_WIDTH=%d; "
        "mapping scalar lanes without SIMD packing.\n"
        % config.arch_simd_width
      )
  if unroll_factor > 1:
    print(
      "Requested Morpher scalar loop unroll: factor=%d (remainders preserved)\n"
      % unroll_factor
    )
  if config.auto_unroll_fuse_innermost_loops and config.auto_unroll_enabled:
    print(
      "Requested Morpher innermost loop fusion where LLVM can prove legality.\n"
    )
  if (
      unroll_factor > 1
      and not simd_active
      and config.auto_unroll_scalar_reduction_policy != "none"):
    print(
      "Requested scalar reduction lowering: policy=%s terms=%d\n"
      % (
        config.auto_unroll_scalar_reduction_policy,
        config.auto_unroll_scalar_reduction_terms,
      )
    )

  remove_glob(dfg_kernel, "memtraces/*")
  remove_glob(dfg_kernel, "*.xml")
  remove_glob(dfg_kernel, "*.dot")

  ir_commands = build_ir_generation_commands(context)

  print("Generating IR..\n")
  run_command(ir_commands[0])

  print("Optimizing IR..\n")
  run_command(ir_commands[1])

  skeleton_ir = "%s_opt.ll" % kernel
  if _unroll_pass_required(config, unroll_factor):
    print("Unrolling annotated loop for DFG candidate..\n")
    run_command(build_unroll_command(context, unroll_factor))
    skeleton_ir = "%s_uf%d_opt.ll" % (kernel, unroll_factor)

  dfg_artifact_label = "xml" if config.suppress_dfg_dot else "xml/dot"
  print("Generating DFG (%s_PartPredDFG.%s) and data layout (%s_mem_alloc.txt)..\n" % (kernel, dfg_artifact_label, kernel))
  _run_dfg_pass_with_banksize_retry(context, skeleton_ir)

  validate_dfg_outputs(context)

  if config.generate_dfg_pdf and not config.suppress_dfg_dot:
    run_command(Command(
      argv=["dot", "-Tpdf", "%s_PartPredDFG.dot" % kernel, "-o", "%s_PartPredDFG.pdf" % kernel],
      cwd=dfg_kernel,
    ))
  elif not config.generate_dfg_pdf:
    print("DFG PDF generation disabled (generate_dfg_pdf=false).\n")
  else:
    print("DFG PDF generation skipped because DOT emission is disabled.\n")
  dfg_xml = "%s/%s_PartPredDFG.xml" % (dfg_kernel.rstrip("/"), kernel)
  mapper_xml = dfg_xml
  simd_mapping = None
  scalar_iter_reindex: "dict[int, int]" = {}
  if config.remove_iter and not simd_active:
    _require_experimental_python_semantic_transforms(config, "remove_iter")
    pruned_xml = "%s/%s_noiter_PartPredDFG.xml" % (dfg_kernel.rstrip("/"), kernel)
    n, scalar_iter_reindex = prune_iter_control_xml(dfg_xml, pruned_xml)
    print("Iter-control prune (scalar): removed %d nodes\n" % n)
    mapper_xml = pruned_xml
  if simd_active:
    _require_experimental_python_semantic_transforms(config, "SIMD XML packing")
    simd_xml = "%s/%s_simd%d_PartPredDFG.xml" % (
      dfg_kernel.rstrip("/"),
      kernel,
      config.arch_simd_width,
    )
    report, simd_mapping = pack_simd_xml(
      dfg_xml,
      simd_xml,
      lanes=config.arch_simd_width,
      vector_ops=config.arch_vector_ops,
      scalar_broadcast=config.arch_scalar_broadcast,
      prune_scalar_lane_outputs=config.remove_agi,
      prune_iter_control=config.remove_iter,
    )
    mapper_xml = simd_xml
    print(
      "SIMD pack: input_nodes=%d, output_nodes=%d, packed_groups=%d, "
      "removed_nodes=%d, pruned_artifact_nodes=%d, pack_ratio=%.3f\n"
      % (
        report.input_nodes,
        report.output_nodes,
        report.packed_groups,
        report.packed_nodes_removed,
        report.artifact_nodes_pruned,
        report.pack_ratio,
      )
    )
  copy_file(mapper_xml, context.mapper_kernel)
  if mapper_xml != dfg_xml:
    staged_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
    packed_name = os.path.join(context.mapper_kernel, os.path.basename(mapper_xml))
    if packed_name != staged_xml:
      os.replace(packed_name, staged_xml)
  staged_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
  _print_staged_dfg_summary(staged_xml)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.mapper_kernel)
  # Stage the AGI sub-graph next to the DFG XML so the layout solver can find it.
  for agi_path in Path(dfg_kernel).glob("*_AGI_subgraph.xml"):
    copy_file(str(agi_path), context.mapper_kernel)
  remove_glob(dfg_kernel, "*.log")

  if not config.enable_simulation:
    print("Simulation disabled (enable_simulation=false): skipping code instrumentation and trace generation.\n")
    remove_glob(dfg_kernel, "*.ll")
    return


  print("\nCode instrumentation..\n")
  run_commands(build_instrumentation_commands(context))

  trace_limit = int(config.max_test_samples)
  if trace_limit < 0:
    raise RuntimeError("max_test_samples must be >= 0, got %d" % trace_limit)

  trace_env = os.environ.copy()
  trace_env["MORPHER_MAX_TRACES"] = str(trace_limit)
  print("Trace generation limit (MORPHER_MAX_TRACES): %d" % trace_limit)
  print("Running instrumented code to generate the data memory content (memtraces/%s_trace_x.txt)..\n" % kernel)
  start_time = time.time()
  with open("%s/final_log.txt" % dfg_kernel.rstrip("/"), "w", encoding="utf-8") as stdout_log:
    with open("%s/final_err_log.txt" % dfg_kernel.rstrip("/"), "w", encoding="utf-8") as stderr_log:
      result = run_command(Command(
        argv=["./final"],
        cwd=dfg_kernel,
        stdout=stdout_log,
        stderr=stderr_log,
        check=False,
        env=trace_env,
      )
      )
  elapsed = time.time() - start_time
  trace_count = count_trace_files("%s/memtraces" % dfg_kernel.rstrip("/"), kernel)
  print("Instrumentation run complete in %.2f sec. Generated %d trace file(s)." % (elapsed, trace_count))
  if result.returncode != 0:
    printable_code = result.returncode if result.returncode >= 0 else (128 + (-result.returncode))
    print("Warning: instrumented binary ./final exited with code %d; simulation traces will not be available." % printable_code)
  elif trace_count == 0 and unroll_factor > 1:
    print(
      "Warning: 0 trace files generated despite a clean instrumented run.\n"
      "  The unroll factor (%d) likely exceeds the runtime loop trip count: the\n"
      "  unrolled body never executes, only the scalar remainder. Lower\n"
      "  `auto_unroll.factors` (e.g. [4] or [2]) or use a larger benchmark.\n"
      "  Downstream simulation will be skipped." % unroll_factor
    )
  else:
    trace_zero = "%s/memtraces/%s_trace_0.txt" % (dfg_kernel.rstrip("/"), kernel)
    if os.path.isfile(trace_zero):
      copy_file(trace_zero, context.simulator_kernel)
    else:
      print("Warning: no %s_trace_0.txt found; continuing with available trace files." % kernel)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.simulator_kernel)

  if scalar_iter_reindex:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
      reindex_addr_trace(addr_trace_path, addr_trace_path, scalar_iter_reindex)
  if simd_mapping is not None:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    for trace_path in sorted(_glob.glob("%s/%s_trace_*.txt" % (addr_trace_dir, kernel))):
      filter_trace_vars(trace_path, trace_path, report.artifact_vars_pruned)
    for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
      remap_addr_trace(addr_trace_path, addr_trace_path, simd_mapping)

  remove_glob(dfg_kernel, "*.ll")
