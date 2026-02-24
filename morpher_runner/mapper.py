from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.fs_ops import copy_glob
from morpher_runner.fs_ops import remove_glob


def build_mapper_commands(context):
  config = context.config
  kernel = context.kernel

  commands = []
  if "_mem" in config.json_arch:
    commands.append(
      Command(
        argv=[
          "python",
          "%s/update_mem_alloc.py" % context.mapper_home,
          "%s/json_arch/%s" % (context.mapper_home, config.json_arch_before_memupdate),
          "%s_mem_alloc.txt" % kernel,
          str(config.banksize),
          str(config.numberofbanks),
          config.json_arch,
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
          config.json_arch,
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
          "%s/json_arch/%s" % (context.mapper_home, config.json_arch),
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

  return commands


def run_mapper(context):
  config = context.config

  print("\n-----Running Morpher_CGRA_Mapper-----\n")
  if "_mem" in config.json_arch:
    print("\nUpdating memory allocation..\n")

  commands = build_mapper_commands(context)
  for cmd in commands:
    run_command(cmd)
    print(" ".join(cmd.argv))

  if "_mem" in config.json_arch:
    remove_glob(context.simulator_kernel, "*.bin")
    copy_glob(context.mapper_kernel, "*.bin", context.simulator_kernel, required=True)
