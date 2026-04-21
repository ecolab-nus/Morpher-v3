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

  @property
  def pack_ratio(self) -> float:
    if self.input_nodes == 0:
      return 1.0
    return float(self.output_nodes) / float(self.input_nodes)


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
  return candidates


def _build_replacement(packable: dict[tuple[int, int], list[ET.Element]]) -> dict[int, int]:
  replacement = {}
  for group_nodes in packable.values():
    rep = _node_idx(group_nodes[0])
    for node in group_nodes:
      replacement[_node_idx(node)] = rep
  return replacement


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
      first_inputs = _refs_signature(first, "Inputs", "Input", replacement, self_group)
      first_outputs = _refs_signature(first, "Outputs", "Output", replacement, self_group)
      first_rec = _refs_signature(first, "RecParents", "RecParent", replacement, self_group)
      for node in group_nodes[1:]:
        if _refs_signature(node, "Inputs", "Input", replacement, self_group) != first_inputs:
          remove_keys.append(key)
          break
        if _refs_signature(node, "Outputs", "Output", replacement, self_group) != first_outputs:
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
) -> None:
  container = node.find(container_name)
  if container is None:
    return

  original_refs = [ref for ref in list(container) if ref.tag == ref_name]
  for ref in original_refs:
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
  removed = {
    _node_idx(node)
    for group_nodes in packable.values()
    for node in group_nodes[1:]
  }

  remaining_nodes = [node for node in nodes if _node_idx(node) not in removed]
  reindex = {_node_idx(node): idx for idx, node in enumerate(remaining_nodes)}

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
    _rewrite_refs(node, old_idx, "Outputs", "Output", replacement, reindex)
    _rewrite_refs(node, old_idx, "RecParents", "RecParent", replacement, reindex)
    dfg.append(node)

  dfg.set("count", str(len(remaining_nodes)))
  _write_unwrapped_xml(root, output_path)
  return SimdPackReport(
    input_nodes=input_nodes,
    output_nodes=len(remaining_nodes),
    packed_groups=len(packable),
    packed_nodes_removed=len(removed),
  )
