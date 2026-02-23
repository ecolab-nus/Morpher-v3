import os
from dataclasses import dataclass

import yaml


@dataclass
class RunnerConfig:
  json_arch_before_memupdate: str
  json_arch: str
  mapper_subfolder: str
  init_II: int
  ydim: int
  xdim: int
  numberofbanks: int
  banksize: int
  max_test_samples: int
  mapping_method: int
  backtrack_limit: int
  max_mapper_iter: int
  max_II: int
  llvm_debug_type: str

  @classmethod
  def from_yaml(cls, config_path):
    with open(config_path, "r", encoding="utf-8") as stream:
      config_dict = yaml.safe_load(stream)
    return cls(
      json_arch_before_memupdate=config_dict["json_arch_before_memupdate"],
      json_arch=config_dict["json_arch"],
      mapper_subfolder=config_dict["mapper_subfolder"],
      init_II=config_dict["init_II"],
      ydim=config_dict["ydim"],
      xdim=config_dict["xdim"],
      numberofbanks=config_dict["numberofbanks"],
      banksize=config_dict["banksize"],
      max_test_samples=config_dict["max_test_samples"],
      mapping_method=config_dict["mapping_method"],
      backtrack_limit=config_dict.get("backtrack_limit", 0),
      max_mapper_iter=config_dict.get("max_mapper_iter", 30),
      max_II=config_dict.get("max_II", 32),
      llvm_debug_type=config_dict["llvm_debug_type"],
    )


@dataclass
class RunnerContext:
  config: RunnerConfig
  kernel: str
  csource: str
  csourcefile: str
  appfolder: str
  config_path: str
  runmode: str
  morpher_home: str
  dfg_gen_home: str
  mapper_home: str
  simulator_home: str
  dfg_gen_kernel: str
  mapper_kernel: str
  simulator_kernel: str
  mem_trace: str

  @property
  def totalsize(self):
    return self.config.numberofbanks * self.config.banksize


def build_context(csource, function, config_path, runmode):
  config = RunnerConfig.from_yaml(config_path)
  appfolder, csourcefile = csource.rsplit("/", 1)

  morpher_home = os.getcwd()
  dfg_gen_home = morpher_home + "/Morpher_DFG_Generator"
  mapper_home = morpher_home + "/Morpher_CGRA_Mapper"
  simulator_home = morpher_home + "/hycube_simulator"

  dfg_gen_kernel = dfg_gen_home + "/benchmarks/" + appfolder + "/"
  mapper_kernel = mapper_home + "/benchmarks/" + config.mapper_subfolder + "/" + appfolder + "/"
  simulator_kernel = simulator_home + "/benchmarks/" + appfolder + "/"
  mem_trace = dfg_gen_kernel + "memtraces"

  os.makedirs(mapper_kernel, exist_ok=True)
  os.makedirs(simulator_kernel, exist_ok=True)
  os.makedirs(mem_trace, exist_ok=True)

  return RunnerContext(
    config=config,
    kernel=function,
    csource=csource,
    csourcefile=csourcefile,
    appfolder=appfolder,
    config_path=config_path,
    runmode=runmode,
    morpher_home=morpher_home,
    dfg_gen_home=dfg_gen_home,
    mapper_home=mapper_home,
    simulator_home=simulator_home,
    dfg_gen_kernel=dfg_gen_kernel,
    mapper_kernel=mapper_kernel,
    simulator_kernel=simulator_kernel,
    mem_trace=mem_trace,
  )

