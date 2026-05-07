import os
import re
import shutil
import json
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


MAPPER_PRESETS = {
  "fast": {
    "mapping_method": 2,
    "backtrack_limit": 2,
    "max_mapper_iter": 12,
    "max_II": 16,
    "ii_restarts": 0,
    "skip_ii_estimation": False,
    "print_dfg": False,
    "sa_seed": 0,
    "sa_restarts": 0,
    "sa_moves_per_temp": 12,
    "sa_temp_max": 120.0,
    "sa_temp_min": 20.0,
    "sa_candidate_topk": 4,
    "proximity_sort": True,
  },
  "balanced": {
    "mapping_method": 2,
    "backtrack_limit": 4,
    "max_mapper_iter": 30,
    "max_II": 32,
    "ii_restarts": 0,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 0,
    "sa_restarts": 2,
    "sa_moves_per_temp": 20,
    "sa_temp_max": 200.0,
    "sa_temp_min": 10.0,
    "sa_candidate_topk": 8,
    "proximity_sort": True,
  },
  "quality": {
    "mapping_method": 2,
    "backtrack_limit": 8,
    "max_mapper_iter": 60,
    "max_II": 64,
    "ii_restarts": 1,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 0,
    "sa_restarts": 4,
    "sa_moves_per_temp": 40,
    "sa_temp_max": 250.0,
    "sa_temp_min": 5.0,
    "sa_candidate_topk": 12,
    "proximity_sort": True,
  },
  "exhaust": {
    "mapping_method": 2,
    "backtrack_limit": 12,
    "max_mapper_iter": 100,
    "max_II": 96,
    "ii_restarts": 2,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 0,
    "sa_restarts": 8,
    "sa_moves_per_temp": 80,
    "sa_temp_max": 300.0,
    "sa_temp_min": 3.0,
    "sa_candidate_topk": 16,
    "proximity_sort": False,
  },
}


def _parse_mapper_preset(config_dict):
  preset_name = str(config_dict.get("mapper_preset", "balanced")).strip().lower().replace("-", "_")
  if preset_name not in MAPPER_PRESETS:
    raise RuntimeError(
      "mapper_preset must be one of: %s."
      % ", ".join(sorted(MAPPER_PRESETS.keys()))
    )

  resolved = dict(MAPPER_PRESETS[preset_name])
  override_keys = [
    "mapping_method",
    "backtrack_limit",
    "max_mapper_iter",
    "max_II",
    "ii_restarts",
    "skip_ii_estimation",
    "print_dfg",
    "sa_seed",
    "sa_restarts",
    "sa_moves_per_temp",
    "sa_temp_max",
    "sa_temp_min",
    "sa_candidate_topk",
    "proximity_sort",
  ]
  used_overrides = []
  for key in override_keys:
    if key in config_dict:
      resolved[key] = config_dict[key]
      used_overrides.append(key)

  if used_overrides:
    print(
      "Warning: mapper_preset=%s overridden by explicit mapper keys (%s). "
      "Prefer using the preset alone unless you are intentionally doing advanced tuning."
      % (preset_name, ", ".join(used_overrides))
    )

  return preset_name, resolved


def resolve_arch_template_path(morpher_home, mapper_home, arch_template_json):
  template = arch_template_json
  if os.path.isabs(template):
    if os.path.isfile(template):
      return template
    raise RuntimeError("Architecture template not found: %s" % template)

  candidates = [
    os.path.join(mapper_home, "json_arch", template),
    os.path.join(morpher_home, template),
  ]
  for candidate in candidates:
    if os.path.isfile(candidate):
      return candidate

  raise RuntimeError(
    "Architecture template '%s' not found. Checked: %s"
    % (template, ", ".join(candidates))
  )


def _find_arch_dims(node):
  if isinstance(node, dict):
    dims = node.get("DIMS")
    if isinstance(dims, dict) and "X" in dims and "Y" in dims:
      return dims["X"], dims["Y"]
    for value in node.values():
      found = _find_arch_dims(value)
      if found is not None:
        return found
    return None

  if isinstance(node, list):
    for value in node:
      found = _find_arch_dims(value)
      if found is not None:
        return found
  return None


def infer_arch_dims(morpher_home, mapper_home, arch_template_json):
  arch_path = resolve_arch_template_path(morpher_home, mapper_home, arch_template_json)
  with open(arch_path, "r", encoding="utf-8") as stream:
    arch_data = json.load(stream)

  arch_root = arch_data.get("ARCH")
  if not isinstance(arch_root, dict):
    raise RuntimeError("Architecture JSON %s is missing top-level ARCH object." % arch_path)

  cgra_root = arch_root.get("CGRA")
  if cgra_root is None:
    raise RuntimeError("Architecture JSON %s is missing ARCH.CGRA." % arch_path)

  dims = _find_arch_dims(cgra_root)
  if dims is None:
    raise RuntimeError("Could not infer CGRA dimensions from architecture JSON: %s" % arch_path)

  try:
    xdim = int(dims[0])
    ydim = int(dims[1])
  except (TypeError, ValueError):
    raise RuntimeError("Architecture JSON %s has non-integer CGRA dimensions." % arch_path)
  if xdim < 1 or ydim < 1:
    raise RuntimeError("Architecture JSON %s has invalid CGRA dimensions %sx%s." % (arch_path, xdim, ydim))
  return xdim, ydim


def infer_arch_spm_config(morpher_home, mapper_home, arch_template_json):
  arch_path = resolve_arch_template_path(morpher_home, mapper_home, arch_template_json)
  with open(arch_path, "r", encoding="utf-8") as stream:
    arch_data = json.load(stream)

  arch_root = arch_data.get("ARCH")
  if not isinstance(arch_root, dict):
    raise RuntimeError("Architecture JSON %s is missing top-level ARCH object." % arch_path)

  cgra_root = arch_root.get("CGRA")
  if not isinstance(cgra_root, dict):
    raise RuntimeError("Architecture JSON %s is missing ARCH.CGRA." % arch_path)

  cgra_submods = cgra_root.get("SUBMODS")
  if not isinstance(cgra_submods, list):
    raise RuntimeError("Architecture JSON %s is missing ARCH.CGRA.SUBMODS list." % arch_path)

  wrapper_names = set()
  for entry in cgra_submods:
    if not isinstance(entry, dict):
      continue
    for key in entry.keys():
      if re.match(r"^SPM_B[0-9]+_WRAPPER$", key):
        wrapper_names.add(key)

  spm = arch_root.get("SPM")
  if not isinstance(spm, dict):
    return None, None
  spm_mem = spm.get("MEM")
  if not isinstance(spm_mem, dict) or "SIZE" not in spm_mem:
    return None, None

  try:
    bank_size = int(spm_mem["SIZE"])
  except (TypeError, ValueError):
    raise RuntimeError("Architecture JSON %s has non-integer ARCH.SPM.MEM.SIZE." % arch_path)
  if bank_size < 1:
    raise RuntimeError("Architecture JSON %s has invalid SPM bank size %d." % (arch_path, bank_size))

  if not wrapper_names:
    return None, bank_size

  return len(wrapper_names), bank_size


@dataclass
class RunnerConfig:
  memory_mode: str
  arch_template_json: str
  mapper_subfolder: str
  mapper_preset: str
  init_II: int
  multihop: int
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
  proximity_sort: bool
  node_pin: str
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
    mapper_preset, mapper_tuning = _parse_mapper_preset(config_dict)
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
    try:
      multihop = int(config_dict.get("multihop", 4))
    except (TypeError, ValueError):
      raise RuntimeError("multihop must be an integer.")
    if multihop < 1:
      raise RuntimeError("multihop must be >= 1.")
    morpher_home = os.getcwd()
    mapper_home = os.path.join(morpher_home, "Morpher_CGRA_Mapper")
    inferred_xdim = None
    inferred_ydim = None
    inferred_numberofbanks = None
    inferred_banksize = None
    try:
      inferred_xdim, inferred_ydim = infer_arch_dims(morpher_home, mapper_home, arch_template_json)
    except RuntimeError:
      if "xdim" not in config_dict or "ydim" not in config_dict:
        raise
    try:
      inferred_numberofbanks, inferred_banksize = infer_arch_spm_config(
        morpher_home,
        mapper_home,
        arch_template_json,
      )
    except RuntimeError:
      if "numberofbanks" not in config_dict or "banksize" not in config_dict:
        raise

    xdim = config_dict.get("xdim", inferred_xdim)
    ydim = config_dict.get("ydim", inferred_ydim)
    if xdim is None or ydim is None:
      raise RuntimeError("Config must provide xdim/ydim or use an architecture JSON with inferable dimensions.")
    try:
      xdim = int(xdim)
      ydim = int(ydim)
    except (TypeError, ValueError):
      raise RuntimeError("xdim and ydim must be integers.")
    if xdim < 1 or ydim < 1:
      raise RuntimeError("xdim and ydim must be >= 1.")
    if inferred_xdim is not None and inferred_ydim is not None and (xdim != inferred_xdim or ydim != inferred_ydim):
      raise RuntimeError(
        "Config xdim/ydim (%d, %d) do not match architecture JSON dimensions (%d, %d)."
        % (xdim, ydim, inferred_xdim, inferred_ydim)
      )
    numberofbanks = config_dict.get("numberofbanks", inferred_numberofbanks)
    banksize = config_dict.get("banksize", inferred_banksize)
    if numberofbanks is None or banksize is None:
      raise RuntimeError(
        "Config must provide numberofbanks/banksize or use an architecture JSON with explicit SPM bank metadata."
      )
    try:
      numberofbanks = int(numberofbanks)
      banksize = int(banksize)
    except (TypeError, ValueError):
      raise RuntimeError("numberofbanks and banksize must be integers.")
    if numberofbanks < 1 or banksize < 1:
      raise RuntimeError("numberofbanks and banksize must be >= 1.")
    if inferred_numberofbanks is not None and numberofbanks != inferred_numberofbanks:
      raise RuntimeError(
        "Config numberofbanks (%d) does not match architecture JSON bank count (%d)."
        % (numberofbanks, inferred_numberofbanks)
      )
    if inferred_banksize is not None and banksize != inferred_banksize:
      raise RuntimeError(
        "Config banksize (%d) does not match architecture JSON SPM bank size (%d)."
        % (banksize, inferred_banksize)
      )

    return cls(
      memory_mode=memory_mode,
      arch_template_json=arch_template_json,
      mapper_subfolder=config_dict["mapper_subfolder"],
      mapper_preset=mapper_preset,
      init_II=config_dict["init_II"],
      multihop=multihop,
      ydim=ydim,
      xdim=xdim,
      numberofbanks=numberofbanks,
      banksize=banksize,
      max_test_samples=config_dict["max_test_samples"],
      mapping_method=mapper_tuning["mapping_method"],
      backtrack_limit=mapper_tuning["backtrack_limit"],
      max_mapper_iter=mapper_tuning["max_mapper_iter"],
      max_II=mapper_tuning["max_II"],
      ii_restarts=mapper_tuning["ii_restarts"],
      skip_ii_estimation=mapper_tuning["skip_ii_estimation"],
      print_dfg=mapper_tuning["print_dfg"],
      sa_seed=mapper_tuning["sa_seed"],
      sa_restarts=mapper_tuning["sa_restarts"],
      sa_moves_per_temp=mapper_tuning["sa_moves_per_temp"],
      sa_temp_max=mapper_tuning["sa_temp_max"],
      sa_temp_min=mapper_tuning["sa_temp_min"],
      sa_candidate_topk=mapper_tuning["sa_candidate_topk"],
      enable_simulation=config_dict.get("enable_simulation", True),
      validation_mode=validation_mode,
      spm_mode=config_dict.get("spm_mode", "on"),
      remove_agi=config_dict.get("remove_agi", False),
      node_pin=str(config_dict.get("node_pin", "") or ""),
      remove_iter=config_dict.get("remove_iter", False),
      auto_unroll_enabled=auto_unroll_enabled,
      auto_unroll_factors=auto_unroll_factors,
      auto_unroll_remainder_policy=auto_unroll_remainder_policy,
      auto_unroll_stop_on_degradation=auto_unroll_stop_on_degradation,
      simd_enabled=simd_enabled,
      simd_lanes=simd_lanes,
      simd_pack_load_store=simd_pack_load_store,
      simd_reduction_policy=simd_reduction_policy,
      proximity_sort=mapper_tuning["proximity_sort"],
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
