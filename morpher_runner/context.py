import os
import re
import shutil
from dataclasses import dataclass
from datetime import datetime

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
  skip_ii_estimation: bool
  print_dfg: bool
  sa_seed: int
  sa_restarts: int
  sa_moves_per_temp: int
  sa_temp_max: float
  sa_temp_min: float
  sa_candidate_topk: int
  enable_simulation: bool
  validation_mode: str
  spm_mode: str
  remove_agi: bool
  remove_iter: bool
  auto_unroll_enabled: bool
  auto_unroll_factors: list
  auto_unroll_remainder_policy: str
  auto_unroll_stop_on_degradation: bool
  simd_enabled: bool
  simd_lanes: int
  simd_pack_load_store: bool
  simd_reduction_policy: str

  @staticmethod
  def _parse_simd(config_dict):
    simd_cfg = config_dict.get("simd", {})
    if simd_cfg is None:
      simd_cfg = {}
    if isinstance(simd_cfg, bool):
      simd_cfg = {"enabled": simd_cfg}
    if not isinstance(simd_cfg, dict):
      raise RuntimeError("Config key 'simd' must be a mapping or boolean.")

    enabled = bool(simd_cfg.get("enabled", False))
    try:
      lanes = int(simd_cfg.get("lanes", 1))
    except (TypeError, ValueError):
      raise RuntimeError("simd.lanes must be an integer.")
    if lanes < 1:
      raise RuntimeError("simd.lanes must be >= 1.")
    if enabled and lanes <= 1:
      raise RuntimeError("simd.lanes must be > 1 when simd.enabled=true.")

    pack_load_store = bool(simd_cfg.get("pack_load_store", True))
    reduction_policy = str(simd_cfg.get("reduction_policy", "tree")).strip().lower()
    if reduction_policy not in ("tree", "packed-psum"):
      raise RuntimeError("simd.reduction_policy must be one of: tree, packed-psum.")
    return enabled, lanes, pack_load_store, reduction_policy

  @staticmethod
  def _parse_auto_unroll(config_dict):
    auto_cfg = config_dict.get("auto_unroll", {})
    if auto_cfg is None:
      auto_cfg = {}
    if isinstance(auto_cfg, bool):
      auto_cfg = {"enabled": auto_cfg}
    if not isinstance(auto_cfg, dict):
      raise RuntimeError("Config key 'auto_unroll' must be a mapping or boolean.")

    enabled = bool(auto_cfg.get("enabled", False))
    raw_factors = auto_cfg.get("factors", [1, 2, 4, 8])
    if not isinstance(raw_factors, list) or not raw_factors:
      raise RuntimeError("auto_unroll.factors must be a non-empty list.")

    factors = []
    for factor in raw_factors:
      try:
        parsed = int(factor)
      except (TypeError, ValueError):
        raise RuntimeError("auto_unroll.factors entries must be integers.")
      if parsed < 1:
        raise RuntimeError("auto_unroll.factors entries must be >= 1.")
      if parsed not in factors:
        factors.append(parsed)

    remainder_policy = str(auto_cfg.get("remainder_policy", "preserve")).strip().lower()
    if remainder_policy not in ("preserve", "assume-multiple"):
      raise RuntimeError(
        "auto_unroll.remainder_policy must be one of: preserve, assume-multiple."
      )

    stop_on_degradation = bool(auto_cfg.get("stop_on_degradation", True))
    return enabled, factors, remainder_policy, stop_on_degradation

  @classmethod
  def from_yaml(cls, config_path):
    with open(config_path, "r", encoding="utf-8") as stream:
      config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
      raise RuntimeError("Config %s must contain a top-level mapping." % config_path)

    memory_mode = _resolve_memory_mode(config_dict)
    arch_template_json = _resolve_arch_template_json(config_dict, memory_mode)
    (
      simd_enabled,
      simd_lanes,
      simd_pack_load_store,
      simd_reduction_policy,
    ) = cls._parse_simd(config_dict)
    (
      auto_unroll_enabled,
      auto_unroll_factors,
      auto_unroll_remainder_policy,
      auto_unroll_stop_on_degradation,
    ) = cls._parse_auto_unroll(config_dict)
    _print_deprecation_warning(config_dict)
    validation_mode = str(config_dict.get("validation_mode", "functional")).strip().lower()
    if validation_mode == "event-driven":
      validation_mode = "event_driven"
    if validation_mode not in ("functional", "event_driven"):
      raise RuntimeError(
        "validation_mode must be one of: functional, event_driven."
      )

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
      ii_restarts=config_dict.get("ii_restarts", 0),
      skip_ii_estimation=config_dict.get("skip_ii_estimation", False),
      print_dfg=config_dict.get("print_dfg", True),
      sa_seed=config_dict.get("sa_seed", 0),
      sa_restarts=config_dict.get("sa_restarts", 2),
      sa_moves_per_temp=config_dict.get("sa_moves_per_temp", 20),
      sa_temp_max=config_dict.get("sa_temp_max", 200.0),
      sa_temp_min=config_dict.get("sa_temp_min", 10.0),
      sa_candidate_topk=config_dict.get("sa_candidate_topk", 8),
      enable_simulation=config_dict.get("enable_simulation", True),
      validation_mode=validation_mode,
      spm_mode=config_dict.get("spm_mode", "on"),
      remove_agi=config_dict.get("remove_agi", False),
      remove_iter=config_dict.get("remove_iter", False),
      auto_unroll_enabled=auto_unroll_enabled,
      auto_unroll_factors=auto_unroll_factors,
      auto_unroll_remainder_policy=auto_unroll_remainder_policy,
      auto_unroll_stop_on_degradation=auto_unroll_stop_on_degradation,
      simd_enabled=simd_enabled,
      simd_lanes=simd_lanes,
      simd_pack_load_store=simd_pack_load_store,
      simd_reduction_policy=simd_reduction_policy,
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


def build_context(csource, function, config_path, runmode, run_tag=""):
  config = RunnerConfig.from_yaml(config_path)
  appfolder, csourcefile = csource.rsplit("/", 1)

  morpher_home = os.getcwd()
  dfg_gen_home = morpher_home + "/Morpher_DFG_Generator"
  mapper_home = morpher_home + "/Morpher_CGRA_Mapper"
  simulator_home = morpher_home + "/simulator/hycube_simulator"

  run_tag = _sanitize_run_tag(run_tag)
  if not run_tag:
    run_tag = datetime.now().strftime("%Y%m%d_%H%M%S")

  run_root = os.path.join(morpher_home, "workspace", "morpher_runs", run_tag)
  dfg_gen_kernel = os.path.join(run_root, "dfg", appfolder) + "/"
  mapper_kernel = os.path.join(run_root, "mapper", config.mapper_subfolder, appfolder) + "/"
  simulator_kernel = os.path.join(run_root, "simulator", appfolder) + "/"
  mem_trace = dfg_gen_kernel + "memtraces"

  if runmode in ("runall", "dfg_gen_only", "runall_artifact_sim"):
    source_dir = dfg_gen_home + "/benchmarks/" + appfolder
    if os.path.isdir(source_dir):
      _prepare_tagged_dfg_kernel(source_dir, dfg_gen_kernel.rstrip("/"))
    else:
      os.makedirs(dfg_gen_kernel, exist_ok=True)

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


def _sanitize_run_tag(run_tag):
  if run_tag is None:
    return ""
  run_tag = run_tag.strip()
  if not run_tag:
    return ""
  return re.sub(r"[^A-Za-z0-9._-]+", "_", run_tag)


def _prepare_tagged_dfg_kernel(source_dir, tagged_dir):
  ignore = shutil.ignore_patterns(
    "memtraces",
    "*.ll",
    "*.bc",
    "*.dot",
    "*.pdf",
    "*.xml",
    "*.log",
    "*.o",
    "*.bin",
    "final",
    "final_*",
    "*_mem_alloc.txt",
    "*_opt_instrument.ll",
    "cfg.*",
  )
  if os.path.isdir(tagged_dir):
    shutil.rmtree(tagged_dir)
  shutil.copytree(source_dir, tagged_dir, ignore=ignore)
