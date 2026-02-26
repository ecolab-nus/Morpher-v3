import os
import time

from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.fs_ops import copy_glob
from morpher_runner.fs_ops import remove_glob


def resolve_arch_template_path(context):
  template = context.config.arch_template_json
  if os.path.isabs(template):
    if os.path.isfile(template):
      return template
    raise RuntimeError("Architecture template not found: %s" % template)

  candidates = [
    os.path.join(context.mapper_home, "json_arch", template),
    os.path.join(context.morpher_home, template),
  ]
  for candidate in candidates:
    if os.path.isfile(candidate):
      return candidate

  raise RuntimeError(
    "Architecture template '%s' not found. Checked: %s"
    % (template, ", ".join(candidates))
  )


def runtime_arch_path(context):
  return os.path.join(context.mapper_kernel, "%s_arch_runtime.json" % context.kernel)


def mem_alloc_path(context):
  return os.path.join(context.mapper_kernel, "%s_mem_alloc.txt" % context.kernel)


def build_mapper_commands(context):
  config = context.config
  kernel = context.kernel
  mode = config.memory_mode
  if mode not in ("auto", "static"):
    raise RuntimeError("Unsupported memory mode: %s" % mode)

  template_arch = resolve_arch_template_path(context)
  mem_alloc = mem_alloc_path(context)

  commands = []
  mapper_arch = template_arch
  if mode == "auto":
    mapper_arch = runtime_arch_path(context)
    commands.append(
      Command(
        argv=[
          "python",
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
          "-d",
          "%s_PartPredDFG.xml" % kernel,
          "-x",
          str(config.xdim),
          "-y",
          str(config.ydim),
          "-j",
          mapper_arch,
          "-i",
          str(config.init_II),
          "-t",
          "HyCUBE_4REG",
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
          "--spm_mode",
          str(config.spm_mode),
        ],
        cwd=context.mapper_kernel,
      )
    )
  else:
    commands.append(
      Command(
        argv=[
          "%s/build/src/cgra_xml_mapper" % context.mapper_home,
          "-d",
          "%s_PartPredDFG.xml" % kernel,
          "-x",
          str(config.xdim),
          "-y",
          str(config.ydim),
          "-j",
          mapper_arch,
          "-i",
          str(config.init_II),
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
          "--spm_mode",
          str(config.spm_mode),
        ],
        cwd=context.mapper_kernel,
      )
    )

  return commands, mapper_arch


def run_mapper(context):
  config = context.config

  print("\n-----Running Morpher_CGRA_Mapper-----\n")
  print("Memory mode: %s" % config.memory_mode)
  if config.memory_mode == "auto":
    print("\nUpdating memory allocation..\n")

  commands, mapper_arch = build_mapper_commands(context)
  print("Arch used by mapper: %s\n" % mapper_arch)
  total = len(commands)
  for idx, cmd in enumerate(commands, start=1):
    print("Running command [%d/%d]: %s" % (idx, total, " ".join(cmd.argv)))
    start = time.time()
    run_command(cmd)
    elapsed = time.time() - start
    print("Command [%d/%d] completed in %.2f sec\n" % (idx, total, elapsed))

  remove_glob(context.simulator_kernel, "*.bin")
  copy_glob(context.mapper_kernel, "*.bin", context.simulator_kernel, required=True)
