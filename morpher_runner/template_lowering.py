"""Correctness-first source-template lowering.

This module is the gate between ``SystolicCandidatePass`` JSON and template
DFG XML. It accepts only records whose source facts prove a template contract;
conditional or rejected records are returned as data so callers can fall back
to normal DFG generation without treating topology-only mappings as source
equivalent.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import json
import xml.etree.ElementTree as ET

from morpher_runner.systolic_binding import (
    SOURCE_BOUND_TEMPLATES,
    ArchFacts,
    Binding,
    analyze_binding,
    assess_source_contract,
    source_model,
)
from morpher_runner.systolic_templates import TEMPLATES
from morpher_runner.systolic_templates import main_phase_cell_node_ids


@dataclass(frozen=True)
class TemplateLoweringResult:
  status: str
  reason: str
  metadata: dict[str, Any]
  xml: str = ""
  mem_alloc: str = ""
  binding: Binding | None = None

  @property
  def accepted(self) -> bool:
    return self.status == "accepted"


def _unique(values: list[str]) -> list[str]:
  return list(dict.fromkeys(value for value in values if value))


def mem_alloc_text(bases: list[str], stride: int = 1024) -> str:
  return "var,base_addr\n" + "".join(
      f"{base},{index * stride}\n"
      for index, base in enumerate(_unique(bases))
  )


def _bind_bases(xml: str, load_bases: list[str],
                store_bases: list[str]) -> str:
  root = ET.fromstring("<root>" + xml + "</root>")
  load_i = 0
  store_i = 0
  for node in root.findall(".//Node"):
    op = (node.findtext("OP") or "").strip()
    if op.startswith("V"):
      op = op[1:]
    if op not in {"LOAD", "LOADH", "LOADB", "STORE", "STOREH", "STOREB"}:
      continue
    existing = (node.findtext("BasePointerName") or "").strip()
    if existing == "var_a" and load_bases:
      base = load_bases[0]
    elif existing == "var_b" and len(load_bases) >= 2:
      base = load_bases[1]
    elif existing in {"var_c", "store"} and store_bases:
      base = store_bases[0]
    else:
      bases = load_bases if op.startswith("LOAD") else store_bases
      if not bases:
        continue
      idx = load_i if op.startswith("LOAD") else store_i
      base = bases[idx % len(bases)]
    if op.startswith("LOAD"):
      load_i += 1
    else:
      store_i += 1
    for old in list(node.findall("BasePointerName")):
      node.remove(old)
    elem = ET.SubElement(node, "BasePointerName", {"size": "256"})
    elem.text = base
  return "\n".join(
      ET.tostring(child, encoding="unicode") for child in list(root)
  ) + "\n"


def _template_bases(binding: Binding) -> tuple[list[str], list[str]]:
  inputs = list(binding.source_input_bases)
  if binding.coefficient_base:
    if len(inputs) < 2:
      inputs.append(binding.coefficient_base)
    else:
      inputs[1] = binding.coefficient_base
  while len(inputs) < 2:
    inputs.append(f"__systolic_input{len(inputs)}")
  outputs = list(binding.source_output_bases) or ["__systolic_output"]
  return inputs, outputs


def _metadata(record: dict[str, Any], binding: Binding | None,
              status: str, reason: str) -> dict[str, Any]:
  assessment = assess_source_contract(record)
  base: dict[str, Any] = {
      "status": status,
      "reason": reason,
      "source_equivalent": status == "accepted",
      "source_equivalence_scope": (
          "steady_state_main_dfg"
          if status == "accepted" else "normal_dfg_fallback_required"
      ),
      "contract_status": assessment.status,
      "contract_confidence": assessment.confidence,
      "contract_transform": assessment.transform,
      "contract_reason": assessment.reason,
      "lowering_family": record.get("lowering_family", ""),
      "experimental": False,
      "required_future_proof": "",
      "fallback": "normal_dfg_generator",
      "limitations": [
          "v1 binds base roles and template shape; exact byte schedules are not emitted",
          "output-stationary drains are not end-to-end source-equivalent until same-PE TREG handoff is enforced",
      ],
  }
  if binding is None:
    return base
  base.update({
      "function": binding.function,
      "template": binding.template,
      "transform": binding.transform,
      "terms_per_output": binding.terms_per_output,
      "terms_evidence": binding.terms_evidence,
      "active_cells": binding.active_cells,
      "template_cells": binding.template_cells,
      "leftover_cells": binding.leftover_cells,
      "template_spatial_utilization": binding.template_spatial_utilization,
      "generated_dim_x": binding.generated_dim_x,
      "generated_dim_y": binding.generated_dim_y,
      "input_loads_per_cycle": binding.input_loads_per_cycle,
      "output_stores_per_cycle": binding.output_stores_per_cycle,
      "memory_ops_per_cycle": binding.memory_ops_per_cycle,
      "boundary_phase_required": binding.boundary_phase_required,
      "simd_width": binding.simd_width,
      "prologue_required": False,
      "epilogue_required": binding.boundary_phase_required,
      "prologue_epilogue_functionally_chained": False,
      "source_input_bases": list(binding.source_input_bases),
      "source_output_bases": list(binding.source_output_bases),
      "main_cell_node_ids": list(main_phase_cell_node_ids(
          binding.template,
          binding.generated_dim_x,
          binding.generated_dim_y,
          binding.active_cells,
      )),
      "epilogue_cell_node_ids": list(range(binding.active_cells)),
      "epilogue_template": (
          "gemm_output_stationary_treg_drain_epilogue"
          if binding.boundary_phase_required else ""
      ),
  })
  return base


def lower_template_record(
    record: dict[str, Any],
    record_index: int = 0,
    *,
    dim_x: int = 4,
    dim_y: int = 4,
    arch: ArchFacts | None = None,
    source_report: str = "",
    active_cell_limit: int | None = None,
) -> TemplateLoweringResult:
  """Try to lower one candidate record to a source-bound template."""
  assessment = assess_source_contract(record)
  if assessment.status == "no" or not assessment.template:
    reason = assessment.reason
    return TemplateLoweringResult(
        "rejected", reason, _metadata(record, None, "rejected", reason)
    )
  if assessment.status == "conditional":
    meta = _metadata(record, None, "conditional", assessment.reason)
    meta["required_future_proof"] = assessment.confidence
    return TemplateLoweringResult("conditional", assessment.reason, meta)
  if assessment.template not in SOURCE_BOUND_TEMPLATES:
    reason = f"template {assessment.template} is not source-bound in v1"
    return TemplateLoweringResult(
        "rejected", reason, _metadata(record, None, "rejected", reason)
    )

  model = source_model(record)
  binding = analyze_binding(
      model, record_index, assessment.template, dim_x, dim_y, arch,
      source_report=source_report, active_cell_limit=active_cell_limit,
  )
  if binding.binding_status != "supported":
    status = "conditional" if binding.binding_status == "conditional" else "rejected"
    meta = _metadata(record, binding, status, binding.binding_reason)
    if status == "conditional":
      meta["required_future_proof"] = binding.binding_reason
    return TemplateLoweringResult(status, binding.binding_reason, meta, binding=binding)

  terms = binding.terms_per_output or 1
  xml = TEMPLATES[binding.template](
      binding.generated_dim_x,
      binding.generated_dim_y,
      terms,
      op_kind="MAC",
      useful_cells=binding.active_cells,
      simd_lanes=binding.simd_width,
  )
  inputs, outputs = _template_bases(binding)
  xml = _bind_bases(xml, inputs, outputs)
  mem_alloc = mem_alloc_text(inputs + outputs)
  reason = binding.binding_reason
  return TemplateLoweringResult(
      "accepted", reason, _metadata(record, binding, "accepted", reason),
      xml=xml, mem_alloc=mem_alloc, binding=binding,
  )


def write_lowering_artifacts(
    result: TemplateLoweringResult,
    output_dir: Path,
    kernel: str,
) -> tuple[Path, Path, Path]:
  if not result.accepted:
    raise ValueError("cannot write template artifacts for non-accepted lowering")
  output_dir.mkdir(parents=True, exist_ok=True)
  xml_path = output_dir / f"{kernel}_systolic.xml"
  mem_path = output_dir / f"{kernel}_mem_alloc.txt"
  meta_path = output_dir / f"{kernel}_metadata.json"
  xml_path.write_text(result.xml, encoding="utf-8")
  mem_path.write_text(result.mem_alloc, encoding="utf-8")
  meta_path.write_text(
      json.dumps(result.metadata, indent=2, sort_keys=True) + "\n",
      encoding="utf-8",
  )
  return xml_path, mem_path, meta_path
