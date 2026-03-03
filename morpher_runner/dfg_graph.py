"""Pure-Python DFG representation and XML I/O."""
import re
from collections import deque
from dataclasses import dataclass, field
from typing import Dict, List, Optional
from xml.etree import ElementTree as ET

# Fix malformed XML emitted by DFG generator: missing space between attributes.
# e.g.  ALAP="0"BB="name"  →  ALAP="0" BB="name"
# Pattern matches: closing quote of a value followed by the next attribute name and its '='.
# This avoids corrupting attribute values like type="I1", type="P", type="PS".
_MISSING_ATTR_SPACE_RE = re.compile(r'"([A-Za-z_][A-Za-z0-9_]*=)')


def _fix_xml_attr_spacing(text: str) -> str:
  return _MISSING_ATTR_SPACE_RE.sub(r'" \1', text)


@dataclass
class DFGEdge:
  src_idx: int
  dst_idx: int
  nextiter: int = 0
  edge_type: str = "I1"
  npb: int = 0


@dataclass
class DFGNode:
  idx: int
  asap: int
  alap: int
  bb: int
  const: int
  shift: int
  offset: int
  op: str
  inputs: List["DFGEdge"] = field(default_factory=list)
  outputs: List["DFGEdge"] = field(default_factory=list)
  rec_parents: List[int] = field(default_factory=list)


class DFGGraph:
  def __init__(self):
    self.nodes: List[DFGNode] = []
    self.iter_stride: int = 1
    self.iter_max_count: int = 1
    self.mutex_bbs: str = ""
    self._next_idx: int = 0

  def _node_by_idx(self, idx: int) -> Optional[DFGNode]:
    for n in self.nodes:
      if n.idx == idx:
        return n
    return None

  @staticmethod
  def from_xml(path: str) -> "DFGGraph":
    g = DFGGraph()
    # DFG XML has multiple top-level elements; wrap in a dummy root to parse.
    # Also fix missing spaces between attributes (emitted by DFG generator).
    with open(path, "r", encoding="utf-8") as f:
      content = f.read()
    content = _fix_xml_attr_spacing(content)
    wrapped = "<__ROOT__>" + content + "</__ROOT__>"
    root = ET.fromstring(wrapped)

    # Parse <MutexBB>
    mutex_el = root.find("MutexBB")
    if mutex_el is not None:
      g.mutex_bbs = ET.tostring(mutex_el, encoding="unicode")

    # Parse <ITER>
    iter_el = root.find("ITER")
    if iter_el is not None:
      g.iter_stride = int(iter_el.get("stride", "1"))
      g.iter_max_count = int(iter_el.get("max_count", "1"))

    # Parse <DFG>
    dfg_el = root.find("DFG")
    if dfg_el is None:
      raise RuntimeError("No <DFG> element found in %s" % path)

    node_map: Dict[int, DFGNode] = {}
    for node_el in dfg_el.findall("Node"):
      idx = int(node_el.get("idx"))
      asap = int(node_el.get("ASAP", "0"))
      alap = int(node_el.get("ALAP", "0"))
      # BB may be a string name in generated DFGs — normalize to 0
      bb_raw = node_el.get("BB", "0")
      try:
        bb = int(bb_raw)
      except ValueError:
        bb = 0
      try:
        const = int(node_el.get("CONST", "0"))
      except ValueError:
        const = 0
      shift = int(node_el.get("SHIFT", "0"))
      offset = int(node_el.get("OFFSET", "0"))
      op_el = node_el.find("OP")
      op = op_el.text.strip() if op_el is not None and op_el.text else "NOP"

      rec_parents = []
      rec_el = node_el.find("RecParents")
      if rec_el is not None:
        for rp in rec_el.findall("RecParent"):
          rec_parents.append(int(rp.get("idx")))

      node = DFGNode(
        idx=idx, asap=asap, alap=alap, bb=bb,
        const=const, shift=shift, offset=offset,
        op=op, rec_parents=rec_parents,
      )
      node_map[idx] = node
      g.nodes.append(node)

    # Now parse edges — outputs define edges
    for node_el in dfg_el.findall("Node"):
      src_idx = int(node_el.get("idx"))
      outputs_el = node_el.find("Outputs")
      if outputs_el is not None:
        for out_el in outputs_el.findall("Output"):
          dst_idx = int(out_el.get("idx"))
          nextiter = int(out_el.get("nextiter", "0"))
          edge_type = out_el.get("type", "I1").strip()
          npb_raw = out_el.get("NPB")
          try:
            npb = int(npb_raw) if npb_raw is not None else 0
          except ValueError:
            npb = 0
          edge = DFGEdge(
            src_idx=src_idx, dst_idx=dst_idx, nextiter=nextiter, edge_type=edge_type, npb=npb
          )
          if src_idx in node_map:
            node_map[src_idx].outputs.append(edge)
          if dst_idx in node_map:
            node_map[dst_idx].inputs.append(edge)

    g._next_idx = max((n.idx for n in g.nodes), default=-1) + 1
    return g

  def to_xml(self, path: str, reindex: bool = True) -> None:
    """Write DFG to XML file. If reindex=True, nodes are renumbered 0..N-1."""
    nodes = self.nodes

    if reindex:
      old_to_new: Dict[int, int] = {n.idx: i for i, n in enumerate(nodes)}
    else:
      old_to_new = {n.idx: n.idx for n in nodes}

    lines = []
    lines.append("<MutexBB>")
    lines.append("</MutexBB>")
    lines.append('<ITER stride="%d" max_count="%d">' % (self.iter_stride, self.iter_max_count))
    lines.append("</ITER>")
    lines.append('<DFG count="%d">' % len(nodes))

    for node in nodes:
      new_idx = old_to_new[node.idx]
      lines.append(
        '  <Node idx="%d" ASAP="%d" ALAP="%d" BB="%d" CONST="%d" SHIFT="%d" OFFSET="%d">'
        % (new_idx, node.asap, node.alap, node.bb, node.const, node.shift, node.offset)
      )
      lines.append("    <OP>%s</OP>" % node.op)

      # Inputs
      in_edges = [e for e in node.inputs if e.src_idx in old_to_new]
      if in_edges:
        lines.append("    <Inputs>")
        for e in in_edges:
          lines.append('      <Input idx="%d"/>' % old_to_new[e.src_idx])
        lines.append("    </Inputs>")
      else:
        lines.append("    <Inputs/>")

      # Outputs
      out_edges = [e for e in node.outputs if e.dst_idx in old_to_new]
      if out_edges:
        lines.append("    <Outputs>")
        for e in out_edges:
          if e.edge_type.upper().startswith("P"):
            lines.append(
              '      <Output idx="%d" nextiter="%d" NPB="%d" type="%s"/>'
              % (old_to_new[e.dst_idx], e.nextiter, e.npb, e.edge_type)
            )
          else:
            lines.append(
              '      <Output idx="%d" nextiter="%d" type="%s"/>'
              % (old_to_new[e.dst_idx], e.nextiter, e.edge_type)
            )
        lines.append("    </Outputs>")
      else:
        lines.append("    <Outputs/>")

      # RecParents
      valid_rec = [rp for rp in node.rec_parents if rp in old_to_new]
      if valid_rec:
        lines.append("    <RecParents>")
        for rp in valid_rec:
          lines.append('      <RecParent idx="%d"/>' % old_to_new[rp])
        lines.append("    </RecParents>")
      else:
        lines.append("    <RecParents/>")

      lines.append("  </Node>")

    lines.append("</DFG>")
    lines.append("")

    with open(path, "w", encoding="utf-8") as f:
      f.write("\n".join(lines))

  def recompute_asap(self) -> None:
    """BFS from source nodes to compute ASAP. Boundary LOAD nodes (no in-partition inputs) get ASAP=0."""
    idx_set = {n.idx for n in self.nodes}
    node_map = {n.idx: n for n in self.nodes}

    # Reset all ASAP
    for n in self.nodes:
      n.asap = -1

    # In-degree within this partition (ignoring nextiter edges)
    in_deg = {n.idx: 0 for n in self.nodes}
    for n in self.nodes:
      for e in n.inputs:
        if e.src_idx in idx_set and e.nextiter == 0:
          in_deg[n.idx] += 1

    queue = deque()
    for n in self.nodes:
      if in_deg[n.idx] == 0:
        n.asap = 0
        queue.append(n)

    while queue:
      n = queue.popleft()
      for e in n.outputs:
        if e.dst_idx not in idx_set or e.nextiter != 0:
          continue
        dst = node_map[e.dst_idx]
        new_asap = n.asap + 1
        if new_asap > dst.asap:
          dst.asap = new_asap
        in_deg[dst.idx] -= 1
        if in_deg[dst.idx] == 0:
          queue.append(dst)

    # Set ALAP = ASAP (conservative)
    for n in self.nodes:
      if n.asap < 0:
        n.asap = 0
      n.alap = n.asap

  def topological_nodes(self) -> List[DFGNode]:
    """Return nodes sorted by (asap, idx)."""
    return sorted(self.nodes, key=lambda n: (n.asap, n.idx))

  def op_histogram(self) -> Dict[str, int]:
    hist: Dict[str, int] = {}
    for n in self.nodes:
      hist[n.op] = hist.get(n.op, 0) + 1
    return hist

  def add_node(self, op: str, asap: int, alap: int, bb: int = 0) -> DFGNode:
    idx = self._next_idx
    self._next_idx += 1
    node = DFGNode(
      idx=idx, asap=asap, alap=alap, bb=bb,
      const=0, shift=0, offset=0, op=op,
    )
    self.nodes.append(node)
    return node

  def add_edge(
    self, src_idx: int, dst_idx: int, nextiter: int = 0, edge_type: str = "I1", npb: int = 0
  ) -> DFGEdge:
    src_map = {n.idx: n for n in self.nodes}
    edge = DFGEdge(src_idx=src_idx, dst_idx=dst_idx, nextiter=nextiter, edge_type=edge_type, npb=npb)
    if src_idx in src_map:
      src_map[src_idx].outputs.append(edge)
    if dst_idx in src_map:
      src_map[dst_idx].inputs.append(edge)
    return edge
