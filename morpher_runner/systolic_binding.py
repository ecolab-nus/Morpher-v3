"""Source-to-template systolic binding analysis.

This module separates semantic binding from mapper placement:

* source shape is derived from candidate JSON, not from function names;
* every template has an explicit source contract;
* unused template cells are reported as spatial underutilization;
* transforms such as im2col and batched dot products are named in the result;
* missing source facts produce ``conditional`` rather than a false match.

The mapper still has the final word on placement/routing.  A static binding
result only says that the source roles can be represented by the template.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from functools import reduce
from math import ceil
from operator import mul
from pathlib import Path
from typing import Any
import json


SOURCE_BOUND_TEMPLATES = {
    "batched_dot_output_stationary",
    "gemm_output_stationary",
}

MAIN_TEMPLATES = (
    "batched_dot_output_stationary",
    "gemm_output_stationary",
    "gemm_two_stream_store",
    "matvec_row",
    "matvec_parallel",
    "gemm_fanout",
    "gemm_chain",
    "gemm_weight_stationary",
)


def _product(values: list[int]) -> int | None:
  if not values:
    return None
  return reduce(mul, values, 1)


def _positive_int(value: object) -> int | None:
  if isinstance(value, bool):
    return None
  if isinstance(value, int) and value > 0:
    return value
  return None


def _unique(values: list[str]) -> list[str]:
  return list(dict.fromkeys(value for value in values if value))


@dataclass(frozen=True)
class SourceModel:
  function: str
  source_shape: str
  verdict: str
  reject_reason: str
  parallel_ivs: tuple[str, ...]
  reduction_ivs: tuple[str, ...]
  parallel_extents: tuple[int | None, ...]
  reduction_extents: tuple[int | None, ...]
  output_count: int | None
  terms_per_output: int | None
  terms_evidence: str
  products_per_reduction_step: int
  mul_ops: int
  add_ops: int
  total_arith: int
  coefficients_known: bool
  input_bases: tuple[str, ...]
  stream_bases: tuple[str, ...]
  output_bases: tuple[str, ...]
  operand_roles_verified: bool
  distinct_input_accesses: int
  exact_access_schedules: bool
  kernel_shape_factor: float


@dataclass(frozen=True)
class ArchFacts:
  path: str
  memory_pes: int
  mac_pes: int
  scalar_eq_alus: int
  has_treg: bool
  simd_width: int


@dataclass(frozen=True)
class ContractAssessment:
  status: str
  template: str
  transform: str
  confidence: str
  reason: str


@dataclass(frozen=True)
class Binding:
  function: str
  record_index: int
  source_report: str
  source_shape: str
  template: str
  transform: str
  binding_status: str
  binding_reason: str
  terms_per_output: int | None
  terms_evidence: str
  active_cells: int
  template_cells: int
  leftover_cells: int
  template_spatial_utilization: float
  generated_dim_x: int
  generated_dim_y: int
  input_loads_per_cycle: int
  output_stores_per_cycle: int
  memory_ops_per_cycle: int
  memory_pes: int | None
  memory_ii_lower_bound: int | None
  compute_pes: int | None
  simd_width: int
  requires_treg: bool
  treg_available: bool | None
  boundary_phase_required: bool
  source_input_bases: tuple[str, ...]
  source_output_bases: tuple[str, ...]
  coefficient_base: str
  mapper_multihop: int | None = None
  map_status: str = "not_run"
  mapped_ii: int | None = None
  mapped_util_phase: float | None = None
  mapped_compute_util: float | None = None
  mapped_scalar_eq_mac_per_cycle: float | None = None
  main_feasibility: str = "conditional"
  end_to_end_feasibility: str = "conditional"
  artifact_xml: str = ""
  mapper_reason: str = ""

  def to_dict(self) -> dict[str, Any]:
    row = asdict(self)
    row["source_input_bases"] = ";".join(self.source_input_bases)
    row["source_output_bases"] = ";".join(self.source_output_bases)
    return row


def source_model(record: dict[str, Any]) -> SourceModel:
  loop_classes = record.get("loop_classes") or []
  trip_by_iv = {
      str(item.get("iv") or ""): _positive_int(item.get("trip_count"))
      for item in (record.get("trip_counts") or [])
  }
  parallel_ivs = tuple(
      str(item.get("iv") or "")
      for item in loop_classes if item.get("kind") == "PARALLEL"
  )
  reduction_ivs = tuple(
      str(item.get("iv") or "")
      for item in loop_classes if item.get("kind") == "REDUCTION"
  )
  parallel_extents = tuple(trip_by_iv.get(iv) for iv in parallel_ivs)
  reduction_extents = tuple(trip_by_iv.get(iv) for iv in reduction_ivs)

  arithmetic = record.get("arithmetic_summary") or {}
  mac_pairs = int(arithmetic.get("mac_pairs", record.get("mac_pairs", 0)) or 0)
  stencil_pairs = int(
      arithmetic.get("stencil_pairs", record.get("stencil_pairs", 0)) or 0
  )
  total_arith = int(
      arithmetic.get("total_arith", record.get("total_arith", 0)) or 0
  )
  mul_ops = int(arithmetic.get("mul_ops", 0) or 0)
  add_ops = int(arithmetic.get("add_ops", 0) or 0)
  weighted_terms = _positive_int(arithmetic.get("weighted_terms"))
  coefficients_known = bool(arithmetic.get("coefficients_known", False))
  weighted_sum_terms = arithmetic.get("weighted_sum_terms") or []
  if weighted_sum_terms:
    weighted_terms = len(weighted_sum_terms)

  if len(parallel_ivs) >= 2 and reduction_ivs and mac_pairs > 0:
    shape = "two_axis_reduction"
  elif len(parallel_ivs) == 1 and reduction_ivs and mac_pairs > 0:
    shape = "one_axis_reduction"
  elif len(parallel_ivs) >= 1 and not reduction_ivs and (
      mac_pairs > 0
      or stencil_pairs > 0
      or (coefficients_known and weighted_terms is not None)
  ):
    shape = "fixed_weighted_sum"
  else:
    shape = "unsupported"

  reduction_product = _product(
      [value for value in reduction_extents if value is not None]
  )
  products_per_step = max(1, mac_pairs) if reduction_ivs else 1
  if reduction_ivs and reduction_product is not None and all(
      value is not None for value in reduction_extents
  ):
    terms = reduction_product * products_per_step
    terms_evidence = (
        "exact_trip_count"
        if products_per_step == 1
        else "exact_trip_count_x_body_products"
    )
  elif weighted_terms is not None:
    terms = weighted_terms
    terms_evidence = "counted_arithmetic_terms"
  elif shape == "fixed_weighted_sum" and mac_pairs > 0:
    terms = mac_pairs + 1
    terms_evidence = "inferred_from_mac_chain"
  elif shape == "fixed_weighted_sum" and total_arith > 0:
    terms = total_arith + 1
    terms_evidence = "inferred_from_add_tree"
  else:
    terms = None
    terms_evidence = "unknown"

  accesses = record.get("affine_accesses") or []
  stores = [access for access in accesses if access.get("kind") == "store"]
  output_bases = _unique([
      str(access.get("base") or "") for access in stores
  ])
  loads = [access for access in accesses if access.get("kind") == "load"]
  input_bases = _unique([
      str(access.get("base") or "")
      for access in loads
      if str(access.get("base") or "") not in output_bases
  ])
  if not input_bases:
    input_bases = _unique([
        str(access.get("base") or "") for access in loads
    ])

  candidate_loads = [
      access for access in loads
      if str(access.get("base") or "") not in output_bases
  ]
  if not candidate_loads:
    candidate_loads = loads

  input_scevs = {
      str(access.get("pointer_scev"))
      for access in loads
      if access.get("pointer_scev")
  }
  exact_schedules = bool(accesses) and all(
      bool(access.get("pointer_scev")) for access in accesses
  )

  def deps(access: dict[str, Any]) -> set[str]:
    return {
        str(iv) for iv, coefficient in (
            access.get("loop_coefficients") or {}
        ).items() if coefficient
    }

  stream_bases: tuple[str, ...] = ()
  operand_roles_verified = False
  reduction_set = set(reduction_ivs)
  if shape == "two_axis_reduction" and len(parallel_ivs) >= 2:
    p0, p1 = parallel_ivs[:2]
    lhs_matches = [
        access for access in candidate_loads
        if reduction_set.issubset(deps(access))
        and p0 in deps(access) and p1 not in deps(access)
    ]
    rhs_matches = [
        access for access in candidate_loads
        if reduction_set.issubset(deps(access))
        and p1 in deps(access) and p0 not in deps(access)
    ]
    if (
        len(lhs_matches) >= products_per_step
        and len(rhs_matches) >= products_per_step
    ):
      stream_bases = tuple(
          str(access.get("base") or "")
          for pair in zip(
              lhs_matches[:products_per_step],
              rhs_matches[:products_per_step],
          )
          for access in pair
      )
      operand_roles_verified = True
  elif shape == "one_axis_reduction" and parallel_ivs:
    parallel = parallel_ivs[0]
    matrix_matches = [
        access for access in candidate_loads
        if reduction_set.issubset(deps(access))
        and parallel in deps(access)
    ]
    vector_matches = [
        access for access in candidate_loads
        if reduction_set.issubset(deps(access))
        and parallel not in deps(access)
    ]
    if len(matrix_matches) >= products_per_step and vector_matches:
      stream_bases = tuple(
          str(access.get("base") or "")
          for matrix in matrix_matches[:products_per_step]
          for access in (matrix, vector_matches[0])
      )
      operand_roles_verified = True
  elif shape == "fixed_weighted_sum" and candidate_loads:
    stream_bases = (str(candidate_loads[0].get("base") or ""),)
    operand_roles_verified = exact_schedules and coefficients_known

  output_count = None
  known_parallel = [value for value in parallel_extents if value is not None]
  if known_parallel and len(known_parallel) == len(parallel_extents):
    output_count = _product(known_parallel)

  # `systolic_reject_reason` came from an older family whitelist.  This
  # analyzer deliberately re-derives the source shape, so only hard candidate
  # rejection (aliasing, non-affine access, side effects, etc.) is inherited.
  reject = str(record.get("reject_reason") or "")
  return SourceModel(
      function=str(record.get("function") or "kernel"),
      source_shape=shape,
      verdict=str(record.get("verdict") or "unknown"),
      reject_reason=reject,
      parallel_ivs=parallel_ivs,
      reduction_ivs=reduction_ivs,
      parallel_extents=parallel_extents,
      reduction_extents=reduction_extents,
      output_count=output_count,
      terms_per_output=terms,
      terms_evidence=terms_evidence,
      products_per_reduction_step=products_per_step,
      mul_ops=mul_ops,
      add_ops=add_ops,
      total_arith=total_arith,
      coefficients_known=coefficients_known,
      input_bases=tuple(input_bases),
      stream_bases=stream_bases,
      output_bases=tuple(output_bases),
      operand_roles_verified=operand_roles_verified,
      distinct_input_accesses=len(input_scevs) if input_scevs else len(loads),
      exact_access_schedules=exact_schedules,
      kernel_shape_factor=float(record.get("kernel_shape_factor", 1.0) or 1.0),
  )


def assess_source_contract(record: dict[str, Any]) -> ContractAssessment:
  """Assess whether source facts justify replacing an old shape match.

  This is a static recurrence/access contract. It does not claim mapped or
  end-to-end functional correctness.
  """
  model = source_model(record)
  accesses = record.get("affine_accesses") or []

  def deps(access: dict[str, Any]) -> set[str]:
    return {
        str(iv) for iv, coefficient in (
            access.get("loop_coefficients") or {}
        ).items() if coefficient
    }

  loads = [access for access in accesses if access.get("kind") == "load"]
  stores = [access for access in accesses if access.get("kind") == "store"]
  output_bases = {
      str(access.get("base") or "") for access in stores if access.get("base")
  }
  input_loads = [
      access for access in loads
      if str(access.get("base") or "") not in output_bases
  ]
  extra_arithmetic = model.total_arith > 2 * model.products_per_reduction_step

  if model.source_shape == "two_axis_reduction" and len(model.parallel_ivs) >= 2:
    p0, p1 = model.parallel_ivs[:2]
    red = set(model.reduction_ivs)
    expected_output = {p0, p1}
    store_spaces = {frozenset(deps(access)) for access in stores}
    if not stores or frozenset(expected_output) not in store_spaces:
      return ContractAssessment(
          "no", "", "algorithmic_rewrite", "rejected",
          "no output store matches the two parallel output axes",
      )
    if any(set(space) != expected_output for space in store_spaces):
      return ContractAssessment(
          "no", "", "algorithmic_rewrite", "rejected",
          "the nest updates multiple output index spaces, not one GEMM recurrence",
      )
    lhs = [
        access for access in input_loads
        if red.issubset(deps(access)) and p0 in deps(access) and p1 not in deps(access)
    ]
    rhs = [
        access for access in input_loads
        if red.issubset(deps(access)) and p1 in deps(access) and p0 not in deps(access)
    ]
    products = model.products_per_reduction_step
    if len(lhs) >= products and len(rhs) >= products:
      transforms = []
      status = "direct"
      confidence = "exact_source_contract"
      if products > 1:
        status = "transform"
        transforms.append("phase_split_products")
        confidence = "source_contract_with_phase_split"
      if lhs and rhs and str(lhs[0].get("base")) == str(rhs[0].get("base")):
        transforms.append("same_base_dual_stream")
      if model.kernel_shape_factor < 1.0:
        transforms.append("triangular_tile_packing")
      if extra_arithmetic:
        return ContractAssessment(
            "conditional", "gemm_output_stationary",
            "+".join(transforms + ["input_prescale"]),
            "missing_scalar_operand_provenance",
            "affine GEMM streams are proven, but extra arithmetic must be explicitly folded into an input stream",
        )
      return ContractAssessment(
          status, "gemm_output_stationary",
          "+".join(transforms) or "direct_gemm",
          confidence,
          "complementary row/K and K/column streams feed one output recurrence",
      )
    aliased_rhs = [
        access for access in loads
        if str(access.get("base") or "") in output_bases
        and red.issubset(deps(access)) and p1 in deps(access)
    ]
    if lhs and aliased_rhs:
      return ContractAssessment(
          "conditional", "gemm_output_stationary",
          "output_alias_wavefront+triangular_tile_packing",
          "requires_dependence_order_proof",
          "one GEMM operand aliases the output and needs an explicit wavefront schedule",
      )
    return ContractAssessment(
        "conditional", "gemm_output_stationary", "recover_missing_stream_role",
        "missing_operand_provenance",
        "GEMM-shaped arithmetic is present but complementary source streams are not proven",
    )

  if model.source_shape == "one_axis_reduction" and model.parallel_ivs:
    parallel = model.parallel_ivs[0]
    red = set(model.reduction_ivs)
    matrix = [
        access for access in input_loads
        if red.issubset(deps(access)) and parallel in deps(access)
    ]
    vector = [
        access for access in input_loads
        if red.issubset(deps(access)) and parallel not in deps(access)
    ]
    products = model.products_per_reduction_step
    if len(matrix) >= products and vector:
      if products > 1:
        return ContractAssessment(
            "conditional", "batched_dot_output_stationary",
            "phase_split_products+epilogue_combine",
            "requires_multi_accumulator_handoff",
            "multiple dot products are proven but their accumulator/epilogue split is not represented",
        )
      if extra_arithmetic:
        return ContractAssessment(
            "conditional", "batched_dot_output_stationary", "input_prescale",
            "missing_scalar_operand_provenance",
            "matrix/vector roles are proven, but extra arithmetic must be folded into an input stream",
        )
      return ContractAssessment(
          "direct", "batched_dot_output_stationary", "batched_dot",
          "exact_source_contract",
          "matrix[output,K] and vector[K] streams feed independent output accumulators",
      )
    return ContractAssessment(
        "conditional", "batched_dot_output_stationary",
        "recover_missing_stream_role", "missing_operand_provenance",
        "one reduction operand is not recoverable as an affine source stream",
    )

  if model.source_shape == "fixed_weighted_sum" and len(model.parallel_ivs) >= 2:
    p0, p1 = model.parallel_ivs[:2]
    matrix = [access for access in loads if {p0, p1}.issubset(deps(access))]
    p0_only = [access for access in input_loads if deps(access) == {p0}]
    p1_only = [access for access in input_loads if deps(access) == {p1}]
    store_spaces = {frozenset(deps(access)) for access in stores}
    if matrix and frozenset({p0}) in store_spaces and frozenset({p1}) in store_spaces:
      return ContractAssessment(
          "transform", "batched_dot_output_stationary",
          "loop_distribution_to_two_batched_dots",
          "source_contract_with_loop_distribution",
          "shared matrix reads and distinct axis-indexed outputs prove two independent GEMV reductions",
      )
    if (
        len(output_bases) == 1 and matrix
        and len(p0_only) >= 2 and len(p1_only) >= 2
        and model.mul_ops >= 2
    ):
      return ContractAssessment(
          "transform", "gemm_output_stationary",
          "phase_split_rank1_updates",
          "source_contract_with_phase_split",
          "two row/column operand pairs update one matrix output",
      )
    if (
        model.terms_per_output is not None
        and model.coefficients_known
        and model.exact_access_schedules
        and model.distinct_input_accesses >= model.terms_per_output
        and model.output_bases
        and matrix
    ):
      return ContractAssessment(
          "transform", "batched_dot_output_stationary",
          "patch_extract_fixed_weighted_sum",
          "source_contract_with_affine_taps",
          "exact affine taps and coefficients prove a fixed weighted sum per output",
      )

  return ContractAssessment(
      "no", "", "algorithmic_rewrite", "rejected",
      "the recovered source recurrence does not fit a source-bound template contract",
  )


def read_arch_facts(path: Path) -> ArchFacts:
  data = json.loads(path.read_text(encoding="utf-8"))
  arch = data.get("ARCH") or {}
  cgra = arch.get("CGRA") or {}
  mods: list[dict[str, Any]] = []
  for entry in cgra.get("SUBMODS") or []:
    if isinstance(entry, dict):
      mods.extend(item for item in (entry.get("MODS") or []) if isinstance(item, dict))

  def module_has_op(module_name: str, wanted: set[str], seen: set[str] | None = None) -> bool:
    seen = set() if seen is None else seen
    if module_name in seen:
      return False
    seen.add(module_name)
    module = arch.get(module_name)
    if not isinstance(module, dict):
      return False
    ops = module.get("OPS")
    if isinstance(ops, dict) and wanted.intersection(str(op) for op in ops):
      return True
    submods = module.get("SUBMODS")
    if isinstance(submods, dict):
      return any(module_has_op(str(name), wanted, seen) for name in submods)
    return False

  def module_has_reg(module_name: str, reg: str) -> bool:
    module = arch.get(module_name)
    return isinstance(module, dict) and reg in (module.get("REGS") or [])

  memory_pes = 0
  mac_pes = 0
  scalar_eq_alus = 0
  has_treg = True
  simd_width = 1
  for item in mods:
    module_name = str(item.get("MOD") or "")
    if not module_name or "DUMMY" in module_name:
      continue
    module = arch.get(module_name) or {}
    width = int(module.get("SIMD_WIDTH", 1) or 1)
    simd_width = max(simd_width, width)
    is_memory_pe = module_has_op(module_name, {"LOAD", "VLOAD"})
    has_mac = module_has_op(module_name, {"MAC", "VMAC"})
    if is_memory_pe:
      memory_pes += 1
    # Edge memory cells in vector architectures also advertise VMAC, but the
    # source-bound templates place accumulator lanes only on the inner PE
    # grid. Keep compute-only utilization independent of memory-side ALUs.
    if has_mac and not is_memory_pe:
      mac_pes += 1
    if not is_memory_pe and module_has_op(
        module_name,
        {"ADD", "MUL", "MAC", "VADD", "VMUL", "VMAC", "LOAD", "VLOAD"},
    ):
      scalar_eq_alus += width
    if has_mac and not is_memory_pe:
      has_treg = has_treg and module_has_reg(module_name, "TREG")

  return ArchFacts(
      path=str(path),
      memory_pes=memory_pes,
      mac_pes=mac_pes,
      scalar_eq_alus=scalar_eq_alus,
      has_treg=has_treg and mac_pes > 0,
      simd_width=simd_width,
  )


def _scaled_active(raw_active: int, capacity: int, shape_factor: float) -> int:
  active = min(max(raw_active, 0), capacity)
  if shape_factor < 1.0:
    active = max(1, round(active * shape_factor))
  return active


def analyze_binding(
    model: SourceModel,
    record_index: int,
    template: str,
    dim_x: int,
    dim_y: int,
    arch: ArchFacts | None = None,
    source_report: str = "",
    active_cell_limit: int | None = None,
) -> Binding:
  capacity = dim_x * dim_y
  active = 0
  gen_x = dim_x
  gen_y = dim_y
  loads = 0
  stores = 0
  transform = "none"
  status = "incompatible"
  reason = ""
  requires_treg = False
  boundary = False
  coeff_base = ""

  if model.verdict == "no" or model.reject_reason:
    reason = model.reject_reason or "candidate pass rejected source nest"
  elif model.source_shape == "unsupported":
    reason = "source arithmetic/loop shape has no supported binding"
  elif template == "batched_dot_output_stationary":
    if model.source_shape not in {"one_axis_reduction", "fixed_weighted_sum"}:
      reason = "shared-coefficient batched dot does not represent two-axis GEMM outputs"
    else:
      transform = (
          "batched_dot"
          if model.source_shape == "one_axis_reduction"
          else "patch_extract_batched_dot"
      )
      if model.products_per_reduction_step > 1:
        transform = "concatenated_batched_dot_terms"
      output_count = model.output_count or capacity
      if active_cell_limit is not None:
        output_count = min(output_count, max(1, active_cell_limit))
      active = _scaled_active(output_count, capacity, model.kernel_shape_factor)
      loads = active + 1
      requires_treg = True
      boundary = True
      coeff_base = (
          model.input_bases[1]
          if len(model.input_bases) >= 2
          else "__systolic_coeff"
      )
      if model.terms_per_output is None:
        status = "conditional"
        reason = "reduction/weighted-sum term count is unknown"
      elif not model.operand_roles_verified:
        status = "conditional"
        reason = "shared coefficient and per-output source roles are not proven"
      elif model.products_per_reduction_step > 1:
        status = "conditional"
        reason = "multiple products per reduction step need an alternating stream/phase lowering"
      elif (
          model.source_shape == "one_axis_reduction"
          and model.total_arith > 2 * model.products_per_reduction_step
      ):
        status = "conditional"
        reason = "extra arithmetic must be explicitly folded into an input stream"
      elif model.source_shape == "fixed_weighted_sum" and (
          not model.exact_access_schedules
          or model.distinct_input_accesses < model.terms_per_output
          or not model.coefficients_known
      ):
        status = "conditional"
        reason = "tap count is known but exact affine taps or coefficients are missing"
      else:
        status = "supported"
        reason = "independent outputs bind to active MAC cells with one shared coefficient stream"
  elif template == "gemm_output_stationary":
    requires_treg = True
    boundary = True
    stores = 0
    if model.source_shape == "two_axis_reduction":
      transform = (
          "direct_gemm"
          if model.products_per_reduction_step == 1
          else "concatenated_gemm_terms"
      )
      p0 = model.parallel_extents[0] or dim_y
      p1 = model.parallel_extents[1] or dim_x
      rows = min(dim_y, p0)
      cols = min(dim_x, p1)
      active = _scaled_active(rows * cols, capacity, model.kernel_shape_factor)
      loads = rows + cols
      if not model.operand_roles_verified:
        status = "conditional"
        reason = "complementary row/K and column/K source access roles are not proven"
      elif model.products_per_reduction_step > 1:
        status = "conditional"
        reason = "concatenated GEMM terms are legal but alternating operand streams are not emitted yet"
      elif model.total_arith > 2 * model.products_per_reduction_step:
        status = "conditional"
        reason = "extra arithmetic must be explicitly folded into an input stream"
      else:
        status = "supported"
        reason = "two output axes and one reduction axis bind directly to GEMM rows/columns/K"
    elif model.source_shape in {"one_axis_reduction", "fixed_weighted_sum"}:
      transform = "algorithmic_rewrite"
      reason = (
          "GEMM output-stationary requires two proven output axes; "
          "use batched_dot_output_stationary for one-axis or fixed weighted sums"
      )
  elif template == "matvec_row":
    transform = "single_output_reduction_chain"
    terms = model.terms_per_output or dim_x
    active = min(dim_x, terms)
    gen_x, gen_y = active, 1
    loads = active
    stores = 1
    status = "conditional"
    reason = (
        "template carries one value per cell; weighted products must be "
        "precomputed or the template must gain a second operand stream"
    )
  elif template in {"gemm_fanout", "gemm_chain", "matvec_parallel"}:
    reason = "legacy topology template does not expose enough source operand roles"
  elif template == "gemm_weight_stationary":
    reason = "current template models the stationary weight implicitly and is not source-bindable"
  else:
    reason = "unknown template contract"

  active = min(active, capacity)
  leftover = max(0, capacity - active)
  mem_ops = loads + stores
  memory_pes = arch.memory_pes if arch else None
  memory_lb = None
  if memory_pes:
    memory_lb = max(1, ceil(mem_ops / memory_pes))
  treg_available = arch.has_treg if arch else None
  if requires_treg and treg_available is False:
    status = "incompatible"
    reason = "template requires one persistent accumulator register per active cell"
  if arch and active > arch.mac_pes:
    status = "incompatible"
    reason = f"binding needs {active} MAC cells but architecture exposes {arch.mac_pes}"

  main_feasibility = "no" if status == "incompatible" else "conditional"
  return Binding(
      function=model.function,
      record_index=record_index,
      source_report=source_report,
      source_shape=model.source_shape,
      template=template,
      transform=transform,
      binding_status=status,
      binding_reason=reason,
      terms_per_output=model.terms_per_output,
      terms_evidence=model.terms_evidence,
      active_cells=active,
      template_cells=capacity,
      leftover_cells=leftover,
      template_spatial_utilization=(active / capacity if capacity else 0.0),
      generated_dim_x=gen_x,
      generated_dim_y=gen_y,
      input_loads_per_cycle=loads,
      output_stores_per_cycle=stores,
      memory_ops_per_cycle=mem_ops,
      memory_pes=memory_pes,
      memory_ii_lower_bound=memory_lb,
      compute_pes=arch.mac_pes if arch else None,
      simd_width=arch.simd_width if arch else 1,
      requires_treg=requires_treg,
      treg_available=treg_available,
      boundary_phase_required=boundary,
      source_input_bases=model.stream_bases or model.input_bases,
      source_output_bases=model.output_bases,
      coefficient_base=coeff_base,
      main_feasibility=main_feasibility,
      end_to_end_feasibility=main_feasibility,
  )


def with_mapper_result(
    binding: Binding,
    map_status: str,
    mapped_ii: int | None,
    mapped_util_phase: float | None,
    artifact_xml: str,
    mapper_reason: str = "",
    mapper_multihop: int | None = None,
) -> Binding:
  row = asdict(binding)
  row.update({
      "map_status": map_status,
      "mapped_ii": mapped_ii,
      "mapped_util_phase": mapped_util_phase,
      "mapped_compute_util": (
          binding.active_cells / (binding.compute_pes * mapped_ii)
          if mapped_ii and binding.compute_pes else None
      ),
      "mapped_scalar_eq_mac_per_cycle": (
          binding.active_cells * binding.simd_width / mapped_ii
          if mapped_ii else None
      ),
      "mapper_multihop": mapper_multihop,
      "artifact_xml": artifact_xml,
      "mapper_reason": mapper_reason,
  })
  if binding.binding_status == "incompatible" or map_status in {"failed", "crashed"}:
    row["main_feasibility"] = "no"
    row["end_to_end_feasibility"] = "no"
  elif map_status != "success":
    row["main_feasibility"] = "conditional"
    row["end_to_end_feasibility"] = "conditional"
  elif binding.binding_status == "supported":
    row["main_feasibility"] = "yes"
    # Mapper success proves placement/routing of the generated main DFG, not
    # functional equivalence to the source. Boundary phases, exact address
    # schedules, and final values require separate verification.
    row["end_to_end_feasibility"] = "conditional"
  else:
    row["main_feasibility"] = "conditional"
    row["end_to_end_feasibility"] = "conditional"
  return Binding(**row)
