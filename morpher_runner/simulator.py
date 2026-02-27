import re
import glob
import os
from os import listdir
from os.path import isfile, join

import numpy as np
from tqdm import tqdm

from morpher_runner.exec import Command
from morpher_runner.exec import run_command


def collect_trace_files(context):
  kernel = context.kernel
  return [
    f for f in listdir(context.mem_trace)
    if isfile(join(context.mem_trace, f)) and re.match(kernel + "_trace_[0-9]*\\.txt", f)
  ]


def select_trace_samples(files, max_test_samples):
  if len(files) > max_test_samples:
    return np.random.choice(files, size=max_test_samples, replace=False)
  return np.random.choice(files, size=len(files), replace=False)


def resolve_cmem_bin(context):
  matches = sorted(glob.glob(context.simulator_kernel + "*.bin"))
  if not matches:
    raise RuntimeError("No CGRA config binaries found in %s" % context.simulator_kernel)
  return matches[0]


def build_sim_command(context, trace_file):
  config = context.config
  kernel = context.kernel
  cmem_bin = resolve_cmem_bin(context)
  return Command(
    argv=[
      context.simulator_home + "/src/build/hycube_simulator",
      "-x",
      str(config.xdim),
      "-y",
      str(config.ydim),
      "-c",
      cmem_bin,
      "-d",
      join(context.mem_trace, trace_file),
      "-a",
      context.simulator_kernel + kernel + "_mem_alloc.txt",
      "-m",
      str(context.totalsize),
    ],
    cwd=context.simulator_kernel,
  )


def run_simulator(context):
  verbose_cmds = os.getenv("MORPHER_VERBOSE_CMDS", "").strip().lower() not in ("", "0", "false", "off", "no")
  print("\n-----Running hycube_simulator-----\n")

  files = collect_trace_files(context)
  samplefiles = select_trace_samples(files, context.config.max_test_samples)
  print("Trace samples: %d\n" % len(samplefiles))

  matches = 0
  mismatches = 0
  total = len(samplefiles)
  for idx, file in enumerate(tqdm(samplefiles), start=1):
    command = build_sim_command(context, file)
    if verbose_cmds:
      print("[sim %d/%d] %s" % (idx, total, " ".join(command.argv)))
    run_command(command)

    with open("%s/sim_result.txt" % context.simulator_kernel.rstrip("/"), "r", encoding="utf-8") as f:
      sim_result = f.read()
    sim_result_ = sim_result.split(",")
    sim_result__ = [int(e) for e in sim_result_]
    matches_, mismatches_ = sim_result__
    matches = matches + matches_
    mismatches = mismatches + mismatches_

  print("Matches: %d Mismatches: %d" % (matches, mismatches))
  if mismatches == 0:
    print("Simulation test passed!!!")
