import os
from dataclasses import dataclass

import yaml


def _resolve_memory_mode(config_dict):
  mode = config_dict.get("memory_mode")
  if mode is None:
    legacy_json_arch = config_dict.get("json_arch")
    legacy_before_memupdate = config_dict.get("json_arch_before_memupdate")
    if legacy_before_memupdate or (isinstance(legacy_json_arch, str) and "_mem" in legacy_json_arch):
      return "auto"
    return "static"

  mode = str(mode).strip().lower()
  if mode not in ("auto", "static"):
    raise RuntimeError("Invalid memory_mode=%r. Expected one of: auto, static." % mode)
  return mode


def _resolve_arch_template_json(config_dict, memory_mode):
  if "arch_template_json" in config_dict:
    return config_dict["arch_template_json"]

  legacy_before_memupdate = config_dict.get("json_arch_before_memupdate")
  legacy_json_arch = config_dict.get("json_arch")

  if memory_mode == "auto" and legacy_before_memupdate:
    return legacy_before_memupdate
  if legacy_json_arch:
    return legacy_json_arch

  raise RuntimeError(
    "Missing required config key 'arch_template_json'. "
    "Legacy fallback keys json_arch/json_arch_before_memupdate were also not found."
  )


def _print_deprecation_warning(config_dict):
  deprecated_keys = []
  if "json_arch" in config_dict:
    deprecated_keys.append("json_arch")
  if "json_arch_before_memupdate" in config_dict:
    deprecated_keys.append("json_arch_before_memupdate")
  if deprecated_keys:
    print(
      "Warning: deprecated config keys in use (%s). "
      "Please migrate to memory_mode + arch_template_json."
      % ", ".join(deprecated_keys)
    )


@dataclass
class RunnerConfig:
  memory_mode: str
  arch_template_json: str
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
  ii_restarts: int
  spm_mode: str
  llvm_debug_type: str

  @classmethod
  def from_yaml(cls, config_path):
    with open(config_path, "r", encoding="utf-8") as stream:
      config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
      raise RuntimeError("Config %s must contain a top-level mapping." % config_path)

    memory_mode = _resolve_memory_mode(config_dict)
    arch_template_json = _resolve_arch_template_json(config_dict, memory_mode)
    _print_deprecation_warning(config_dict)

    return cls(
      memory_mode=memory_mode,
      arch_template_json=arch_template_json,
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
      ii_restarts=config_dict.get("ii_restarts", 1),
      spm_mode=config_dict.get("spm_mode", "on"),
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
