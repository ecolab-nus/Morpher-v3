import re
import subprocess
import os

from morpher_runner.exec import Command
from morpher_runner.exec import run_command


class BankSizeTooSmallError(RuntimeError):
  def __init__(self, message, required_banksize=None):
    super().__init__(message)
    self.required_banksize = required_banksize


_REQUIRED_BANKSIZE_RE = re.compile(
  r"Required SPM bank size for provisional layout:\s*(\d+)\s*bytes",
  re.IGNORECASE,
)


def resolve_llvm_tool(tool_name):
  """Resolve an LLVM tool name, optionally using a configured bin dir or suffix."""
  bin_dir = os.environ.get("MORPHER_LLVM_BIN_DIR", "").strip()
  suffix = os.environ.get("MORPHER_LLVM_SUFFIX", "").strip()

  candidates = []
  if suffix:
    candidates.append("%s%s" % (tool_name, suffix))
  candidates.append(tool_name)

  if bin_dir:
    for candidate in candidates:
      path = os.path.join(bin_dir, candidate)
      if os.path.isfile(path) and os.access(path, os.X_OK):
        return path

  return candidates[0]


def resolve_llvm_lib_dir():
  lib_dir = os.environ.get("MORPHER_LLVM_LIB_DIR", "").strip()
  if lib_dir:
    return lib_dir

  bin_dir = os.environ.get("MORPHER_LLVM_BIN_DIR", "").strip()
  if not bin_dir:
    return ""

  candidate = os.path.normpath(os.path.join(bin_dir, "..", "lib"))
  if os.path.isdir(candidate):
    return candidate
  return ""


def build_llvm_tool_env():
  env = os.environ.copy()
  lib_dir = resolve_llvm_lib_dir()
  if not lib_dir:
    return env

  ld_library_path = env.get("LD_LIBRARY_PATH", "").strip()
  entries = [entry for entry in ld_library_path.split(":") if entry]
  if lib_dir not in entries:
    entries.insert(0, lib_dir)
  env["LD_LIBRARY_PATH"] = ":".join(entries)
  return env


def build_clang_ir_command(src_file, out_file, define_cgra_compiler=False, suppress_frontend_warnings=False, cwd=None):
  """Compile C/C++ source into LLVM IR with Morpher's expected non-vectorized flags."""
  cmd_parts = [resolve_llvm_tool("clang")]
  if define_cgra_compiler:
    cmd_parts.extend(["-D", "CGRA_COMPILER"])
  cmd_parts.extend([
    "-target", "i386-unknown-linux-gnu",
    "-c", "-emit-llvm",
    "-O2",
    "-ffp-contract=off",
    "-fno-inline",
    "-fno-vectorize", "-fno-slp-vectorize",
    "-fno-unroll-loops",
  ])
  if suppress_frontend_warnings:
    cmd_parts.extend([
      "-Wno-implicit-function-declaration",
      "-Wno-format",
      "-Wno-main-return-type",
    ])
  cmd_parts.extend([src_file, "-S", "-o", out_file])
  return Command(argv=cmd_parts, cwd=cwd, env=build_llvm_tool_env())


def find_arrays_larger_than_bank(ir_file, banksize):
  """Best-effort scan of LLVM IR globals to surface large arrays."""
  if not os.path.exists(ir_file):
    return []

  oversized = []
  ir_array_re = re.compile(r"^@([A-Za-z0-9_.$]+)\s*=.*\[(\d+)\s+x\s+i(\d+)\]")
  with open(ir_file, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
      m = ir_array_re.match(line.strip())
      if not m:
        continue
      name = m.group(1)
      count = int(m.group(2))
      bits = int(m.group(3))
      if bits % 8 != 0:
        continue
      size_bytes = count * (bits // 8)
      if size_bytes > banksize:
        oversized.append((name, size_bytes))

  oversized.sort(key=lambda x: x[1], reverse=True)
  return oversized


def suggest_larger_banksize(ir_file, banksize):
  oversized = find_arrays_larger_than_bank(ir_file, banksize)
  if oversized:
    return max(banksize + 1, oversized[0][1]), oversized
  return max(banksize + 1, banksize * 2), []


def run_dfg_opt_with_diagnostics(opt_cmd_argv, kernel, banksize, numberofbanks, cwd=None):
  """Run DFG generation pass and print friendly memory-bank diagnostics."""
  result = run_command(Command(
    argv=opt_cmd_argv,
    cwd=cwd,
    check=False,
    capture_output=True,
    text=True,
    env=build_llvm_tool_env(),
  ))
  output = (result.stdout or "") + "\n" + (result.stderr or "")
  dfg_generation_error = "DFG generation error in" in output

  if result.returncode != 0 or dfg_generation_error:
    returncode = result.returncode if result.returncode != 0 else 1
    bank_size_error = (
      "Assertion `size <= bank_size' failed" in output or
      "DFG generation error in AllocateSPMBanks" in output
    )
    unsupported_dims_error = (
      "DFG generation error in UnsupportedArrayDimensions" in output or
      "supports only up to 2D arrays" in output
    )

    if unsupported_dims_error:
      print("Error: DFG generation does not support this kernel's array dimensionality.")
      print("Morpher currently supports only up to 2D arrays for mapped memory accesses.")
      for line in output.splitlines():
        if "DFG generation error in UnsupportedArrayDimensions" in line:
          print(line)
          break
      raise subprocess.CalledProcessError(returncode, opt_cmd_argv)

    if bank_size_error:
      print("Error: DFG generation failed during SPM bank allocation.")
      print(
        "Config: numberofbanks=%d, banksize=%d bytes, total_mem=%d bytes."
        % (numberofbanks, banksize, numberofbanks * banksize)
      )
      suggested_banksize, oversized = suggest_larger_banksize("%s_opt.ll" % kernel, banksize)
      if oversized:
        print("At least one array is larger than the configured bank size.")
        print("Arrays larger than banksize (from LLVM IR):")
        for name, size_bytes in oversized[:8]:
          print("  - %s: %d bytes" % (name, size_bytes))
      else:
        print("No single oversized array was found in LLVM IR.")
        print("The allocation likely overflowed a bank due to cumulative placement.")
      print("Suggested banksize for retry: at least %d bytes.\n" % suggested_banksize)
      raise BankSizeTooSmallError(
        "DFG generation failed during SPM bank allocation.",
        required_banksize=suggested_banksize,
      )

    if result.stdout:
      print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
      print(result.stderr, end="" if result.stderr.endswith("\n") else "\n")
    raise subprocess.CalledProcessError(result.returncode, opt_cmd_argv)

  required_banksize = None
  # Preserve the useful legacy summary without flooding stdout with full pass logs.
  for line in output.splitlines():
    if "DFG node count:" in line:
      print(line)
      continue
    if "Required SPM bank size for provisional layout:" in line:
      print(line)
      match = _REQUIRED_BANKSIZE_RE.search(line)
      if match:
        required_banksize = int(match.group(1))
      continue
    if "Continuing DFG generation with effective bank size:" in line:
      print(line)

  return {
    "required_banksize": required_banksize,
  }
