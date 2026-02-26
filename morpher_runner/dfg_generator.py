import os
import re
import subprocess
import time

from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.exec import run_commands
from morpher_runner.fs_ops import copy_file
from morpher_runner.fs_ops import remove_glob
from morpher_runner.utils import build_clang_ir_command
from morpher_runner.utils import run_dfg_opt_with_diagnostics


def count_trace_files(trace_dir, kernel):
  pattern = re.compile(r"^%s_trace_[0-9]+\\.txt$" % re.escape(kernel))
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


def build_dfg_pass_command(context):
  config = context.config
  kernel = context.kernel
  return [
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
    "%s_opt.ll" % kernel,
    "-S",
    "-o",
    "%s_opt_instrument.ll" % kernel,
  ]


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


def run_dfg_generator(context):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel

  print("-----Running Morpher_DFG_Generator-----\n")

  remove_glob(dfg_kernel, "memtraces/*")
  remove_glob(dfg_kernel, "*.xml")
  remove_glob(dfg_kernel, "*.dot")

  ir_commands = build_ir_generation_commands(context)

  print("Generating IR..\n")
  run_command(ir_commands[0])

  print("Optimizing IR..\n")
  run_command(ir_commands[1])

  print("Generating DFG (%s_PartPredDFG.xml/dot) and data layout (%s_mem_alloc.txt)..\n" % (kernel, kernel))
  run_dfg_opt_with_diagnostics(
    build_dfg_pass_command(context),
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
  copy_file("%s/%s_PartPredDFG.xml" % (dfg_kernel.rstrip("/"), kernel), context.mapper_kernel)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.mapper_kernel)
  remove_glob(dfg_kernel, "*.log")

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

  remove_glob(dfg_kernel, "*.ll")
