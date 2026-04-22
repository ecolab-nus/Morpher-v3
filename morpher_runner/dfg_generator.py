import os
import re
import subprocess
import time

from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.exec import run_commands
from morpher_runner.fs_ops import copy_file
from morpher_runner.fs_ops import remove_glob
from morpher_runner.simd_packer import pack_simd_xml
from morpher_runner.simd_packer import remap_addr_trace
from morpher_runner.utils import build_clang_ir_command
from morpher_runner.utils import run_dfg_opt_with_diagnostics


def count_trace_files(trace_dir, kernel):
  pattern = re.compile(r"^%s_trace_[0-9]+\.txt$" % re.escape(kernel))
  count = 0
  for filename in os.listdir(trace_dir):
    if pattern.match(filename):
      count += 1
  return count


def build_ir_generation_commands(context):
  kernel = context.kernel
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
        "opt",
        "-gvn",
        "-mem2reg",
        "-memdep",
        "-memcpyopt",
        "-lcssa",
        "-loop-simplify",
        "-licm",
        "-loop-deletion",
        "-indvars",
        "-simplifycfg",
        "-mergereturn",
        "-indvars",
        "%s.ll" % kernel,
        "-S",
        "-o",
        "%s_opt.ll" % kernel,
      ],
      cwd=context.dfg_gen_kernel,
    ),
  ]


def build_unroll_command(context, unroll_factor, remainder_policy):
  kernel = context.kernel
  reduction_lanes = context.config.simd_lanes if context.config.simd_enabled else 1
  return Command(
    argv=[
      "opt",
      "-load",
      "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
      "-morpher-loop-unroll",
      "-morpher-unroll-fn",
      kernel,
      "-morpher-unroll-factor",
      str(unroll_factor),
      "-morpher-remainder-policy",
      remainder_policy,
      "-morpher-reduction-lanes",
      str(reduction_lanes),
      "-simplifycfg",
      "-loop-simplify",
      "-lcssa",
      "-instcombine",
      "-licm",
      "-gvn",
      "-dce",
      "-simplifycfg",
      "-loop-simplify",
      "-lcssa",
      "%s_opt.ll" % kernel,
      "-S",
      "-o",
      "%s_uf%d_opt.ll" % (kernel, unroll_factor),
    ],
    cwd=context.dfg_gen_kernel,
  )


def build_dfg_pass_command(context, skeleton_ir=None):
  config = context.config
  kernel = context.kernel
  if skeleton_ir is None:
    skeleton_ir = "%s_opt.ll" % kernel
  cmd = [
    "opt",
    "-load",
    "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
    "-fn",
    kernel,
    "-nobanks",
    str(config.numberofbanks),
    "-banksize",
    str(config.banksize),
    "-skeleton",
    skeleton_ir,
    "-S",
    "-o",
    "%s_opt_instrument.ll" % kernel,
  ]
  if config.remove_agi:
    cmd.append("-remove-agi")
  return cmd


def validate_dfg_outputs(context):
  kernel = context.kernel
  expected = [
    "%s_PartPredDFG.dot" % kernel,
    "%s_PartPredDFG.xml" % kernel,
    "%s_mem_alloc.txt" % kernel,
    "%s_opt_instrument.ll" % kernel,
  ]

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
  commands = [
    Command(
      argv=[
        "clang",
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
        argv=["llvm-link", "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "polybench.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
      )
    )
  else:
    commands.append(
      Command(
        argv=["llvm-link", "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
      )
    )

  commands.extend(
    [
      Command(argv=["llc", "-filetype=obj", "final.ll", "-o", "final.o"], cwd=context.dfg_gen_kernel),
      Command(argv=["clang++", "-m32", "final.o", "-o", "final"], cwd=context.dfg_gen_kernel),
    ]
  )
  return commands


def run_dfg_generator(context, unroll_factor=1, remainder_policy="preserve"):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel
  if config.simd_enabled and unroll_factor <= 1:
    unroll_factor = config.simd_lanes

  print("-----Running Morpher_DFG_Generator-----\n")
  if config.simd_enabled:
    print(
      "SIMD packing enabled: lanes=%d, pack_load_store=%s\n"
      % (config.simd_lanes, "true" if config.simd_pack_load_store else "false")
    )
  if unroll_factor > 1:
    print(
      "Requested Morpher scalar loop unroll: factor=%d, remainder_policy=%s\n"
      % (unroll_factor, remainder_policy)
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
  if unroll_factor > 1:
    print("Unrolling annotated loop for DFG candidate..\n")
    run_command(build_unroll_command(context, unroll_factor, remainder_policy))
    skeleton_ir = "%s_uf%d_opt.ll" % (kernel, unroll_factor)

  print("Generating DFG (%s_PartPredDFG.xml/dot) and data layout (%s_mem_alloc.txt)..\n" % (kernel, kernel))
  run_dfg_opt_with_diagnostics(
    build_dfg_pass_command(context, skeleton_ir=skeleton_ir),
    kernel,
    config.banksize,
    config.numberofbanks,
    cwd=dfg_kernel,
  )

  validate_dfg_outputs(context)

  run_command(Command(
    argv=["dot", "-Tpdf", "%s_PartPredDFG.dot" % kernel, "-o", "%s_PartPredDFG.pdf" % kernel],
    cwd=dfg_kernel,
  ))
  dfg_xml = "%s/%s_PartPredDFG.xml" % (dfg_kernel.rstrip("/"), kernel)
  mapper_xml = dfg_xml
  simd_mapping = None
  if config.simd_enabled:
    simd_xml = "%s/%s_simd%d_PartPredDFG.xml" % (
      dfg_kernel.rstrip("/"),
      kernel,
      config.simd_lanes,
    )
    report, simd_mapping = pack_simd_xml(
      dfg_xml,
      simd_xml,
      lanes=config.simd_lanes,
      pack_load_store=config.simd_pack_load_store,
    )
    mapper_xml = simd_xml
    print(
      "SIMD pack: input_nodes=%d, output_nodes=%d, packed_groups=%d, "
      "removed_nodes=%d, pack_ratio=%.3f\n"
      % (
        report.input_nodes,
        report.output_nodes,
        report.packed_groups,
        report.packed_nodes_removed,
        report.pack_ratio,
      )
    )
  copy_file(mapper_xml, context.mapper_kernel)
  if mapper_xml != dfg_xml:
    staged_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
    packed_name = os.path.join(context.mapper_kernel, os.path.basename(mapper_xml))
    if packed_name != staged_xml:
      os.replace(packed_name, staged_xml)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.mapper_kernel)
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
      result = subprocess.run(
        ["./final"],
        cwd=dfg_kernel,
        stdout=stdout_log,
        stderr=stderr_log,
        check=False,
        env=trace_env,
      )
  elapsed = time.time() - start_time
  trace_count = count_trace_files("%s/memtraces" % dfg_kernel.rstrip("/"), kernel)
  print("Instrumentation run complete in %.2f sec. Generated %d trace file(s)." % (elapsed, trace_count))
  if result.returncode != 0:
    printable_code = result.returncode if result.returncode >= 0 else (128 + (-result.returncode))
    print("Warning: instrumented binary ./final exited with code %d; simulation traces will not be available." % printable_code)
  else:
    trace_zero = "%s/memtraces/%s_trace_0.txt" % (dfg_kernel.rstrip("/"), kernel)
    if os.path.isfile(trace_zero):
      copy_file(trace_zero, context.simulator_kernel)
    else:
      print("Warning: no %s_trace_0.txt found; continuing with available trace files." % kernel)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.simulator_kernel)

  if simd_mapping is not None:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
      remap_addr_trace(addr_trace_path, addr_trace_path, simd_mapping)

  remove_glob(dfg_kernel, "*.ll")
