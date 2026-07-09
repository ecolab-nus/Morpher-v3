"""Post-mapping utilization estimates from staged DFG and architecture facts."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import json
import xml.etree.ElementTree as ET


@dataclass(frozen=True)
class UtilizationEstimate:
  compute_nodes: int
  memory_nodes: int
  compute_pes: int
  memory_pes: int
  total_pes: int
  achieved_ii: int | None
  compute_util_percent: float | None
  memory_util_percent: float | None
  total_util_percent: float | None

  def csv_fields(self) -> dict[str, object]:
    return {
        "compute_nodes": self.compute_nodes,
        "memory_nodes": self.memory_nodes,
        "compute_pes": self.compute_pes,
        "memory_pes": self.memory_pes,
        "total_pes": self.total_pes,
        "compute_util_percent": _fmt_percent(self.compute_util_percent),
        "memory_util_percent": _fmt_percent(self.memory_util_percent),
        "total_util_percent": _fmt_percent(self.total_util_percent),
    }


def estimate_utilization(
    dfg_xml: Path,
    arch_json: Path,
    achieved_ii: int | None,
) -> UtilizationEstimate:
  compute_nodes, memory_nodes = count_dfg_nodes(dfg_xml)
  compute_pes, memory_pes = count_arch_pes(arch_json)
  total_pes = compute_pes + memory_pes
  return UtilizationEstimate(
      compute_nodes=compute_nodes,
      memory_nodes=memory_nodes,
      compute_pes=compute_pes,
      memory_pes=memory_pes,
      total_pes=total_pes,
      achieved_ii=achieved_ii,
      compute_util_percent=_util_percent(compute_nodes, compute_pes, achieved_ii),
      memory_util_percent=_util_percent(memory_nodes, memory_pes, achieved_ii),
      total_util_percent=_util_percent(
          compute_nodes + memory_nodes, total_pes, achieved_ii
      ),
  )


def count_dfg_nodes(dfg_xml: Path) -> tuple[int, int]:
  if not dfg_xml.is_file():
    return 0, 0
  text = dfg_xml.read_text(encoding="utf-8", errors="ignore")
  root = ET.fromstring("<Root>\n%s\n</Root>" % text)
  compute_nodes = 0
  memory_nodes = 0
  for op_elem in root.findall(".//OP"):
    op = _normalize_op(op_elem.text or "")
    if not op or op == "NOP":
      continue
    if is_memory_op(op):
      memory_nodes += 1
    elif is_compute_op(op):
      compute_nodes += 1
  return compute_nodes, memory_nodes


def count_arch_pes(arch_json: Path) -> tuple[int, int]:
  if not arch_json.is_file():
    return 0, 0
  data = json.loads(arch_json.read_text(encoding="utf-8"))
  arch = data.get("ARCH") or {}
  cgra = arch.get("CGRA") or {}
  compute_pes = 0
  memory_pes = 0
  for item in _cgra_mods(cgra):
    module_name = str(item.get("MOD") or "")
    if not module_name or "DUMMY" in module_name:
      continue
    if _module_has_op(arch, module_name, MEMORY_OPS):
      memory_pes += 1
    elif _module_has_op(arch, module_name, COMPUTE_OPS):
      compute_pes += 1
  return compute_pes, memory_pes


MEMORY_OPS = {
    "LOAD",
    "LOADB",
    "LOADH",
    "STORE",
    "STOREB",
    "STOREH",
    "VLOAD",
    "VSTORE",
}

COMPUTE_OPS = {
    "ADD",
    "SUB",
    "MUL",
    "MAC",
    "DIV",
    "REM",
    "AND",
    "OR",
    "XOR",
    "SHL",
    "SHR",
    "CMP",
    "SELECT",
    "PHI",
    "MOV",
    "VADD",
    "VSUB",
    "VMUL",
    "VMAC",
    "VCMP",
    "VSELECT",
}


def is_memory_op(op: str) -> bool:
  op = _normalize_op(op)
  if op.startswith("OLOAD") or op.startswith("OSTORE"):
    return False
  return op in MEMORY_OPS or "LOAD" in op or "STORE" in op


def is_compute_op(op: str) -> bool:
  op = _normalize_op(op)
  if not op or op == "NOP":
    return False
  return not is_memory_op(op)


def _normalize_op(op: str) -> str:
  return op.strip().upper()


def _cgra_mods(cgra: dict[str, Any]) -> list[dict[str, Any]]:
  mods: list[dict[str, Any]] = []
  for entry in cgra.get("SUBMODS") or []:
    if isinstance(entry, dict):
      mods.extend(item for item in (entry.get("MODS") or []) if isinstance(item, dict))
  return mods


def _module_has_op(
    arch: dict[str, Any],
    module_name: str,
    wanted: set[str],
    seen: set[str] | None = None,
) -> bool:
  seen = set() if seen is None else seen
  if module_name in seen:
    return False
  seen.add(module_name)
  module = arch.get(module_name)
  if not isinstance(module, dict):
    return False
  ops = module.get("OPS")
  if isinstance(ops, dict) and wanted.intersection(str(op).upper() for op in ops):
    return True
  submods = module.get("SUBMODS")
  if isinstance(submods, dict):
    return any(_module_has_op(arch, str(name), wanted, seen) for name in submods)
  return False


def _util_percent(nodes: int, pes: int, ii: int | None) -> float | None:
  if nodes < 0 or pes <= 0 or ii is None or ii <= 0:
    return None
  return 100.0 * float(nodes) / float(pes * ii)


def _fmt_percent(value: float | None) -> str:
  return "" if value is None else f"{value:.3f}"
