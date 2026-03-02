#!/usr/bin/env python
import os
import shutil
import subprocess
import sys

from morpher_runner.context import build_context
from morpher_runner.dfg_generator import run_dfg_generator
from morpher_runner.mapper import run_mapper
from morpher_runner.simulator import run_simulator

DEFAULT_CONFIG_PATH = "config/default_config.yaml"
RUNMODES = {"runall", "dfg_gen_only", "mapper_only", "sim_only"}


def _derive_kernel_from_xml(xml_path):
  stem = os.path.splitext(os.path.basename(xml_path))[0]
  for suffix in ("_PartPredDFG", "_FullPredDFG", "_DFG"):
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
    "Run modes: runall, dfg_gen_only, mapper_only, sim_only\n"
    "XML mode defaults: function=<derived-from-xml>, config_path=%s, runmode=mapper_only\n"
    % DEFAULT_CONFIG_PATH
  )


def _parse_cli(argv):
  positional = []
  mem_alloc_path = None

  i = 0
  while i < len(argv):
    token = argv[i]
    if token == "--mem-alloc":
      if i + 1 >= len(argv):
        raise RuntimeError("Missing value for --mem-alloc.\n\n%s" % _usage())
      mem_alloc_path = argv[i + 1]
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
    if len(positional) > 4:
      raise RuntimeError("Too many positional arguments for XML mode.\n\n%s" % _usage())

    xml_path = first
    function = positional[1] if len(positional) >= 2 else _derive_kernel_from_xml(xml_path)
    config = positional[2] if len(positional) >= 3 else DEFAULT_CONFIG_PATH
    runmode = positional[3] if len(positional) >= 4 else "mapper_only"

    if runmode not in ("mapper_only", "runall"):
      raise RuntimeError(
        "XML mode supports runmode 'mapper_only' or 'runall' (got: %s)." % runmode
      )

    csource = "xml_inputs/%s/%s.c" % (function, function)
    return csource, function, config, runmode, xml_path, mem_alloc_path

  if len(positional) < 2 or len(positional) > 4:
    raise RuntimeError("Invalid arguments.\n\n%s" % _usage())
  if mem_alloc_path:
    raise RuntimeError("--mem-alloc can only be used when the first argument is an XML file.")

  csource = positional[0]
  function = positional[1]
  config = positional[2] if len(positional) >= 3 else DEFAULT_CONFIG_PATH
  runmode = positional[3] if len(positional) >= 4 else "runall"
  return csource, function, config, runmode, None, None


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


def main(
  csource,
  function,
  config=DEFAULT_CONFIG_PATH,
  runmode="runall",
  dfg_xml_path=None,
  mem_alloc_path=None,
):
  print(r"""
    __  ___                 __                 ________________  ___       ____            _                ______                                             __  
   /  |/  /___  _________  / /_  ___  _____   / ____/ ____/ __ \/   |     / __ \___  _____(_)___ _____     / ____/________ _____ ___  ___ _      ______  _____/ /__
  / /|_/ / __ \/ ___/ __ \/ __ \/ _ \/ ___/  / /   / / __/ /_/ / /| |    / / / / _ \/ ___/ / __ `/ __ \   / /_  / ___/ __ `/ __ `__ \/ _ \ | /| / / __ \/ ___/ //_/
 / /  / / /_/ / /  / /_/ / / / /  __/ /     / /___/ /_/ / _, _/ ___ |   / /_/ /  __(__  ) / /_/ / / / /  / __/ / /  / /_/ / / / / / /  __/ |/ |/ / /_/ / /  / ,<   
/_/  /_/\____/_/  / .___/_/ /_/\___/_/      \____/\____/_/ |_/_/  |_|  /_____/\___/____/_/\__, /_/ /_/  /_/   /_/   \__,_/_/ /_/ /_/\___/|__/|__/\____/_/  /_/|_|  
                 /_/                                                                     /____/                                                                    
    """)

  context = build_context(csource, function, config, runmode)

  print(
    "\n Kernel: %s \n C source: %s/benchmarks/%s \n Memory mode: %s \n Arch template: %s"
    "\n Config: %s\n Run mode: %s\n"
    % (
      context.kernel,
      context.dfg_gen_home,
      context.csource,
      context.config.memory_mode,
      context.config.arch_template_json,
      context.config_path,
      context.runmode,
    )
  )

  if runmode not in RUNMODES:
    raise RuntimeError("Invalid run mode '%s'. Valid values: %s" % (runmode, ", ".join(sorted(RUNMODES))))

  if dfg_xml_path:
    _stage_mapper_inputs_from_xml(context, dfg_xml_path, mem_alloc_path)
    print("Skipping DFG generation stage (XML input mode).\n")

  if (runmode == "runall" or runmode == "dfg_gen_only") and not dfg_xml_path:
    run_dfg_generator(context)

  if runmode == "runall" or runmode == "mapper_only":
    run_mapper(context)

  if runmode == "runall" or runmode == "sim_only":
    if context.config.enable_simulation:
      run_simulator(context)
    else:
      print("\nSimulation disabled by config (enable_simulation=false); skipping hycube_simulator.\n")


if __name__ == "__main__":
  try:
    csource, function, config, runmode, dfg_xml_path, mem_alloc_path = _parse_cli(sys.argv[1:])
    main(csource, function, config, runmode, dfg_xml_path, mem_alloc_path)
  except subprocess.CalledProcessError as e:
    cmd = e.cmd if isinstance(e.cmd, str) else " ".join(e.cmd)
    print("Error: external command failed with exit code %d." % e.returncode)
    print("Failed command: %s" % cmd)
    sys.exit(e.returncode if e.returncode else 1)
  except RuntimeError as e:
    print("Error: %s" % e)
    sys.exit(1)
