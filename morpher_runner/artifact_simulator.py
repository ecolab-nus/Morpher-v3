import glob
import os
import re
from os import listdir
from os.path import isfile, join

import numpy as np

from morpher_runner.exec import Command
from morpher_runner.exec import run_command


def collect_trace_files(context):
  kernel = context.kernel
  pattern = re.compile(r"^%s_trace_[0-9]+\.txt$" % re.escape(kernel))
  return [
    f for f in listdir(context.mem_trace)
    if isfile(join(context.mem_trace, f)) and pattern.match(f)
  ]


def select_trace_samples(files, max_test_samples):
  if len(files) > max_test_samples:
    return np.random.choice(files, size=max_test_samples, replace=False)
  return np.random.choice(files, size=len(files), replace=False)


def resolve_artifact_simulator(context):
  candidates = [
    os.path.join(context.morpher_home, "simulator", "artifact_simulator", "build", "artifact_simulator"),
    os.path.join(context.morpher_home, "simulator", "artifact_simulator", "build", "src", "artifact_simulator"),
  ]
  for candidate in candidates:
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
      return candidate
  raise RuntimeError(
    "artifact_simulator executable not found. Build it with: "
    "cmake -S simulator/artifact_simulator -B simulator/artifact_simulator/build && "
    "cmake --build simulator/artifact_simulator/build"
  )


def resolve_mapping_file(context, validation_mode):
  matches = sorted(
    glob.glob(os.path.join(context.mapper_kernel, "*.mappingwithlatency.txt")),
    key=os.path.getmtime,
    reverse=True,
  )
  if not matches:
    raise RuntimeError("No mapper latency artifact found in %s" % context.mapper_kernel)
  for match in matches:
    if os.path.getsize(match) > 0:
      return match
  # Some mapper runs emit an empty mappingwithlatency file but still emit
  # mapping_i.txtmapping.json; the C++ parser handles that fallback.
  return matches[0]


def resolve_mem_alloc_file(context):
  latest = sorted(
    glob.glob(os.path.join(context.mapper_kernel, "*_mem_alloc.txt")),
    key=os.path.getmtime,
    reverse=True,
  )
  for match in latest:
    if os.path.getsize(match) > 0:
      return match
  fallback = os.path.join(context.mapper_kernel, "%s_mem_alloc.txt" % context.kernel)
  if os.path.isfile(fallback):
    return fallback
  raise RuntimeError("No mem_alloc artifact found in %s" % context.mapper_kernel)


def resolve_simd_manifest(context):
  matches = sorted(
    glob.glob(os.path.join(context.mapper_kernel, "*_SIMD_manifest.json"))
  )
  if len(matches) > 1:
    raise RuntimeError(
      "Expected at most one SIMD manifest in %s, found %d."
      % (context.mapper_kernel, len(matches))
    )
  return matches[0] if matches else None


def build_artifact_sim_command(context, trace_file, validation_mode):
  kernel = context.kernel
  argv = [
      resolve_artifact_simulator(context),
      "--dfg",
      os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel),
      "--mapping",
      resolve_mapping_file(context, validation_mode),
      "--mem-alloc",
      resolve_mem_alloc_file(context),
      "--trace",
      os.path.join(context.mem_trace, trace_file),
  ]
  addr_trace = trace_file.replace("_trace_", "_addr_trace_")
  addr_trace_path = os.path.join(context.mem_trace, addr_trace)
  if os.path.isfile(addr_trace_path):
    argv.extend(["--addr-trace", addr_trace_path])
  simd_manifest_path = resolve_simd_manifest(context)
  if simd_manifest_path is not None:
    argv.extend(["--simd-manifest", simd_manifest_path])
  if validation_mode == "event_driven":
    argv.extend([
      "--arch",
      os.path.join(context.mapper_kernel, "%s_arch_runtime.json" % kernel),
      "--result",
      os.path.join(context.mapper_kernel, "result.txt"),
    ])
  return Command(
    argv=argv,
    cwd=context.mapper_kernel,
  )


def run_artifact_simulator(context, validation_mode="functional"):
  print("\n-----Running Morpher artifact_simulator (%s)-----\n" % validation_mode)
  files = collect_trace_files(context)
  if not files:
    print(
      "Artifact simulation SKIPPED: no trace files in %s for kernel '%s'.\n"
      "  (Common cause: unroll factor exceeds the runtime loop trip count, so\n"
      "  the instrumented binary's unrolled body never executes. See the\n"
      "  DFG-generation warning earlier in this run.)"
      % (context.mem_trace, context.kernel)
    )
    return
  samplefiles = select_trace_samples(files, context.config.max_test_samples)
  print("Trace samples: %d\n" % len(samplefiles))

  matches = 0
  mismatches = 0
  for file in samplefiles:
    command = build_artifact_sim_command(context, file, validation_mode)
    command.check = False
    command.capture_output = True
    command.text = True
    result = run_command(command)
    output = (result.stdout or "") + "\n" + (result.stderr or "")
    print(output, end="" if output.endswith("\n") else "\n")
    parsed = _parse_result(output)
    if parsed is None:
      raise RuntimeError("artifact_simulator did not print a parseable Simulation Result.")
    matches += parsed[0]
    mismatches += parsed[1]
    if result.returncode not in (0, 2):
      raise RuntimeError("artifact_simulator failed with exit code %d." % result.returncode)

  print("Matches: %d Mismatches: %d" % (matches, mismatches))
  if mismatches == 0:
    print("Artifact simulation test passed!!!")


def _parse_result(output):
  import re
  match = re.search(r"Simulation Result:\s*Matches::\s*(\d+)\s*,\s*Mismatches::\s*(\d+)", output)
  if not match:
    return None
  return int(match.group(1)), int(match.group(2))
