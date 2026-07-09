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
  if "simd" in config_dict:
    deprecated_keys.append("simd")
  if deprecated_keys:
    print(
      "Warning: deprecated config keys in use (%s). "
      "Please migrate to memory_mode + arch_template_json, and move SIMD capability into the architecture JSON."
      % ", ".join(deprecated_keys)
    )


MAPPER_PRESETS = {
  "fast": {
    "mapping_method": 2,
    "backtrack_limit": 2,
    "max_mapper_iter": 12,
    "max_II": 16,
    "ii_restarts": 1,
    "skip_ii_estimation": False,
    "print_dfg": False,
    "sa_seed": 1,
    "sa_restarts": 1,
    "sa_moves_per_temp": 12,
    "sa_temp_max": 120.0,
    "sa_temp_min": 20.0,
    "sa_candidate_topk": 4,
    "sa_stall_temp_steps": 4,
    "sa_stall_min_unmapped": 1,
    "off_spm_mem_candidate_topk": "auto_fast",
    "pf_candidate_topk": "auto_fast",
    "pf_frontier_stall_repeats": 4,
    "pf_frontier_stall_min_mapped": 3,
    "rut_recovery_seed": 1,
    "proximity_sort": True,
    "membound_mapping": False,
    "recmin_policy": "asap",
  },
  "balanced": {
    "mapping_method": 2,
    "backtrack_limit": 4,
    "max_mapper_iter": 30,
    "max_II": 32,
    "ii_restarts": 2,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 1,
    "sa_restarts": 2,
    "sa_moves_per_temp": 20,
    "sa_temp_max": 200.0,
    "sa_temp_min": 10.0,
    "sa_candidate_topk": 8,
    "sa_stall_temp_steps": 6,
    "sa_stall_min_unmapped": 1,
    "off_spm_mem_candidate_topk": "auto_balanced",
    "pf_candidate_topk": "auto_balanced",
    "pf_frontier_stall_repeats": 4,
    "pf_frontier_stall_min_mapped": 3,
    "rut_recovery_seed": 1,
    "proximity_sort": True,
    "membound_mapping": False,
    "recmin_policy": "asap",
  },
  "quality": {
    "mapping_method": 2,
    "backtrack_limit": 8,
    "max_mapper_iter": 60,
    "max_II": 64,
    "ii_restarts": 1,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 1,
    "sa_restarts": 4,
    "sa_moves_per_temp": 40,
    "sa_temp_max": 250.0,
    "sa_temp_min": 5.0,
    "sa_candidate_topk": 12,
    "sa_stall_temp_steps": 0,
    "sa_stall_min_unmapped": 1,
    "off_spm_mem_candidate_topk": 0,
    "pf_candidate_topk": 0,
    "pf_frontier_stall_repeats": 0,
    "pf_frontier_stall_min_mapped": 0,
    "rut_recovery_seed": 1,
    "proximity_sort": True,
    "membound_mapping": False,
    "recmin_policy": "asap",
  },
  "exhaust": {
    "mapping_method": 2,
    "backtrack_limit": 12,
    "max_mapper_iter": 100,
    "max_II": 96,
    "ii_restarts": 2,
    "skip_ii_estimation": False,
    "print_dfg": True,
    "sa_seed": 1,
    "sa_restarts": 8,
    "sa_moves_per_temp": 80,
    "sa_temp_max": 300.0,
    "sa_temp_min": 3.0,
    "sa_candidate_topk": 16,
    "sa_stall_temp_steps": 0,
    "sa_stall_min_unmapped": 1,
    "off_spm_mem_candidate_topk": 0,
    "pf_candidate_topk": 0,
    "pf_frontier_stall_repeats": 0,
    "pf_frontier_stall_min_mapped": 0,
    "rut_recovery_seed": 1,
    "proximity_sort": False,
    "membound_mapping": False,
    "recmin_policy": "asap",
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
    "sa_stall_temp_steps",
    "sa_stall_min_unmapped",
    "off_spm_mem_candidate_topk",
    "pf_candidate_topk",
    "pf_frontier_stall_repeats",
    "pf_frontier_stall_min_mapped",
    "rut_recovery_seed",
    "proximity_sort",
    "membound_mapping",
    "recmin_policy",
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


def _auto_select_recmin_policy(
    config_dict,
    mapper_tuning,
    auto_unroll_enabled,
    scalar_reduction_policy,
):
  if not auto_unroll_enabled or scalar_reduction_policy == "none":
    return
  if "recmin_policy" in config_dict:
    return

  mapper_tuning["recmin_policy"] = "path"
  print(
    "Auto-selected recmin_policy=path because auto_unroll.scalar_reduction_policy=%s. "
    "Set recmin_policy explicitly to override."
    % scalar_reduction_policy
  )


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


def _module_supports_memory_ops(arch_root, module_name, seen=None):
  if seen is None:
    seen = set()
  if module_name in seen:
    return False
  seen.add(module_name)

  module = arch_root.get(module_name)
  if not isinstance(module, dict):
    return False

  ops = module.get("OPS")
  if isinstance(ops, dict) and any(str(op).startswith("LOAD") for op in ops.keys()):
    return True

  submods = module.get("SUBMODS")
  if isinstance(submods, dict):
    for submod_name in submods.keys():
      if _module_supports_memory_ops(arch_root, submod_name, seen):
        return True
  if isinstance(submods, list):
    for entry in submods:
      if not isinstance(entry, dict):
        continue
      mod_name = entry.get("MOD")
      if isinstance(mod_name, str) and _module_supports_memory_ops(arch_root, mod_name, seen):
        return True
      for key in entry.keys():
        if key not in ("MOD", "name", "X", "Y", "DIMS", "PATTERN", "CONNECTIONS", "MODS"):
          if _module_supports_memory_ops(arch_root, key, seen):
            return True
      mods = entry.get("MODS")
      if isinstance(mods, list):
        for mod_entry in mods:
          if isinstance(mod_entry, dict):
            mod_name = mod_entry.get("MOD")
            if isinstance(mod_name, str) and _module_supports_memory_ops(arch_root, mod_name, seen):
              return True
  return False


def infer_arch_mem_pe_count(morpher_home, mapper_home, arch_template_json):
  arch_path = resolve_arch_template_path(morpher_home, mapper_home, arch_template_json)
  with open(arch_path, "r", encoding="utf-8") as stream:
    arch_data = json.load(stream)

  arch_root = arch_data.get("ARCH")
  if not isinstance(arch_root, dict):
    return None
  cgra_root = arch_root.get("CGRA")
  if not isinstance(cgra_root, dict):
    return None
  cgra_submods = cgra_root.get("SUBMODS")
  if not isinstance(cgra_submods, list):
    return None

  count = 0
  for entry in cgra_submods:
    if not isinstance(entry, dict):
      continue
    mods = entry.get("MODS")
    if isinstance(mods, list):
      for mod_entry in mods:
        if not isinstance(mod_entry, dict):
          continue
        mod_name = mod_entry.get("MOD")
        if isinstance(mod_name, str) and _module_supports_memory_ops(arch_root, mod_name):
          count += 1
      continue

    mod_name = entry.get("MOD")
    if isinstance(mod_name, str) and _module_supports_memory_ops(arch_root, mod_name):
      count += 1
      continue

    for key in entry.keys():
      if key not in ("name", "X", "Y", "DIMS", "PATTERN", "CONNECTIONS"):
        if _module_supports_memory_ops(arch_root, key):
          count += 1

  return count if count > 0 else None


def _resolve_off_spm_mem_candidate_topk(value, _mem_pe_count):
  if isinstance(value, str):
    policy = value.strip().lower().replace("-", "_")
    if policy in ("", "0", "none", "off", "unbounded"):
      return 0
    if policy == "auto_fast":
      return "auto_fast"
    if policy in ("auto_balanced", "auto_mempes", "auto_mem_pes"):
      return "auto_balanced"
    raise RuntimeError(
      "off_spm_mem_candidate_topk must be an integer or one of: auto_fast, auto_balanced, 0."
    )
  try:
    topk = int(value)
  except (TypeError, ValueError):
    raise RuntimeError("off_spm_mem_candidate_topk must be an integer or auto policy string.")
  if topk < 0:
    raise RuntimeError("off_spm_mem_candidate_topk must be >= 0.")
  return topk


def _resolve_nonnegative_int(value, key):
  try:
    parsed = int(value)
  except (TypeError, ValueError):
    raise RuntimeError("%s must be an integer." % key)
  if parsed < 0:
    raise RuntimeError("%s must be >= 0." % key)
  return parsed


def _resolve_positive_int(value, key):
  try:
    parsed = int(value)
  except (TypeError, ValueError):
    raise RuntimeError("%s must be an integer." % key)
  if parsed < 1:
    raise RuntimeError("%s must be >= 1." % key)
  return parsed


def _resolve_pf_candidate_topk(value):
  if isinstance(value, str):
    policy = value.strip().lower().replace("-", "_")
    if policy in ("", "0", "none", "off", "unbounded"):
      return 0
    if policy in ("auto_fast", "auto_balanced"):
      return policy
    raise RuntimeError("pf_candidate_topk must be an integer or one of: auto_fast, auto_balanced, 0.")
  try:
    parsed = int(value)
  except (TypeError, ValueError):
    raise RuntimeError("pf_candidate_topk must be an integer, auto_fast, or auto_balanced.")
  if parsed < 0:
    raise RuntimeError("pf_candidate_topk must be >= 0, auto_fast, or auto_balanced.")
  return parsed


def _resolve_membound_mapping(value):
  if isinstance(value, bool):
    return "auto" if value else "off"
  if value is None:
    return "off"
  mode = str(value).strip().lower().replace("-", "_")
  if mode in ("", "0", "false", "off", "no", "disabled"):
    return "off"
  if mode in ("1", "true", "auto", "on", "yes", "enabled"):
    return "auto"
  if mode in ("force", "forced", "always", "mem_first"):
    return "force"
  raise RuntimeError("membound_mapping must be boolean or one of: off, auto, force.")


def _resolve_recmin_policy(value):
  policy = str(value or "asap").strip().lower()
  if policy not in ("asap", "path"):
    raise RuntimeError("recmin_policy must be one of: asap, path.")
  return policy


VECTOR_OP_PREFIX = "V"
VECTOR_MEMORY_OP_PREFIXES = ("VLOAD", "VSTORE", "VOLOAD", "VOSTORE")


@dataclass(frozen=True)
class ArchSimdConfig:
  enabled: bool
  width: int
  scalar_broadcast: bool
  vector_ops: frozenset


def infer_arch_simd_config(morpher_home, mapper_home, arch_template_json):
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

  grid_entry = None
  for entry in cgra_submods:
    if isinstance(entry, dict) and str(entry.get("PATTERN", "")).strip().upper() == "GRID":
      grid_entry = entry
      break
  if grid_entry is None:
    raise RuntimeError("Architecture JSON %s has no CGRA GRID submodule pattern." % arch_path)

  mods = grid_entry.get("MODS", [])
  if not isinstance(mods, list):
    raise RuntimeError("Architecture JSON %s has invalid GRID MODS list." % arch_path)

  def _module_desc(mod_type):
    desc = arch_root.get(mod_type)
    if not isinstance(desc, dict):
      raise RuntimeError("Architecture JSON %s is missing ARCH.%s." % (arch_path, mod_type))
    return desc

  def _fu_type_for_pe(mod_type):
    pe_desc = _module_desc(mod_type)
    submods = pe_desc.get("SUBMODS", {})
    if not isinstance(submods, dict):
      return None
    for candidate in submods.keys():
      if str(candidate).startswith("FU"):
        return str(candidate)
    return None

  widths = set()
  broadcasts = set()
  vector_ops = set()
  simd_compute_tiles = 0

  for mod in mods:
    if not isinstance(mod, dict):
      continue
    mod_type = str(mod.get("MOD", "PE")).strip()
    if not mod_type.startswith("PE"):
      continue

    pe_desc = _module_desc(mod_type)
    width = pe_desc.get("SIMD_WIDTH")
    if width is None:
      width = 1
    try:
      width = int(width)
    except (TypeError, ValueError):
      raise RuntimeError("ARCH.%s.SIMD_WIDTH must be an integer." % mod_type)
    if width < 1:
      raise RuntimeError("ARCH.%s.SIMD_WIDTH must be >= 1." % mod_type)
    if width == 1:
      continue

    fu_type = _fu_type_for_pe(mod_type)
    if not fu_type:
      raise RuntimeError("ARCH.%s must contain a FU submodule when SIMD_WIDTH > 1." % mod_type)
    fu_desc = _module_desc(fu_type)
    ops = fu_desc.get("OPS", {})
    if not isinstance(ops, dict):
      raise RuntimeError("ARCH.%s.OPS must be a mapping." % fu_type)
    tile_vector_ops = {
      str(op_name).strip()
      for op_name in ops.keys()
      if str(op_name).strip().startswith(VECTOR_OP_PREFIX)
    }
    if not tile_vector_ops:
      raise RuntimeError(
        "ARCH.%s declares SIMD_WIDTH=%d but ARCH.%s.OPS exposes no vector op names."
        % (mod_type, width, fu_type)
      )

    vector_ops.update(tile_vector_ops)
    compute_vector_ops = {
      op_name for op_name in tile_vector_ops
      if not op_name.startswith(VECTOR_MEMORY_OP_PREFIXES)
    }
    if not compute_vector_ops:
      continue

    widths.add(width)
    broadcasts.add(bool(pe_desc.get("SCALAR_BROADCAST", fu_desc.get("SCALAR_BROADCAST", False))))
    simd_compute_tiles += 1

  if not simd_compute_tiles:
    return ArchSimdConfig(
      enabled=False,
      width=1,
      scalar_broadcast=False,
      vector_ops=frozenset(),
    )

  if len(widths) != 1:
    raise RuntimeError(
      "Architecture JSON %s uses mixed SIMD widths across compute PEs (%s). "
      "This flow currently requires one uniform SIMD width."
      % (arch_path, ", ".join(str(width) for width in sorted(widths)))
    )

  return ArchSimdConfig(
    enabled=True,
    width=next(iter(widths)),
    scalar_broadcast=(len(broadcasts) == 1 and True in broadcasts),
    vector_ops=frozenset(sorted(vector_ops)),
  )


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
  sa_stall_temp_steps: int
  sa_stall_min_unmapped: int
  off_spm_mem_candidate_topk: int
  pf_candidate_topk: int
  pf_frontier_stall_repeats: int
  pf_frontier_stall_min_mapped: int
  rut_recovery_seed: int
  membound_mapping: str
  recmin_policy: str
  arch_mem_pe_count: int
  enable_simulation: bool
  generate_dfg_pdf: bool
  suppress_dfg_dot: bool
  validation_mode: str
  spm_mode: str
  proximity_sort: bool
  node_pin: str
  remove_agi: bool
  remove_iter: bool
  layout_solver: bool
  replicate_data: bool
  auto_unroll_enabled: bool
  auto_unroll_factors: list
  auto_unroll_fuse_innermost_loops: bool
  auto_unroll_stop_on_degradation: bool
  auto_unroll_scalar_reduction_policy: str
  auto_unroll_scalar_reduction_terms: int
  arch_simd_enabled: bool
  arch_simd_width: int
  arch_scalar_broadcast: bool
  arch_vector_ops: frozenset
  source_template_mode: str = "off"

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

    if "remainder_policy" in auto_cfg:
      raise RuntimeError(
        "auto_unroll.remainder_policy has been removed. Morpher always preserves "
        "remainders; size benchmarks so mapped loop trip counts are legal multiples "
        "of the desired unroll/SIMD factor."
      )

    if "outer_factor" in auto_cfg:
      raise RuntimeError(
        "auto_unroll.outer_factor has been removed. Morpher now attempts legal "
        "innermost loop fusion with auto_unroll.fuse_innermost_loops instead."
      )
    fuse_innermost_loops = bool(auto_cfg.get("fuse_innermost_loops", True))

    stop_on_degradation = bool(auto_cfg.get("stop_on_degradation", True))
    scalar_reduction_policy = str(
      auto_cfg.get("scalar_reduction_policy", "none")
    ).strip().lower()
    if scalar_reduction_policy not in ("none", "tree", "scalar-psum"):
      raise RuntimeError(
        "auto_unroll.scalar_reduction_policy must be one of: none, tree, scalar-psum."
      )
    try:
      scalar_reduction_terms = int(auto_cfg.get("scalar_reduction_terms", 2))
    except (TypeError, ValueError):
      raise RuntimeError("auto_unroll.scalar_reduction_terms must be an integer.")
    if scalar_reduction_terms < 2:
      raise RuntimeError("auto_unroll.scalar_reduction_terms must be >= 2.")
    if scalar_reduction_policy != "none":
      invalid_factors = [
        factor for factor in factors
        if factor > 1 and factor % scalar_reduction_terms != 0
      ]
      if invalid_factors:
        raise RuntimeError(
          "auto_unroll.scalar_reduction_terms=%d must divide every scalar unroll "
          "factor when auto_unroll.scalar_reduction_policy=%s. Invalid factor(s): %s."
          % (
            scalar_reduction_terms,
            scalar_reduction_policy,
            ", ".join(str(factor) for factor in invalid_factors),
          )
        )

    return (
      enabled,
      factors,
      fuse_innermost_loops,
      stop_on_degradation,
      scalar_reduction_policy,
      scalar_reduction_terms,
    )

  @staticmethod
  def _parse_source_template(config_dict):
    source_cfg = config_dict.get("source_template", {})
    if source_cfg is None:
      source_cfg = {}
    if isinstance(source_cfg, bool):
      return "auto" if source_cfg else "off"
    if not isinstance(source_cfg, dict):
      raise RuntimeError("Config key 'source_template' must be a mapping or boolean.")

    enabled = source_cfg.get("enabled", "off")
    if isinstance(enabled, bool):
      return "auto" if enabled else "off"
    mode = str(enabled).strip().lower()
    if mode in ("true", "on", "yes"):
      return "auto"
    if mode in ("false", "off", "no", ""):
      return "off"
    if mode != "auto":
      raise RuntimeError("source_template.enabled must be one of: off, auto.")
    return mode

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
      auto_unroll_enabled,
      auto_unroll_factors,
      auto_unroll_fuse_innermost_loops,
      auto_unroll_stop_on_degradation,
      auto_unroll_scalar_reduction_policy,
      auto_unroll_scalar_reduction_terms,
    ) = cls._parse_auto_unroll(config_dict)
    _auto_select_recmin_policy(
      config_dict,
      mapper_tuning,
      auto_unroll_enabled,
      auto_unroll_scalar_reduction_policy,
    )
    source_template_mode = cls._parse_source_template(config_dict)
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
    inferred_mem_pes = None
    arch_simd = ArchSimdConfig(False, 1, False, frozenset())
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
    inferred_mem_pes = infer_arch_mem_pe_count(morpher_home, mapper_home, arch_template_json)
    arch_simd = infer_arch_simd_config(morpher_home, mapper_home, arch_template_json)

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
      sa_stall_temp_steps=int(mapper_tuning["sa_stall_temp_steps"]),
      sa_stall_min_unmapped=int(mapper_tuning["sa_stall_min_unmapped"]),
      off_spm_mem_candidate_topk=_resolve_off_spm_mem_candidate_topk(
        mapper_tuning.get("off_spm_mem_candidate_topk", 0),
        inferred_mem_pes,
      ),
      pf_candidate_topk=_resolve_pf_candidate_topk(
        mapper_tuning.get("pf_candidate_topk", 0),
      ),
      pf_frontier_stall_repeats=int(mapper_tuning["pf_frontier_stall_repeats"]),
      pf_frontier_stall_min_mapped=int(mapper_tuning["pf_frontier_stall_min_mapped"]),
      rut_recovery_seed=int(mapper_tuning["rut_recovery_seed"]),
      membound_mapping=_resolve_membound_mapping(
        mapper_tuning.get("membound_mapping", False),
      ),
      recmin_policy=_resolve_recmin_policy(
        mapper_tuning.get("recmin_policy", "asap"),
      ),
      arch_mem_pe_count=int(inferred_mem_pes or 0),
      enable_simulation=config_dict.get("enable_simulation", True),
      generate_dfg_pdf=bool(config_dict.get("generate_dfg_pdf", True)),
      suppress_dfg_dot=bool(config_dict.get("suppress_dfg_dot", False)),
      validation_mode=validation_mode,
      spm_mode=config_dict.get("spm_mode", "on"),
      remove_agi=config_dict.get("remove_agi", False),
      node_pin=str(config_dict.get("node_pin", "") or ""),
      remove_iter=config_dict.get("remove_iter", False),
      layout_solver=bool(config_dict.get("layout_solver", False)),
      replicate_data=bool(config_dict.get("replicate_data", False)),
      auto_unroll_enabled=auto_unroll_enabled,
      auto_unroll_factors=auto_unroll_factors,
      auto_unroll_fuse_innermost_loops=auto_unroll_fuse_innermost_loops,
      auto_unroll_stop_on_degradation=auto_unroll_stop_on_degradation,
      auto_unroll_scalar_reduction_policy=auto_unroll_scalar_reduction_policy,
      auto_unroll_scalar_reduction_terms=auto_unroll_scalar_reduction_terms,
      source_template_mode=source_template_mode,
      arch_simd_enabled=arch_simd.enabled,
      arch_simd_width=arch_simd.width,
      arch_scalar_broadcast=arch_simd.scalar_broadcast,
      arch_vector_ops=arch_simd.vector_ops,
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
  dfg_gen_kernel: str
  mapper_kernel: str
  simulator_kernel: str
  mem_trace: str
  selected_simd_enabled: bool = False
  selected_simd_width: int = 1
  selected_simd_artifact_only: bool = False

  @property
  def totalsize(self):
    return self.config.numberofbanks * self.config.banksize


def build_context(csource, function, config_path, runmode, run_tag=""):
  config = RunnerConfig.from_yaml(config_path)
  appfolder, csourcefile = csource.rsplit("/", 1)

  morpher_home = os.getcwd()
  dfg_gen_home = morpher_home + "/Morpher_DFG_Generator"
  mapper_home = morpher_home + "/Morpher_CGRA_Mapper"

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
