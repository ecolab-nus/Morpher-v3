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

ASSOCIATIVE_REDUCTION_OPS = {"ADD", "AND", "OR", "XOR"}


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
  simd_reduced_store_groups: frozenset = frozenset()  # post_idx values collapsed from lane stores to one scalar trace.


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


def _vector_op_name(op: str | None) -> str | None:
  if op is None:
    return None
  if op.startswith("V"):
    return op
  return "V%s" % op


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
  scalar_broadcast: bool,
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
      if not scalar_broadcast and _requires_scalar_broadcast(chunk):
        continue
      candidates[(key[0], chunk_idx)] = chunk
  _add_phi_cmerge_candidates(candidates, nodes, lanes)
  _add_packed_output_store_candidates(candidates, nodes, lanes)
  return candidates


def _requires_scalar_broadcast(group_nodes: list[ET.Element]) -> bool:
  if not group_nodes:
    return False
  op = _child_text(group_nodes[0], "OP")
  if _is_load_op(op) or _is_store_op(op):
    return False

  arity = min((len(_input_refs(node)) for node in group_nodes), default=0)
  for input_pos in range(arity):
    ref_type = (_input_refs(group_nodes[0])[input_pos].attrib.get("type") or "").strip()
    if ref_type in {"P", "PS"}:
      continue
    lane_refs = []
    for node in group_nodes:
      refs = _input_refs(node)
      if input_pos >= len(refs):
        return False
      lane_refs.append(_ref_idx(refs[input_pos]))
    if lane_refs and len(set(lane_refs)) == 1:
      return True
  return False


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


def _find_vred_store_groups(
  nodes: list[ET.Element],
  packable: dict[tuple[int, int], list[ET.Element]],
  replacement: dict[int, int],
  vector_ops: "set[str]",
) -> dict[int, dict[str, object]]:
  if "VRED" not in vector_ops:
    return {}

  pack_group_by_rep = {
    _node_idx(group_nodes[0]): group_nodes
    for group_nodes in packable.values()
  }
  by_idx = {_node_idx(node): node for node in nodes}
  vred_groups = {}
  for store_rep, store_group_nodes in pack_group_by_rep.items():
    store_op = _child_text(store_group_nodes[0], "OP")
    if store_op not in {"STORE", "STOREH", "STOREB", "OSTORE", "OSTOREH", "OSTOREB"}:
      continue

    reduce_op = store_group_nodes[0].attrib.get("simd_reduce_op")
    reduce_base = store_group_nodes[0].attrib.get("simd_reduce_base")
    if not reduce_op or not reduce_base:
      continue
    if any(
      node.attrib.get("simd_reduce_op") != reduce_op
      or node.attrib.get("simd_reduce_base") != reduce_base
      for node in store_group_nodes
    ):
      continue

    producer_reps = set()
    for store_node in store_group_nodes:
      refs = _input_refs(store_node)
      if not refs:
        producer_reps = set()
        break
      producer_reps.add(replacement.get(_ref_idx(refs[0]), _ref_idx(refs[0])))
    if len(producer_reps) != 1:
      continue

    producer_rep = next(iter(producer_reps))
    if producer_rep == store_rep or producer_rep not in pack_group_by_rep:
      continue

    final_store = None
    final_add = None
    init_load = None
    for node in nodes:
      if _base_pointer_name(node) != reduce_base or not _is_ostore_op(_child_text(node, "OP")):
        continue
      refs = _input_refs(node)
      if len(refs) != 1:
        continue
      add_node = by_idx.get(_ref_idx(refs[0]))
      if add_node is None or _child_text(add_node, "OP") != reduce_op:
        continue
      add_inputs = _input_refs(add_node)
      if len(add_inputs) != 2:
        continue
      init_candidate = None
      reduce_input_idx = None
      for add_ref in add_inputs:
        candidate = by_idx.get(_ref_idx(add_ref))
        if candidate is not None and _is_oload_op(_child_text(candidate, "OP")):
          init_candidate = candidate
        else:
          reduce_input_idx = _ref_idx(add_ref)
      if init_candidate is None or reduce_input_idx is None:
        continue
      final_store = node
      final_add = add_node
      init_load = init_candidate
      break

    if final_store is None or final_add is None or init_load is None:
      continue

    vred_groups[store_rep] = {
      "producer_rep": producer_rep,
      "reduce_op": reduce_op,
      "reduce_base": reduce_base,
      "final_store_idx": _node_idx(final_store),
      "final_add_idx": _node_idx(final_add),
      "init_load_idx": _node_idx(init_load),
      "scalar_reduce_idx": int(reduce_input_idx),
    }
  return vred_groups


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
  vector_ops: frozenset[str] | set[str] | tuple[str, ...],
  scalar_broadcast: bool = False,
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
  candidates = _candidate_groups(nodes, lanes, True, scalar_broadcast)
  packable = _prune_packable_groups(candidates)
  replacement = _build_replacement(packable)
  vred_store_groups = _find_vred_store_groups(nodes, packable, replacement, set(vector_ops))
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
  iter_pruned: set[int] = set()
  if prune_iter_control:
    iter_pruned = (
      _prunable_external_control_nodes(nodes, replacement, removed | artifact_pruned)
      - removed
    )
    artifact_pruned |= iter_pruned
  vred_tail_keep = set()
  vred_extra_prune = set()
  for store_rep, meta in vred_store_groups.items():
    vred_tail_keep.update({
      int(meta["init_load_idx"]),
      int(meta["final_add_idx"]),
      int(meta["final_store_idx"]),
    })
    vred_extra_prune.update({
      int(store_rep),
      int(meta["scalar_reduce_idx"]),
    })
  artifact_pruned -= vred_tail_keep
  artifact_pruned |= vred_extra_prune
  if prune_scalar_lane_outputs or prune_iter_control:
    # Bases that surviving nodes still write to must remain in the trace.
    # The VRED rewrite (below) re-bases the rep OSTORE to its simd_reduce_base
    # — usually the same name as the pruned lane OSTOREs — so filtering that
    # name out of the trace would leave the simulator with no expected values
    # at the post-VRED store location.
    surviving_idx = {
      _node_idx(node)
      for node in nodes
      if _node_idx(node) not in (removed | artifact_pruned)
    }
    surviving_bases: set = set()
    for node in nodes:
      if _node_idx(node) not in surviving_idx:
        continue
      base = _base_pointer_name(node)
      if base:
        surviving_bases.add(base)
      reduce_base = node.attrib.get("simd_reduce_base")
      if reduce_base and _node_idx(node) in vred_store_groups:
        surviving_bases.add(reduce_base)
    artifact_vars_pruned = {
      name
      for node in nodes
      if _node_idx(node) in artifact_pruned
      for name in [_base_pointer_name(node)]
      if name and name not in surviving_bases
    }

  removed_or_pruned = removed | artifact_pruned
  remaining_nodes = [node for node in nodes if _node_idx(node) not in removed_or_pruned]
  reindex = {_node_idx(node): idx for idx, node in enumerate(remaining_nodes)}

  if iter_pruned:
    iter_local_idx = {old: i for i, old in enumerate(sorted(iter_pruned))}
    iter_subgraph_path = output_path.parent / output_path.name.replace(
      "_PartPredDFG.xml", "_ITER_subgraph.xml"
    )
    if iter_subgraph_path == output_path:
      iter_subgraph_path = output_path.parent / (output_path.stem + "_ITER_subgraph.xml")
    _write_iter_subgraph(nodes, iter_pruned, iter_local_idx, reindex, iter_subgraph_path)

  # Compose lane->rep with the post-pack reindex so AGI externalIdx values
  # land in the mapper's final namespace. Lanes 1..L-1 collapse to lane 0;
  # last-wins in the XML is acceptable (lane 0 is the canonical representative
  # for v1 partition/replication decisions).
  composed = {pre: reindex[replacement.get(pre, pre)]
              for pre in {_node_idx(n) for n in nodes}
              if replacement.get(pre, pre) in reindex}
  remap_agi_subgraph_externalidx(input_path.parent, composed)

  # Capture pre-packing indices before mutating node elements below.
  simd_groups = {}
  splat_reps = _splat_load_reps(packable)
  simd_splat_groups = set()
  simd_reduced_store_groups = set()
  for group_nodes in packable.values():
    rep_pre_idx = _node_idx(group_nodes[0])
    if rep_pre_idx not in reindex:
      continue
    rep_post_idx = reindex[rep_pre_idx]
    simd_groups[rep_post_idx] = [_node_idx(n) for n in group_nodes]
    if rep_pre_idx in splat_reps:
      simd_splat_groups.add(rep_post_idx)
    if rep_pre_idx in vred_store_groups:
      simd_reduced_store_groups.add(rep_post_idx)

  for node in nodes:
    dfg.remove(node)

  rep_ids = {replacement_idx for replacement_idx in replacement.values()}
  vector_ops = set(vector_ops)
  reindexed_nodes_by_old_idx: dict[int, ET.Element] = {}
  vred_final_store_ids = {
    int(meta["final_store_idx"])
    for meta in vred_store_groups.values()
  }
  for node in remaining_nodes:
    old_idx = _node_idx(node)
    node.set("idx", str(reindex[old_idx]))
    reindexed_nodes_by_old_idx[old_idx] = node
    if old_idx in vred_final_store_ids:
      node.attrib.pop("CONST", None)
      node.set("simd_packed", "true")
      node.set("simd_lanes", str(lanes))
      node.set("simd_reduce_consumer", "true")
    if old_idx in rep_ids:
      if old_idx not in vred_store_groups:
        op_elem = node.find("OP")
        scalar_op = _child_text(node, "OP")
        if not _is_ostore_op(scalar_op):
          vector_op = _vector_op_name(scalar_op)
          if vector_op not in vector_ops:
            raise RuntimeError(
              "Architecture SIMD capability does not expose required vector op '%s' for packed scalar op '%s'."
              % (vector_op, scalar_op)
            )
          if op_elem is not None:
            op_elem.text = vector_op
        node.set("simd_packed", "true")
        node.set("simd_lanes", str(lanes))
        if scalar_op in ASSOCIATIVE_REDUCTION_OPS:
          node.set("simd_reduce_op", scalar_op)
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

  next_idx = len(remaining_nodes)
  for store_rep, vred_meta in vred_store_groups.items():
    producer_rep = int(vred_meta["producer_rep"])
    reduce_op = str(vred_meta["reduce_op"])
    final_add_idx = int(vred_meta["final_add_idx"])
    if producer_rep not in reindex:
      raise RuntimeError("VRED producer representative missing from SIMD reindex map.")
    if final_add_idx not in reindex:
      raise RuntimeError("VRED final ADD missing from SIMD reindex map.")
    final_add_node = reindexed_nodes_by_old_idx.get(final_add_idx)
    if final_add_node is None:
      raise RuntimeError("VRED final ADD node missing after SIMD packing.")
    vred_idx = next_idx
    next_idx += 1

    inputs = final_add_node.find("Inputs")
    if inputs is None:
      inputs = ET.SubElement(final_add_node, "Inputs")
    ET.SubElement(inputs, "Input", idx=str(vred_idx), type="I2")

    producer_node = reindexed_nodes_by_old_idx.get(producer_rep)
    if producer_node is not None:
      outputs = producer_node.find("Outputs")
      if outputs is not None:
        for ref in list(outputs):
          if ref.tag != "Output":
            continue
          if ref.attrib.get("idx") == str(reindex[final_add_idx]):
            outputs.remove(ref)
        ET.SubElement(outputs, "Output", idx=str(vred_idx), nextiter="0", type="I1")

    sample_group = pack_group_by_rep[producer_rep]
    sample_node = sample_group[0]
    producer_asap = int(sample_node.attrib.get("ASAP", final_add_node.attrib.get("ASAP", "0")))
    producer_alap = int(sample_node.attrib.get("ALAP", final_add_node.attrib.get("ALAP", "0")))
    add_asap = int(final_add_node.attrib.get("ASAP", str(producer_asap + 1)))
    add_alap = int(final_add_node.attrib.get("ALAP", str(max(producer_alap + 1, add_asap))))
    vred_asap = max(producer_asap + 1, min(add_asap, producer_alap + 1))
    vred_alap = max(vred_asap, min(add_alap, producer_alap + 1))
    vred_node = ET.Element(
      "Node",
      idx=str(vred_idx),
      ASAP=str(vred_asap),
      ALAP=str(vred_alap),
      BB=sample_node.attrib.get("BB", final_add_node.attrib.get("BB", "")),
      simd_reduce_op=str(reduce_op),
    )
    op_elem = ET.SubElement(vred_node, "OP")
    op_elem.text = "VRED"
    vred_inputs = ET.SubElement(vred_node, "Inputs")
    ET.SubElement(vred_inputs, "Input", idx=str(reindex[producer_rep]), type="I1")
    vred_outputs = ET.SubElement(vred_node, "Outputs")
    ET.SubElement(vred_outputs, "Output", idx=str(reindex[final_add_idx]), nextiter="0", type="I2")
    ET.SubElement(vred_node, "RecParents")
    dfg.append(vred_node)

  dfg.set("count", str(next_idx))
  _write_unwrapped_xml(root, output_path)

  report = SimdPackReport(
    input_nodes=input_nodes,
    output_nodes=next_idx,
    packed_groups=len(packable),
    packed_nodes_removed=len(removed),
    artifact_nodes_pruned=len(artifact_pruned),
    artifact_vars_pruned=tuple(sorted(artifact_vars_pruned)),
  )
  return report, SimdMapping(
    reindex=dict(reindex),
    simd_groups=simd_groups,
    simd_splat_groups=frozenset(simd_splat_groups),
    simd_reduced_store_groups=frozenset(simd_reduced_store_groups),
  )


def prune_iter_control_xml(
  input_xml: str,
  output_xml: str,
) -> "tuple[int, dict[int, int]]":
  """Apply iter-control pruning to a scalar (non-SIMD) DFG XML.

  Returns (n_pruned, reindex) where reindex maps old node idx to new node idx.
  Callers should pass reindex to reindex_addr_trace so the simulator can match
  external addresses to the correct post-pruning node indices.

  Also writes a companion *_ITER_subgraph.xml next to output_xml containing the
  pruned nodes. Intra-ITER refs use a local idx; refs across the boundary to a
  surviving compute node use externalIdx in the post-prune index space.
  """
  input_path = Path(input_xml)
  output_path = Path(output_xml)
  root = _read_wrapped_xml(input_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("DFG XML missing <DFG> element: %s" % input_path)
  nodes = list(dfg.findall("Node"))
  pruned = _prunable_external_control_nodes(nodes, {}, set())
  remaining = [n for n in nodes if _node_idx(n) not in pruned]
  reindex = {_node_idx(n): idx for idx, n in enumerate(remaining)}

  iter_local_idx = {old: i for i, old in enumerate(sorted(pruned))}
  iter_subgraph_path = output_path.parent / output_path.name.replace(
    "_noiter_PartPredDFG.xml", "_ITER_subgraph.xml"
  )
  if iter_subgraph_path == output_path:
    iter_subgraph_path = output_path.parent / (output_path.stem + "_ITER_subgraph.xml")
  _write_iter_subgraph(nodes, pruned, iter_local_idx, reindex, iter_subgraph_path)
  remap_agi_subgraph_externalidx(input_path.parent, reindex)

  for node in nodes:
    dfg.remove(node)
  for node in remaining:
    old_idx = _node_idx(node)
    node.set("idx", str(reindex[old_idx]))
    _rewrite_refs(node, old_idx, "Inputs", "Input", {}, reindex)
    _rewrite_refs(node, old_idx, "Outputs", "Output", {}, reindex)
    _rewrite_refs(node, old_idx, "RecParents", "RecParent", {}, reindex)
    dfg.append(node)
  dfg.set("count", str(len(remaining)))
  _write_unwrapped_xml(root, output_path)
  return len(pruned), reindex


def remap_agi_subgraph_externalidx(
  search_dir: Path,
  reindex: "dict[int, int]",
) -> None:
  """Walk *_AGI_subgraph.xml under search_dir and rewrite externalIdx
  attributes (on Input and Output refs) using `reindex`. Refs whose target
  is missing from `reindex` are dropped — they pointed at a node that the
  caller's reindex step removed entirely.

  Called by prune_iter_control_xml (ITER pruning reindex) and pack_simd_xml
  (post-pack reindex composed with the lane->rep replacement map) so the
  AGI sub-graph's externalIdx values track the compute DFG's final indices.
  """
  if not reindex:
    return
  for agi_path in sorted(search_dir.glob("*_AGI_subgraph.xml")):
    try:
      tree = ET.parse(agi_path)
    except ET.ParseError:
      continue
    root = tree.getroot()
    changed = False
    for node in root.findall("Node"):
      for container_name, ref_name in (("Inputs", "Input"), ("Outputs", "Output")):
        container = node.find(container_name)
        if container is None:
          continue
        to_drop = []
        for ref in list(container):
          if ref.tag != ref_name:
            continue
          ext = ref.attrib.get("externalIdx")
          if ext is None:
            continue
          try:
            old = int(ext)
          except ValueError:
            continue
          new = reindex.get(old)
          if new is None:
            to_drop.append(ref)
            continue
          if str(new) != ext:
            ref.set("externalIdx", str(new))
            changed = True
        for ref in to_drop:
          container.remove(ref)
          changed = True
    if changed:
      try:
        ET.indent(root, space="  ")
      except AttributeError:
        pass
      agi_path.write_text(ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8")
      print(f"[agi_remap] Rewrote externalIdx in {agi_path.name}")


def _write_iter_subgraph(
  nodes: list[ET.Element],
  pruned: set[int],
  iter_local_idx: dict[int, int],
  reindex: dict[int, int],
  out_path: Path,
) -> None:
  root = ET.Element("ITER_DFG", attrib={"count": str(len(pruned))})
  for node in nodes:
    old_idx = _node_idx(node)
    if old_idx not in pruned:
      continue
    new_node = ET.SubElement(root, "Node", attrib={"localIdx": str(iter_local_idx[old_idx])})
    bb = node.attrib.get("BB")
    if bb:
      new_node.set("BB", bb)
    const = node.attrib.get("CONST")
    if const is not None:
      new_node.set("CONST", const)
    op_elem = node.find("OP")
    op_text = (op_elem.text or "").strip() if op_elem is not None and op_elem.text else ""
    op_out = ET.SubElement(new_node, "OP")
    op_out.text = op_text
    for container_name, ref_name in (("Inputs", "Input"), ("Outputs", "Output")):
      src = node.find(container_name)
      out_container = ET.SubElement(new_node, container_name)
      if src is None:
        continue
      for ref in list(src):
        if ref.tag != ref_name:
          continue
        partner = _ref_idx(ref)
        attribs = {k: v for k, v in ref.attrib.items() if k != "idx"}
        if partner in pruned:
          attribs["localIdx"] = str(iter_local_idx[partner])
        elif partner in reindex:
          attribs["externalIdx"] = str(reindex[partner])
        else:
          continue
        ET.SubElement(out_container, ref_name, attrib=attribs)
  try:
    ET.indent(root, space="  ")
    xml_text = ET.tostring(root, encoding="unicode")
  except AttributeError:
    from xml.dom import minidom
    xml_text = minidom.parseString(ET.tostring(root, encoding="unicode")).toprettyxml(indent="  ")
    xml_text = "\n".join(line for line in xml_text.splitlines() if line.strip())
    if xml_text.startswith("<?xml"):
      xml_text = xml_text.split("\n", 1)[1]
  out_path.write_text(xml_text + "\n", encoding="utf-8")


def reindex_addr_trace(
  input_path: "str | Path",
  output_path: "str | Path",
  reindex: "dict[int, int]",
) -> None:
  """Rewrite an addr_trace to use post-pruning node indices.

  The instrumented binary records addresses with pre-pruning DFG node indices.
  After iter-control pruning the indices change; this remaps them so the
  artifact simulator can match external addresses to the correct nodes.
  Entries for pruned nodes (not in reindex) are dropped.
  """
  lines_out = ["node_idx,access_idx,addr,size,is_store,base\n"]
  access_idx = 0
  with open(input_path, encoding="utf-8") as f:
    for line in f:
      line = line.rstrip("\n")
      if not line or line.startswith("node_idx"):
        continue
      parts = line.split(",")
      if len(parts) < 6:
        continue
      old_idx = int(parts[0])
      new_idx = reindex.get(old_idx)
      if new_idx is None:
        continue
      lines_out.append("%d,%d,%s\n" % (new_idx, access_idx, ",".join(parts[2:])))
      access_idx += 1
  Path(output_path).write_text("".join(lines_out), encoding="utf-8")


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
      if post_idx in mapping.simd_reduced_store_groups:
        if lane_entries and i < len(lane_entries[0]):
          interleaved.append(lane_entries[0][i])
        continue
      if post_idx in mapping.simd_splat_groups:
        first_lane = lane_entries[0]
        if i < len(first_lane):
          # The static (base, gep_offset, const) signature can match across
          # lanes that nonetheless touch different addresses at runtime (the
          # offset is supplied implicitly by the simulator's stride or by a
          # dynamic input).  Verify against the actual lane entries before
          # replicating lane 0 — otherwise we discard real per-lane data.
          current_entries = [
            lane_e[i]
            for lane_e in lane_entries
            if i < len(lane_e)
          ]
          if len(current_entries) <= 1 or _is_splat_lane_entries(current_entries):
            interleaved.extend([first_lane[i]] * len(pre_indices))
            continue
          # Fall through to the interleave path below.
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
