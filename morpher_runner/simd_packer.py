from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import copy
import re
import xml.etree.ElementTree as ET


PACKABLE_COMPUTE_OPS = {
  "ADD",
  "SUB",
  "MUL",
  "SEXT",
  "DIV",
  "LS",
  "RS",
  "ARS",
  "AND",
  "OR",
  "XOR",
  "CMP",
  "CLT",
  "CGT",
  "SELECT",
  "CMERGE",
}

PACKABLE_MEMORY_OPS = {
  "LOAD",
  "LOADH",
  "LOADB",
  "STORE",
  "STOREH",
  "STOREB",
  "OLOAD",
  "OLOADH",
  "OLOADB",
  "OSTORE",
  "OSTOREH",
  "OSTOREB",
}


@dataclass(frozen=True)
class SimdPackReport:
  input_nodes: int
  output_nodes: int
  packed_groups: int
  packed_nodes_removed: int
  artifact_nodes_pruned: int = 0
  artifact_vars_pruned: tuple[str, ...] = ()

  @property
  def pack_ratio(self) -> float:
    if self.input_nodes == 0:
      return 1.0
    return float(self.output_nodes) / float(self.input_nodes)


@dataclass(frozen=True)
class SimdMapping:
  """Index remapping produced by SIMD packing, needed to fix addr traces."""
  reindex: dict  # pre_idx -> post_idx for all surviving nodes
  simd_groups: dict  # post_idx -> [pre_idx_lane0, ..., pre_idx_laneL-1]
  simd_splat_groups: frozenset = frozenset()  # post_idx values whose lane0 addr should be replicated.


def _read_wrapped_xml(path: Path) -> ET.Element:
  text = path.read_text(encoding="utf-8")
  text = re.sub(r'(")([A-Za-z_][A-Za-z0-9_:-]*=)', r'\1 \2', text)
  return ET.fromstring("<Root>\n%s\n</Root>" % text)


def _write_unwrapped_xml(root: ET.Element, path: Path) -> None:
  parts = []
  for child in list(root):
    parts.append(ET.tostring(child, encoding="unicode"))
  path.write_text("\n".join(parts), encoding="utf-8")


def _child_text(node: ET.Element, name: str) -> str | None:
  child = node.find(name)
  if child is None or child.text is None:
    return None
  return child.text.strip()


def _base_pointer(node: ET.Element) -> tuple[str | None, str | None]:
  child = node.find("BasePointerName")
  if child is None:
    return None, None
  return (child.text.strip() if child.text else "", child.attrib.get("size"))


def _base_pointer_name(node: ET.Element) -> str | None:
  return _base_pointer(node)[0]


def _gep_offset(node: ET.Element) -> str | None:
  return _child_text(node, "GEPOffset")


def _node_idx(node: ET.Element) -> int:
  return int(node.attrib["idx"])


def _ref_idx(ref: ET.Element) -> int:
  return int(ref.attrib["idx"])


def _ref_signature(ref: ET.Element, replacement: dict[int, int]) -> tuple:
  mapped = replacement.get(_ref_idx(ref), _ref_idx(ref))
  attrs = tuple(sorted((k, v) for k, v in ref.attrib.items() if k != "idx"))
  return (mapped, attrs)


def _refs_signature(
  node: ET.Element,
  container_name: str,
  ref_name: str,
  replacement: dict[int, int],
  self_group: set[int],
  skip_external: bool = False,
) -> tuple:
  container = node.find(container_name)
  if container is None:
    return ()
  refs = []
  for ref in list(container):
    if ref.tag != ref_name:
      continue
    raw_idx = _ref_idx(ref)
    if raw_idx in self_group:
      return (("__intra_group__", raw_idx),)
    if skip_external and raw_idx not in replacement:
      continue
    refs.append(_ref_signature(ref, replacement))
  return tuple(sorted(refs))


def _group_key(node: ET.Element) -> tuple[int, int] | None:
  group = node.attrib.get("simd_group")
  lanes = node.attrib.get("simd_lanes")
  if group is None or lanes is None:
    return None
  return int(group), int(lanes)


def _compatible_node_metadata(nodes: list[ET.Element]) -> bool:
  first = nodes[0]
  first_sig = (
    _child_text(first, "OP"),
    first.attrib.get("CONST"),
    _base_pointer(first),
    _gep_offset(first),
  )
  for node in nodes[1:]:
    if (
      _child_text(node, "OP"),
      node.attrib.get("CONST"),
      _base_pointer(node),
      _gep_offset(node),
    ) != first_sig:
      return False
  return True


def _is_packable_op(op: str | None, pack_load_store: bool) -> bool:
  if op in PACKABLE_COMPUTE_OPS:
    return True
  if pack_load_store and op in PACKABLE_MEMORY_OPS:
    return True
  return False


def _is_store_op(op: str | None) -> bool:
  return op in {
    "STORE",
    "STOREH",
    "STOREB",
    "OSTORE",
    "OSTOREH",
    "OSTOREB",
  }


def _is_ostore_op(op: str | None) -> bool:
  return op in {"OSTORE", "OSTOREH", "OSTOREB"}


def _is_oload_op(op: str | None) -> bool:
  return op in {"OLOAD", "OLOADH", "OLOADB"}


def _is_load_op(op: str | None) -> bool:
  return op in {"LOAD", "LOADH", "LOADB", "OLOAD", "OLOADH", "OLOADB"}


def _candidate_groups(
  nodes: list[ET.Element],
  lanes: int,
  pack_load_store: bool,
) -> dict[tuple[int, int], list[ET.Element]]:
  groups: dict[tuple[int, int], list[ET.Element]] = {}
  for node in nodes:
    key = _group_key(node)
    if key is None:
      continue
    if key[1] < lanes:
      continue
    groups.setdefault(key, []).append(node)

  candidates = {}
  for key, group_nodes in groups.items():
    sorted_nodes = sorted(group_nodes, key=_node_idx)
    if not _is_packable_op(_child_text(sorted_nodes[0], "OP"), pack_load_store):
      continue
    for chunk_idx, start in enumerate(range(0, len(sorted_nodes), lanes)):
      chunk = sorted_nodes[start : start + lanes]
      if len(chunk) != lanes:
        continue
      if not _compatible_node_metadata(chunk):
        continue
      candidates[(key[0], chunk_idx)] = chunk
  _add_phi_cmerge_candidates(candidates, nodes, lanes)
  _add_packed_output_store_candidates(candidates, nodes, lanes)
  return candidates


def _input_refs(node: ET.Element) -> list[ET.Element]:
  container = node.find("Inputs")
  if container is None:
    return []
  return [ref for ref in list(container) if ref.tag == "Input"]


def _add_phi_cmerge_candidates(
  candidates: dict[tuple[int, int], list[ET.Element]],
  nodes: list[ET.Element],
  lanes: int,
) -> None:
  """Infer CMERGE groups feeding a packed SELECT PHI.

  The DFG generator lowers PHIs into SELECT plus per-lane CMERGEs.  The SELECT
  nodes inherit SIMD metadata from the LLVM PHI, but the synthetic CMERGEs do
  not.  Infer those CMERGE groups from each SELECT candidate so recurrence
  packability is not blocked by scalar PHI plumbing.
  """
  by_idx = {_node_idx(node): node for node in nodes}
  next_key = 2000000000
  existing = {_node_idx(node) for group in candidates.values() for node in group}

  for group_nodes in list(candidates.values()):
    if _child_text(group_nodes[0], "OP") != "SELECT":
      continue
    arity = min((len(_input_refs(node)) for node in group_nodes), default=0)
    for input_pos in range(arity):
      cmerge_nodes = []
      for node in group_nodes:
        refs = _input_refs(node)
        input_idx = _ref_idx(refs[input_pos])
        input_node = by_idx.get(input_idx)
        if input_node is None or _child_text(input_node, "OP") != "CMERGE":
          cmerge_nodes = []
          break
        if _node_idx(input_node) in existing:
          cmerge_nodes = []
          break
        cmerge_nodes.append(input_node)
      if len(cmerge_nodes) != lanes:
        continue
      if not _compatible_node_metadata(cmerge_nodes):
        continue
      key = (next_key, 0)
      next_key += 1
      candidates[key] = cmerge_nodes
      existing.update(_node_idx(node) for node in cmerge_nodes)


def _output_refs(node: ET.Element) -> list[ET.Element]:
  container = node.find("Outputs")
  if container is None:
    return []
  return [ref for ref in list(container) if ref.tag == "Output"]


def _add_packed_output_store_candidates(
  candidates: dict[tuple[int, int], list[ET.Element]],
  nodes: list[ET.Element],
  lanes: int,
) -> None:
  """Infer packed live-out stores from a packed producer group.

  Deferred reduction mode intentionally leaves one scalar live-out per SIMD
  lane in the LLVM IR.  The mapper artifact should see those as one vector
  output, so collect sibling OSTOREs fed by each SIMD producer group.
  """
  by_idx = {_node_idx(node): node for node in nodes}
  next_key = 2100000000
  existing = {_node_idx(node) for group in candidates.values() for node in group}

  for group_nodes in list(candidates.values()):
    store_nodes = []
    for node in group_nodes:
      lane_stores = []
      for ref in _output_refs(node):
        target = by_idx.get(_ref_idx(ref))
        if target is None:
          continue
        if _child_text(target, "OP") in {
          "STORE", "STOREH", "STOREB",
          "OSTORE", "OSTOREH", "OSTOREB",
        }:
          lane_stores.append(target)
      if len(lane_stores) != 1:
        store_nodes = []
        break
      store_node = lane_stores[0]
      if _node_idx(store_node) in existing:
        store_nodes = []
        break
      store_nodes.append(store_node)
    if len(store_nodes) != lanes:
      continue
    key = (next_key, 0)
    next_key += 1
    candidates[key] = store_nodes
    existing.update(_node_idx(node) for node in store_nodes)


def _build_replacement(packable: dict[tuple[int, int], list[ET.Element]]) -> dict[int, int]:
  replacement = {}
  for group_nodes in packable.values():
    rep = _node_idx(group_nodes[0])
    for node in group_nodes:
      replacement[_node_idx(node)] = rep
  return replacement


def _prunable_scalar_lane_ostores(
  nodes: list[ET.Element],
  replacement: dict[int, int],
) -> set[int]:
  """Drop scalar lane live-outs already represented by a packed vector output."""
  packed_reps = set(replacement.values())
  pruned = set()
  for node in nodes:
    if not _is_ostore_op(_child_text(node, "OP")):
      continue
    inputs = _input_refs(node)
    if not inputs:
      continue
    if any(replacement.get(_ref_idx(ref), _ref_idx(ref)) in packed_reps for ref in inputs):
      pruned.add(_node_idx(node))
  return pruned


def _prunable_packed_store_address_oloads(
  nodes: list[ET.Element],
  replacement: dict[int, int],
  packed_store_reps: set[int],
) -> set[int]:
  """Drop scalar address OLOADs for packed stores when AGI is external."""
  pruned = set()
  for node in nodes:
    if not _is_oload_op(_child_text(node, "OP")):
      continue
    outputs = _output_refs(node)
    if not outputs:
      continue
    if all(replacement.get(_ref_idx(ref), _ref_idx(ref)) in packed_store_reps for ref in outputs):
      pruned.add(_node_idx(node))
  return pruned


def _is_artifact_store_root(node: ET.Element) -> bool:
  op = _child_text(node, "OP")
  if op in {"STORE", "STOREH"}:
    return True
  if op == "STOREB" and node.attrib.get("simd_packed") == "true":
    return True
  return False


def _prunable_external_control_nodes(
  nodes: list[ET.Element],
  replacement: dict[int, int],
  already_removed: set[int],
) -> set[int]:
  """Keep only the data cone needed by artifact stores.

  With remove_agi=true, loop iteration, loop flag stores, PHI merges, and scalar
  live-outs are driven externally or are not part of the mapped data artifact.
  A backward slice from real store roots keeps the compute/load path and prunes
  everything else.
  """
  by_idx = {_node_idx(node): node for node in nodes}
  available = set(by_idx) - already_removed
  roots = {
    _node_idx(node)
    for node in nodes
    if _node_idx(node) in available and _is_artifact_store_root(node)
  }
  if not roots:
    return set()

  keep = set()
  stack = list(roots)
  while stack:
    idx = stack.pop()
    idx = replacement.get(idx, idx)
    if idx in keep or idx not in available:
      continue
    keep.add(idx)
    node = by_idx.get(idx)
    if node is None:
      continue
    for ref in _input_refs(node):
      parent = replacement.get(_ref_idx(ref), _ref_idx(ref))
      if parent in available and parent not in keep:
        stack.append(parent)

  return available - keep


def _splat_load_reps(packable: dict[tuple[int, int], list[ET.Element]]) -> set[int]:
  """Packed loads of the same scalar address should be lane replicated.

  The architecture has no broadcast op, so an invariant scalar load used by all
  SIMD lanes is represented as a normal packed load whose trace repeats lane 0.
  """
  splat = set()
  for group_nodes in packable.values():
    if not group_nodes:
      continue
    if not _is_load_op(_child_text(group_nodes[0], "OP")):
      continue
    first_addr = (_base_pointer(group_nodes[0]), _gep_offset(group_nodes[0]), group_nodes[0].attrib.get("CONST"))
    if all(
      (_base_pointer(node), _gep_offset(node), node.attrib.get("CONST")) == first_addr
      for node in group_nodes[1:]
    ):
      splat.add(_node_idx(group_nodes[0]))
  return splat


def _prune_packable_groups(
  candidates: dict[tuple[int, int], list[ET.Element]],
) -> dict[tuple[int, int], list[ET.Element]]:
  packable = dict(candidates)
  changed = True
  while changed:
    changed = False
    replacement = _build_replacement(packable)
    remove_keys = []
    for key, group_nodes in packable.items():
      self_group = {_node_idx(node) for node in group_nodes}
      first = group_nodes[0]
      op = _child_text(first, "OP")
      skip_external_inputs = op in {"SELECT", "CMERGE"} or _is_store_op(op)
      first_inputs = _refs_signature(
        first, "Inputs", "Input", replacement, self_group,
        skip_external=skip_external_inputs,
      )
      first_outputs = _refs_signature(
        first, "Outputs", "Output", replacement, self_group,
        skip_external=True,
      )
      first_rec = _refs_signature(first, "RecParents", "RecParent", replacement, self_group)
      for node in group_nodes[1:]:
        if _refs_signature(
          node, "Inputs", "Input", replacement, self_group,
          skip_external=skip_external_inputs,
        ) != first_inputs:
          remove_keys.append(key)
          break
        if _refs_signature(
          node, "Outputs", "Output", replacement, self_group,
          skip_external=True,
        ) != first_outputs:
          remove_keys.append(key)
          break
        if _refs_signature(node, "RecParents", "RecParent", replacement, self_group) != first_rec:
          remove_keys.append(key)
          break
    for key in remove_keys:
      if key in packable:
        del packable[key]
        changed = True
  return packable


def _rewrite_refs(
  node: ET.Element,
  old_node_idx: int,
  container_name: str,
  ref_name: str,
  replacement: dict[int, int],
  reindex: dict[int, int],
  source_refs: list[ET.Element] | None = None,
) -> None:
  container = node.find(container_name)
  if container is None:
    return

  original_refs = source_refs
  if original_refs is None:
    original_refs = [ref for ref in list(container) if ref.tag == ref_name]
  for ref in original_refs:
    if ref in list(container):
      container.remove(ref)

  if source_refs is not None:
    for ref in [ref for ref in list(container) if ref.tag == ref_name]:
      container.remove(ref)

  seen = set()
  for ref in original_refs:
    raw_idx = _ref_idx(ref)
    mapped_idx = replacement.get(raw_idx, raw_idx)
    if mapped_idx == old_node_idx:
      continue
    if mapped_idx not in reindex:
      continue

    new_ref = copy.deepcopy(ref)
    new_ref.set("idx", str(reindex[mapped_idx]))
    signature = (new_ref.get("idx"), tuple(sorted(new_ref.attrib.items())))
    if signature in seen:
      continue
    seen.add(signature)
    container.append(new_ref)


def pack_simd_xml(
  input_xml: str | Path,
  output_xml: str | Path,
  *,
  lanes: int,
  pack_load_store: bool = True,
  prune_scalar_lane_outputs: bool = False,
  prune_iter_control: bool = False,
) -> SimdPackReport:
  input_path = Path(input_xml)
  output_path = Path(output_xml)
  root = _read_wrapped_xml(input_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("DFG XML missing <DFG> element: %s" % input_path)

  nodes = list(dfg.findall("Node"))
  input_nodes = len(nodes)
  candidates = _candidate_groups(nodes, lanes, pack_load_store)
  packable = _prune_packable_groups(candidates)
  replacement = _build_replacement(packable)
  pack_group_by_rep = {
    _node_idx(group_nodes[0]): group_nodes
    for group_nodes in packable.values()
  }
  removed = {
    _node_idx(node)
    for group_nodes in packable.values()
    for node in group_nodes[1:]
  }
  artifact_pruned = set()
  artifact_vars_pruned = set()
  if prune_scalar_lane_outputs:
    artifact_pruned = _prunable_scalar_lane_ostores(nodes, replacement) - removed
    packed_store_reps = {
      _node_idx(group_nodes[0])
      for group_nodes in packable.values()
      if _is_store_op(_child_text(group_nodes[0], "OP"))
    }
    artifact_pruned |= (
      _prunable_packed_store_address_oloads(nodes, replacement, packed_store_reps)
      - removed
    )
  if prune_iter_control:
    artifact_pruned |= (
      _prunable_external_control_nodes(nodes, replacement, removed | artifact_pruned)
      - removed
    )
  if prune_scalar_lane_outputs or prune_iter_control:
    artifact_vars_pruned = {
      name
      for node in nodes
      if _node_idx(node) in artifact_pruned
      for name in [_base_pointer_name(node)]
      if name
    }

  removed_or_pruned = removed | artifact_pruned
  remaining_nodes = [node for node in nodes if _node_idx(node) not in removed_or_pruned]
  reindex = {_node_idx(node): idx for idx, node in enumerate(remaining_nodes)}

  # Capture pre-packing indices before mutating node elements below.
  simd_groups = {}
  splat_reps = _splat_load_reps(packable)
  simd_splat_groups = set()
  for group_nodes in packable.values():
    rep_pre_idx = _node_idx(group_nodes[0])
    rep_post_idx = reindex[rep_pre_idx]
    simd_groups[rep_post_idx] = [_node_idx(n) for n in group_nodes]
    if rep_pre_idx in splat_reps:
      simd_splat_groups.add(rep_post_idx)

  for node in nodes:
    dfg.remove(node)

  rep_ids = {replacement_idx for replacement_idx in replacement.values()}
  for node in remaining_nodes:
    old_idx = _node_idx(node)
    node.set("idx", str(reindex[old_idx]))
    if old_idx in rep_ids:
      node.set("simd_packed", "true")
      node.set("simd_lanes", str(lanes))
    _rewrite_refs(node, old_idx, "Inputs", "Input", replacement, reindex)
    output_source_refs = None
    if old_idx in pack_group_by_rep:
      output_source_refs = []
      for group_node in pack_group_by_rep[old_idx]:
        container = group_node.find("Outputs")
        if container is not None:
          output_source_refs.extend(
            ref for ref in list(container) if ref.tag == "Output"
          )
    _rewrite_refs(
      node, old_idx, "Outputs", "Output", replacement, reindex,
      source_refs=output_source_refs,
    )
    _rewrite_refs(node, old_idx, "RecParents", "RecParent", replacement, reindex)
    dfg.append(node)

  dfg.set("count", str(len(remaining_nodes)))
  _write_unwrapped_xml(root, output_path)

  report = SimdPackReport(
    input_nodes=input_nodes,
    output_nodes=len(remaining_nodes),
    packed_groups=len(packable),
    packed_nodes_removed=len(removed),
    artifact_nodes_pruned=len(artifact_pruned),
    artifact_vars_pruned=tuple(sorted(artifact_vars_pruned)),
  )
  return report, SimdMapping(
    reindex=dict(reindex),
    simd_groups=simd_groups,
    simd_splat_groups=frozenset(simd_splat_groups),
  )


def filter_trace_vars(
  input_path: "str | Path",
  output_path: "str | Path",
  pruned_vars: "set[str] | tuple[str, ...] | list[str]",
) -> None:
  pruned = set(pruned_vars)
  if not pruned:
    return
  input_path = Path(input_path)
  output_path = Path(output_path)
  lines = input_path.read_text(encoding="utf-8").splitlines()
  kept = []
  for line in lines:
    if not line:
      kept.append(line)
      continue
    parts = line.split(",")
    if parts[0] == "var_name" or parts[0] not in pruned:
      kept.append(line)
  output_path.write_text("\n".join(kept) + ("\n" if kept else ""), encoding="utf-8")


def _is_splat_lane_entries(entries: list[tuple[str, ...]]) -> bool:
  if not entries:
    return False
  first = entries[0]
  return all(entry[1:] == first[1:] for entry in entries[1:])


def remap_addr_trace(
  input_path: "str | Path",
  output_path: "str | Path",
  mapping: SimdMapping,
) -> None:
  """Rewrite an addr_trace file to use post-SIMD-packing node indices.

  The instrumented binary records one address per pre-packing DFG node.
  After SIMD packing, node indices change and L nodes are collapsed into
  one packed node.  This rebuilds the trace so each packed node has L
  consecutive addresses per loop iteration (one per lane), as the
  artifact simulator expects.
  """
  entries_by_pre_idx: dict = {}
  with open(input_path, encoding="utf-8") as f:
    for line in f:
      line = line.rstrip("\n")
      if not line or line.startswith("node_idx"):
        continue
      parts = line.split(",")
      if len(parts) < 6:
        continue
      pre_idx = int(parts[0])
      # store (addr, size, is_store, base, ...)
      entries_by_pre_idx.setdefault(pre_idx, []).append(tuple(parts[2:]))

  entries_by_post_idx: dict = {}
  all_simd_pre: set = set()

  for post_idx, pre_indices in mapping.simd_groups.items():
    all_simd_pre.update(pre_indices)
    lane_entries = [entries_by_pre_idx.get(p, []) for p in pre_indices]
    n_iters = max((len(e) for e in lane_entries), default=0)
    interleaved = []
    for i in range(n_iters):
      if post_idx in mapping.simd_splat_groups:
        first_lane = lane_entries[0]
        if i < len(first_lane):
          interleaved.extend([first_lane[i]] * len(pre_indices))
          continue
      current_entries = [
        lane_e[i]
        for lane_e in lane_entries
        if i < len(lane_e)
      ]
      if _is_splat_lane_entries(current_entries):
        interleaved.extend([current_entries[0]] * len(current_entries))
        continue
      for lane_e in lane_entries:
        if i < len(lane_e):
          interleaved.append(lane_e[i])
    entries_by_post_idx[post_idx] = interleaved

  for pre_idx, post_idx in mapping.reindex.items():
    if pre_idx not in all_simd_pre:
      entries_by_post_idx[post_idx] = entries_by_pre_idx.get(pre_idx, [])

  lines = ["node_idx,access_idx,addr,size,is_store,base\n"]
  access_idx = 0
  for post_idx in sorted(entries_by_post_idx.keys()):
    for entry in entries_by_post_idx[post_idx]:
      lines.append("%d,%d,%s\n" % (post_idx, access_idx, ",".join(entry)))
      access_idx += 1

  Path(output_path).write_text("".join(lines), encoding="utf-8")
