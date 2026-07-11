#!/usr/bin/env python3
"""Check Morpher scalar-unroll semantics on local PolyBench fixtures.

This is a regression guard for a specific failure mode: reducing DFG size by
accidentally deleting scalar arithmetic.  It performs two checks:

1. Execute optimized LLVM IR before and after the Morpher unroll pass with a
   no-op please_map_me() and compare the program exit code.
2. Generate uf1 and scalar-ufN DFGs, then require scalar arithmetic ops in the
   unrolled DFG to scale by at least N for straight-line mapped bodies.

The execution check is only as strong as each benchmark's returned checksum.
The DFG arithmetic-count check catches dropped mapped-body adds/muls even when
the fixture returns only one output element.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from morpher_runner.context import build_context  # noqa: E402
from morpher_runner.dfg_generator import run_dfg_generator  # noqa: E402
from morpher_runner.utils import build_llvm_tool_env  # noqa: E402
from morpher_runner.utils import resolve_llvm_tool  # noqa: E402


POLYBENCH = [
  "atax",
  "bicg",
  "cholesky",
  "doitgen",
  "gemm",
  "gemver",
  "gesummv",
  "mm2",
  "mvt",
  "symm",
  "syr2k",
  "syrk",
  "trmm",
]

ARITH_OPS = {
  "ADD",
  "SUB",
  "MUL",
  "DIV",
  "UDIV",
  "SDIV",
  "SHL",
  "LSHR",
  "ASHR",
  "AND",
  "OR",
  "XOR",
}


def run(cmd, cwd, check=True, capture=False):
  result = subprocess.run(
    cmd,
    cwd=str(cwd),
    check=False,
    text=True,
    stdout=subprocess.PIPE if capture else None,
    stderr=subprocess.STDOUT if capture else None,
    env=build_llvm_tool_env(),
  )
  if check and result.returncode != 0:
    if capture and result.stdout:
      print(result.stdout)
    raise subprocess.CalledProcessError(result.returncode, cmd)
  return result


def compile_optimized_ir(src, kernel, out_dir):
  clang = resolve_llvm_tool("clang")
  opt = resolve_llvm_tool("opt")
  raw = out_dir / f"{kernel}.ll"
  opt_ir = out_dir / f"{kernel}_opt.ll"
  # Functional execution uses native IR because lli's ORC JIT can crash on the
  # i386 target used by Morpher DFG generation in this environment.  The DFG
  # check below still uses the normal runner path and target.
  run(
    [
      clang,
      "-D",
      "CGRA_COMPILER",
      "-c",
      "-emit-llvm",
      "-O2",
      "-fno-inline",
      "-fno-vectorize",
      "-fno-slp-vectorize",
      "-fno-unroll-loops",
      "-Wno-implicit-function-declaration",
      "-Wno-format",
      "-Wno-main-return-type",
      "-Wno-unknown-pragmas",
      str(src),
      "-S",
      "-o",
      str(raw),
    ],
    REPO,
  )
  run([opt, "-passes=default<O2>", str(raw), "-S", "-o", str(opt_ir)], REPO)
  return opt_ir


def unroll_ir(kernel, opt_ir, factor, out_dir, reduction_lanes=1,
              reduction_policy="tree", fuse=True):
  opt = resolve_llvm_tool("opt")
  out = out_dir / f"{kernel}_uf{factor}_opt.ll"
  cmd = [
    opt,
    "-load-pass-plugin",
    str(REPO / "Morpher_DFG_Generator/build/src/libdfggenPass.so"),
    "-passes=morpher-loop-unroll",
    "--morpher-unroll-fn",
    kernel,
    "--morpher-unroll-factor",
    str(factor),
    "--morpher-promote-memory-reductions",
    "--morpher-reduction-lanes",
    str(reduction_lanes),
    "--morpher-reduction-policy",
    reduction_policy,
    str(opt_ir),
    "-S",
    "-o",
    str(out),
  ]
  if fuse:
    cmd.insert(3, "--morpher-fuse-innermost-loops")
  run(cmd, REPO)
  return out


def interchange_ir(kernel, opt_ir, out_dir):
  opt = resolve_llvm_tool("opt")
  out = out_dir / f"{kernel}_li_opt.ll"
  run(
    [
      opt,
      "-load-pass-plugin",
      str(REPO / "Morpher_DFG_Generator/build/src/libdfggenPass.so"),
      "-passes=morpher-loop-interchange",
      "--morpher-unroll-fn",
      kernel,
      str(opt_ir),
      "-S",
      "-o",
      str(out),
    ],
    REPO,
  )
  return out


def execute_ir(ir, out_dir):
  clang = resolve_llvm_tool("clang")
  llvm_link = resolve_llvm_tool("llvm-link")
  lli = resolve_llvm_tool("lli")
  stub_c = out_dir / "please_map_me_stub.c"
  stub = out_dir / "please_map_me_stub.ll"
  linked = out_dir / (ir.stem + "_linked.bc")
  stub_c.write_text(
    "void please_map_me(void) {}\n"
    "void MorpherVectorAccumulatorInit(const char *name, unsigned value) {}\n"
    "void MorpherVectorAccumulatorExit(const char *name, unsigned value) {}\n",
    encoding="utf-8",
  )
  run([clang, "-c", "-emit-llvm", str(stub_c), "-S", "-o", str(stub)], REPO)
  run([llvm_link, str(ir), str(stub), "-o", str(linked)], REPO)
  result = run([lli, str(linked)], REPO, check=False, capture=True)
  return result.returncode, result.stdout


def dfg_op_counts(tag, bench, unroll_factor):
  xml = (
    REPO
    / "workspace"
    / "morpher_runs"
    / tag
    / "dfg"
    / "polybench"
    / bench
    / f"{bench}_noiter_PartPredDFG.xml"
  )
  if not xml.exists():
    xml = xml.with_name(f"{bench}_PartPredDFG.xml")
  text = xml.read_text(encoding="utf-8", errors="ignore")
  ops = Counter(op.strip() for op in re.findall(r"<OP>(.*?)</OP>", text, re.S))
  arith = sum(v for k, v in ops.items() if k in ARITH_OPS)
  return arith, ops


def run_dfg_check(bench, factor):
  # Use separate tags so failures leave inspectable artifacts.
  base_tag = f"verify_semantics_{bench}_uf1"
  uf_tag = f"verify_semantics_{bench}_uf{factor}"
  for tag, uf in ((base_tag, 1), (uf_tag, factor)):
    ctx = build_context(
      f"polybench/{bench}/{bench}.c",
      bench,
      "config/default.yaml",
      "dfg_gen_only",
      tag,
    )
    ctx.config.enable_simulation = False
    ctx.config.externalize_access = "agi_iter"
    run_dfg_generator(ctx, unroll_factor=uf)

  base_arith, base_ops = dfg_op_counts(base_tag, bench, 1)
  uf_arith, uf_ops = dfg_op_counts(uf_tag, bench, factor)
  if base_arith > 0 and uf_arith < base_arith * factor:
    raise RuntimeError(
      f"{bench}: scalar uf{factor} arithmetic shrank: "
      f"uf1={base_arith} {dict(base_ops)}, uf{factor}={uf_arith} {dict(uf_ops)}"
    )
  return base_arith, uf_arith


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("--factor", type=int, default=4)
  parser.add_argument("--bench", action="append", choices=POLYBENCH)
  parser.add_argument("--source", type=Path)
  parser.add_argument("--function")
  parser.add_argument("--reduction-lanes", type=int, default=1)
  parser.add_argument(
    "--reduction-policy",
    choices=("tree", "packed-psum", "scalar-psum", "vector-psum"),
    default="tree",
  )
  parser.add_argument("--skip-dfg", action="store_true")
  parser.add_argument("--interchange-loops", action="store_true")
  parser.add_argument("--keep-tmp", action="store_true")
  args = parser.parse_args()

  if args.reduction_lanes < 1:
    parser.error("--reduction-lanes must be >= 1")
  if args.source is not None and not args.function:
    parser.error("--source requires --function")
  if args.source is not None and args.bench:
    parser.error("--source cannot be combined with --bench")
  if args.source is not None and not args.skip_dfg:
    parser.error("--source currently requires --skip-dfg")

  if args.source is not None:
    cases = [(args.function, args.source)]
  else:
    benches = args.bench or POLYBENCH
    cases = [
      (
        bench,
        REPO / "Morpher_DFG_Generator" / "benchmarks" / "polybench" / bench / f"{bench}.c",
      )
      for bench in benches
    ]
  tmp = Path(tempfile.mkdtemp(prefix="morpher_semantics_", dir="/tmp"))
  failed = False
  try:
    for kernel, src in cases:
      opt_ir = compile_optimized_ir(src, kernel, tmp)
      transform_input = (
        interchange_ir(kernel, opt_ir, tmp)
        if args.interchange_loops else opt_ir
      )
      uf_ir = unroll_ir(
        kernel,
        transform_input,
        args.factor,
        tmp,
        reduction_lanes=args.reduction_lanes,
        reduction_policy=args.reduction_policy,
      )
      base_rc, base_stdout = execute_ir(opt_ir, tmp)
      uf_rc, uf_stdout = execute_ir(uf_ir, tmp)
      if base_rc != uf_rc or base_stdout != uf_stdout:
        failed = True
        print(
          f"FAIL {kernel}: baseline rc={base_rc}, uf{args.factor} rc={uf_rc}, "
          f"stdout_match={base_stdout == uf_stdout}"
        )
        continue
      if base_rc < 0:
        failed = True
        print(f"FAIL {kernel}: both executions crashed with signal {-base_rc}")
        continue

      msg = (
        f"OK {kernel}: rc={base_rc}, reduction_lanes={args.reduction_lanes}, "
        f"reduction_policy={args.reduction_policy}, "
        f"loop_order={'interchanged' if args.interchange_loops else 'original'}"
      )
      if not args.skip_dfg:
        base_arith, uf_arith = run_dfg_check(kernel, args.factor)
        msg += f", arith uf1={base_arith}, uf{args.factor}={uf_arith}"
      print(msg)
  finally:
    if args.keep_tmp:
      print(f"Temporary IR kept at {tmp}")
    else:
      shutil.rmtree(tmp, ignore_errors=True)

  return 1 if failed else 0


if __name__ == "__main__":
  raise SystemExit(main())
