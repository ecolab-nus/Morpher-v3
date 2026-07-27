"""Manifest/trace bookkeeping for the verified C++ DFG transforms.

This module intentionally does not rewrite semantic DFG XML. AGI/iterator
externalization and SIMD packing are implemented by Morpher_DFG_Generator.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import xml.etree.ElementTree as ET  # building only; parsing goes through defusedxml
from defusedxml.ElementTree import parse as _safe_parse


@dataclass(frozen=True)
class SimdMapping:
  reindex: dict
  simd_groups: dict
  simd_splat_groups: frozenset = frozenset()
  simd_reduced_store_groups: frozenset = frozenset()
  simd_no_vector_word_groups: frozenset = frozenset()
  artifact_vars_pruned: frozenset = frozenset()
  post_pack_iterator_externalized: bool = False
  boundary_trace_vars: frozenset = frozenset()
  host_modeled_boundaries: int = 0


@dataclass(frozen=True)
class BoundaryMapping:
  reindex: dict = None
  artifact_vars_pruned: frozenset = frozenset()
  boundary_trace_vars: frozenset = frozenset()
  state_count: int = 0


def remap_agi_subgraph_externalidx(search_dir: Path, reindex: dict[int, int]) -> None:
  """Remap AGI-boundary references into the final C++ manifest index space."""
  if not reindex:
    return
  for agi_path in sorted(search_dir.glob("*_AGI_subgraph.xml")):
    try:
      tree = _safe_parse(agi_path)
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
          if ref.tag != ref_name or "externalIdx" not in ref.attrib:
            continue
          try:
            old = int(ref.attrib["externalIdx"])
          except ValueError:
            continue
          new = reindex.get(old)
          if new is None:
            to_drop.append(ref)
          elif str(new) != ref.attrib["externalIdx"]:
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


def reindex_addr_trace(input_path: str | Path, output_path: str | Path,
                       reindex: dict[int, int]) -> None:
  """Remap instrumented node indices using a validated C++ manifest."""
  lines_out = ["node_idx,access_idx,addr,size,is_store,base\n"]
  access_idx = 0
  with open(input_path, encoding="utf-8") as stream:
    for line in stream:
      line = line.rstrip("\n")
      if not line or line.startswith("node_idx"):
        continue
      parts = line.split(",")
      if len(parts) < 6:
        continue
      new_idx = reindex.get(int(parts[0]))
      if new_idx is None:
        continue
      lines_out.append("%d,%d,%s\n" % (new_idx, access_idx, ",".join(parts[2:])))
      access_idx += 1
  Path(output_path).write_text("".join(lines_out), encoding="utf-8")


def filter_trace_vars(input_path: str | Path, output_path: str | Path,
                      pruned_vars: set[str] | tuple[str, ...] | list[str]) -> None:
  """Drop trace variables explicitly identified by a validated manifest."""
  pruned = set(pruned_vars)
  if not pruned:
    return
  lines = Path(input_path).read_text(encoding="utf-8").splitlines()
  kept = []
  for line in lines:
    if not line:
      kept.append(line)
      continue
    parts = line.split(",")
    if parts[0] == "var_name" or parts[0] not in pruned:
      kept.append(line)
  Path(output_path).write_text("\n".join(kept) + ("\n" if kept else ""), encoding="utf-8")


def _is_splat_lane_entries(entries: list[tuple[str, ...]]) -> bool:
  return bool(entries) and all(entry == entries[0] for entry in entries[1:])


def _contiguous_vector_word_entry(entries: list[tuple[str, ...]]) -> tuple[str, ...] | None:
  """Collapse equal-size, contiguous scalar lane accesses into one SIMD word."""
  if not entries:
    return None
  try:
    size = int(entries[0][1])
  except (IndexError, ValueError):
    return None
  if size <= 0:
    return None
  is_store = entries[0][2] if len(entries[0]) > 2 else ""
  base = entries[0][3] if len(entries[0]) > 3 else ""
  parsed = []
  for entry in entries:
    try:
      addr = int(entry[0])
      lane_size = int(entry[1])
    except (IndexError, ValueError):
      return None
    if lane_size != size or entry[2] != is_store or entry[3] != base:
      return None
    parsed.append((addr, entry))
  parsed.sort(key=lambda item: item[0])
  if any(addr != parsed[0][0] + lane * size
         for lane, (addr, _entry) in enumerate(parsed)):
    return None
  vector_bytes = []
  for _addr, entry in parsed:
    vector_bytes.extend(entry[4:])
  return (str(parsed[0][0]), str(size * len(entries)), is_store, base, *vector_bytes)


def remap_addr_trace(input_path: str | Path, output_path: str | Path,
                     mapping: SimdMapping) -> None:
  """Remap scalar trace rows according to a validated C++ SIMD manifest."""
  entries_by_pre_idx = {}
  with open(input_path, encoding="utf-8") as stream:
    for line in stream:
      line = line.rstrip("\n")
      if not line or line.startswith("node_idx"):
        continue
      parts = line.split(",")
      if len(parts) >= 6:
        entries_by_pre_idx.setdefault(int(parts[0]), []).append(tuple(parts[2:]))

  entries_by_post_idx = {}
  all_simd_pre = set()
  disable_vector_word = bool(mapping.simd_no_vector_word_groups)
  for post_idx, pre_indices in mapping.simd_groups.items():
    all_simd_pre.update(pre_indices)
    lane_entries = [entries_by_pre_idx.get(pre, []) for pre in pre_indices]
    n_iters = max((len(entries) for entries in lane_entries), default=0)
    interleaved = []
    for iteration in range(n_iters):
      if post_idx in mapping.simd_reduced_store_groups:
        if lane_entries and iteration < len(lane_entries[0]):
          interleaved.append(lane_entries[0][iteration])
        continue
      current = [entries[iteration] for entries in lane_entries
                 if iteration < len(entries)]
      if post_idx in mapping.simd_splat_groups and current and _is_splat_lane_entries(current):
        interleaved.extend([current[0]] * len(pre_indices))
        continue
      if _is_splat_lane_entries(current):
        interleaved.extend([current[0]] * len(current))
        continue
      vector_entry = None if disable_vector_word else _contiguous_vector_word_entry(current)
      if vector_entry is not None:
        interleaved.append(vector_entry)
      else:
        for entries in lane_entries:
          if iteration < len(entries):
            interleaved.append(entries[iteration])
    entries_by_post_idx[post_idx] = interleaved

  for pre_idx, post_idx in mapping.reindex.items():
    if pre_idx not in all_simd_pre:
      entries_by_post_idx[post_idx] = entries_by_pre_idx.get(pre_idx, [])

  lines = ["node_idx,access_idx,addr,size,is_store,base\n"]
  access_idx = 0
  for post_idx in sorted(entries_by_post_idx):
    for entry in entries_by_post_idx[post_idx]:
      lines.append("%d,%d,%s\n" % (post_idx, access_idx, ",".join(entry)))
      access_idx += 1
  Path(output_path).write_text("".join(lines), encoding="utf-8")
