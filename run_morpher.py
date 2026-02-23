#!/usr/bin/env python
import subprocess
import sys

from morpher_runner.context import build_context
from morpher_runner.dfg_generator import run_dfg_generator
from morpher_runner.mapper import run_mapper
from morpher_runner.simulator import run_simulator


def main(csource, function, config="config/default_config.yaml", runmode="runall"):
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
    "\n Kernel: %s \n C source: %s/benchmarks/%s \n CGRA arch: %s/json_arch/%s "
    "\n Config: %s\n Run mode: %s\n"
    % (
      context.kernel,
      context.dfg_gen_home,
      context.csource,
      context.mapper_home,
      context.config.json_arch,
      context.config_path,
      context.runmode,
    )
  )

  if runmode == "runall" or runmode == "dfg_gen_only":
    run_dfg_generator(context)

  if runmode == "runall" or runmode == "mapper_only":
    run_mapper(context)

  if runmode == "runall" or runmode == "sim_only":
    run_simulator(context)


if __name__ == "__main__":
  csource = sys.argv[1]
  function = sys.argv[2]
  config = sys.argv[3] if len(sys.argv) >= 4 else "config/default_config.yaml"
  runmode = sys.argv[4] if len(sys.argv) >= 5 else "runall"

  try:
    main(csource, function, config, runmode)
  except subprocess.CalledProcessError as e:
    cmd = e.cmd if isinstance(e.cmd, str) else " ".join(e.cmd)
    print("Error: external command failed with exit code %d." % e.returncode)
    print("Failed command: %s" % cmd)
    sys.exit(e.returncode if e.returncode else 1)
  except RuntimeError as e:
    print("Error: %s" % e)
    sys.exit(1)
