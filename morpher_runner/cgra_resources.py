"""Parse architecture JSON to determine per-operation resource counts."""
import json
import math
from typing import Dict


def get_op_resource_counts(arch_json_path: str) -> Dict[str, int]:
  """Return {op_name: num_datapaths} for the given architecture JSON.

  Counts PE_MEM instances (can do LOAD/STORE + compute) and PE instances
  (compute only). Memory-only ops (LOAD/STORE/etc.) are limited to PE_MEM
  count; compute ops are available on all PEs (PE + PE_MEM).
  """
  with open(arch_json_path, "r", encoding="utf-8") as f:
    arch = json.load(f)

  arch_root = arch["ARCH"]

  # Count PE_MEM and PE instances from CGRA.SUBMODS[0].MODS
  cgra = arch_root["CGRA"]
  submods = cgra["SUBMODS"]

  pe_mem_count = 0
  pe_count = 0
  for submod_entry in submods:
    if not isinstance(submod_entry, dict):
      continue
    mods_list = submod_entry.get("MODS")
    if mods_list is None:
      continue
    for mod in mods_list:
      mod_type = mod.get("MOD", "")
      if mod_type == "PE_MEM":
        pe_mem_count += 1
      elif mod_type == "PE":
        pe_count += 1

  total_pe = pe_mem_count + pe_count

  # Get ops supported by FU (compute-only PEs)
  fu_ops = set(arch_root.get("FU", {}).get("OPS", {}).keys())

  # Get ops supported by FU_MEM (memory PEs)
  fu_mem_ops = set(arch_root.get("FU_MEM", {}).get("OPS", {}).keys())

  # Memory-exclusive ops: in FU_MEM but not in FU
  mem_only_ops = fu_mem_ops - fu_ops
  # Compute ops present in FU (also available in FU_MEM)
  compute_ops = fu_ops

  result: Dict[str, int] = {}

  # Compute ops: available on all PEs
  for op in compute_ops:
    result[op] = total_pe

  # Memory-only ops: available only on PE_MEM
  for op in mem_only_ops:
    result[op] = pe_mem_count

  # Any ops in FU_MEM that are also in FU keep the full count (already set above)

  return result


def compute_res_mii(op_hist: Dict[str, int], op_resources: Dict[str, int]) -> int:
  """Compute resource-constrained MII = max(ceil(op_count / op_resources)) across all ops."""
  res_mii = 1
  for op, count in op_hist.items():
    if op == "NOP":
      continue
    resources = op_resources.get(op, 1)
    if resources <= 0:
      resources = 1
    mii = math.ceil(count / resources)
    if mii > res_mii:
      res_mii = mii
  return res_mii


def compute_rec_mii(dfg) -> int:
  """Compute recurrence-constrained MII from a DFGGraph.

  For each back-edge (nextiter != 0), recDistance = abs(src.asap - dst.asap) + 1.
  recMII = max(recDistance) across all back-edges, or 0 if none.
  """
  node_map = {n.idx: n for n in dfg.nodes}
  rec_mii = 0
  for node in dfg.nodes:
    for edge in node.outputs:
      if edge.nextiter != 0:
        src = node_map.get(edge.src_idx)
        dst = node_map.get(edge.dst_idx)
        if src is not None and dst is not None:
          dist = abs(src.asap - dst.asap) + 1
          if dist > rec_mii:
            rec_mii = dist
  return rec_mii
