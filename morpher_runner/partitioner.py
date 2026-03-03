"""Partition a DFGGraph into II=1-feasible chunks with inter-partition STORE/LOAD pairs."""
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from morpher_runner.cgra_resources import compute_rec_mii, compute_res_mii
from morpher_runner.dfg_graph import DFGEdge, DFGGraph, DFGNode


class PartitioningError(RuntimeError):
  pass


@dataclass
class PartitionInfo:
  idx: int
  dfg: DFGGraph
  # scratch var name -> byte address for inter-partition communication values
  mem_alloc_additions: Dict[str, int] = field(default_factory=dict)
  res_mii: int = 1
  rec_mii: int = 0
  cross_edges_in: int = 0
  cross_edges_out: int = 0
  injected_loads: int = 0
  injected_stores: int = 0
  incoming_load_count: int = 0
  outgoing_store_count: int = 0


@dataclass
class _SccUnit:
  idx: int
  node_ids: List[int]
  hist: Dict[str, int]
  min_asap: int
  max_asap: int
  preds: Set[int] = field(default_factory=set)
  succs: Set[int] = field(default_factory=set)

  @property
  def size(self) -> int:
    return len(self.node_ids)


@dataclass
class _CrossReq:
  src_idx: int
  dst_part: int
  dst_idx: int
  edge_type: str
  npb: int = 0


def _op_histogram(nodes: List[DFGNode]) -> Dict[str, int]:
  hist: Dict[str, int] = {}
  for n in nodes:
    hist[n.op] = hist.get(n.op, 0) + 1
  return hist


def _merge_hist(base: Dict[str, int], add: Dict[str, int]) -> Dict[str, int]:
  merged = dict(base)
  for op, cnt in add.items():
    merged[op] = merged.get(op, 0) + cnt
  return merged


def _is_mem_op(op: str) -> bool:
  op_u = (op or "").upper()
  return op_u.startswith("LOAD") or op_u.startswith("STORE") or op_u == "LS"


def _is_data_edge(edge_type: str) -> bool:
  edge_u = (edge_type or "").upper()
  return edge_u.startswith("I")


def _is_dup_candidate_node(node: DFGNode) -> bool:
  # Keep this conservative: pure ALU-ish ops only.
  op = (node.op or "").upper()
  if _is_mem_op(op):
    return False
  if op in ("SELECT", "CMERGE", "OSTORE", "NOP"):
    return False
  return op in {
    "ADD", "SUB", "MUL", "AND", "OR", "XOR",
    "LS", "RS", "CMP", "CGT", "CLT", "MOVC",
  }


def _tarjan_scc(node_ids: List[int], edges: Dict[int, Set[int]]) -> List[List[int]]:
  index = 0
  stack: List[int] = []
  on_stack: Set[int] = set()
  indices: Dict[int, int] = {}
  lowlink: Dict[int, int] = {}
  result: List[List[int]] = []

  def strongconnect(v: int) -> None:
    nonlocal index
    indices[v] = index
    lowlink[v] = index
    index += 1
    stack.append(v)
    on_stack.add(v)

    for w in edges.get(v, set()):
      if w not in indices:
        strongconnect(w)
        lowlink[v] = min(lowlink[v], lowlink[w])
      elif w in on_stack:
        lowlink[v] = min(lowlink[v], indices[w])

    if lowlink[v] == indices[v]:
      comp: List[int] = []
      while stack:
        w = stack.pop()
        on_stack.remove(w)
        comp.append(w)
        if w == v:
          break
      result.append(comp)

  for nid in node_ids:
    if nid not in indices:
      strongconnect(nid)

  return result


def _build_scc_units(dfg: DFGGraph) -> Tuple[List[_SccUnit], Dict[int, int], Dict[Tuple[int, int], int]]:
  node_map = {n.idx: n for n in dfg.nodes}
  node_ids = [n.idx for n in dfg.nodes]

  # Build indivisible graph:
  # 1) recurrence edges (nextiter != 0)
  # 2) non-data control edges (type not I*) to avoid cutting predicate/control deps
  rec_edges: Dict[int, Set[int]] = {nid: set() for nid in node_ids}
  for n in dfg.nodes:
    for e in n.outputs:
      if e.dst_idx not in rec_edges:
        continue
      if e.nextiter != 0 or not _is_data_edge(e.edge_type):
        rec_edges[n.idx].add(e.dst_idx)

  comps = _tarjan_scc(node_ids, rec_edges)
  node_to_scc: Dict[int, int] = {}
  units: List[_SccUnit] = []
  for idx, comp in enumerate(comps):
    comp_nodes = [node_map[nid] for nid in comp]
    hist = _op_histogram(comp_nodes)
    min_asap = min(n.asap for n in comp_nodes) if comp_nodes else 0
    max_asap = max(n.asap for n in comp_nodes) if comp_nodes else 0
    units.append(_SccUnit(idx=idx, node_ids=list(comp), hist=hist, min_asap=min_asap, max_asap=max_asap))
    for nid in comp:
      node_to_scc[nid] = idx

  # Build forward DAG between SCCs and edge multiplicity map.
  edge_counts: Dict[Tuple[int, int], int] = {}
  for n in dfg.nodes:
    scc_src = node_to_scc[n.idx]
    for e in n.outputs:
      if e.nextiter != 0:
        continue
      if e.dst_idx not in node_to_scc:
        continue
      scc_dst = node_to_scc[e.dst_idx]
      if scc_dst == scc_src:
        continue
      units[scc_src].succs.add(scc_dst)
      units[scc_dst].preds.add(scc_src)
      key = (scc_src, scc_dst)
      edge_counts[key] = edge_counts.get(key, 0) + 1

  return units, node_to_scc, edge_counts


def _topological_scc_order(units: List[_SccUnit]) -> List[int]:
  indeg: Dict[int, int] = {u.idx: len(u.preds) for u in units}
  unit_map = {u.idx: u for u in units}
  ready = [u.idx for u in units if indeg[u.idx] == 0]
  ready.sort(key=lambda i: (unit_map[i].min_asap, unit_map[i].max_asap, i))

  order: List[int] = []
  while ready:
    curr = ready.pop(0)
    order.append(curr)
    for succ in sorted(unit_map[curr].succs):
      indeg[succ] -= 1
      if indeg[succ] == 0:
        ready.append(succ)
    ready.sort(key=lambda i: (unit_map[i].min_asap, unit_map[i].max_asap, i))

  if len(order) != len(units):
    # Should not happen for SCC DAG; keep deterministic fallback.
    remainder = [u.idx for u in units if u.idx not in set(order)]
    remainder.sort(key=lambda i: (unit_map[i].min_asap, unit_map[i].max_asap, i))
    order.extend(remainder)

  return order


def _estimate_candidate_delta(
  node_map: Dict[int, DFGNode],
  unit: _SccUnit,
  current_part_idx: int,
  assigned_part_by_node: Dict[int, int],
) -> Tuple[Set[int], Dict[int, Set[int]]]:
  """Return (incoming_src_nodes_for_current_part, additional_store_srcs_by_prev_part)."""
  incoming_src_nodes: Set[int] = set()
  add_store_srcs_by_prev: Dict[int, Set[int]] = {}
  unit_node_set = set(unit.node_ids)

  for dst_idx in unit.node_ids:
    dst_node = node_map.get(dst_idx)
    if dst_node is None:
      continue
    for in_e in dst_node.inputs:
      if in_e.nextiter != 0:
        continue
      src_idx = in_e.src_idx
      if src_idx in unit_node_set:
        continue
      src_part = assigned_part_by_node.get(src_idx)
      if src_part is None:
        continue
      if src_part == current_part_idx:
        continue
      incoming_src_nodes.add(src_idx)
      if src_part not in add_store_srcs_by_prev:
        add_store_srcs_by_prev[src_part] = set()
      add_store_srcs_by_prev[src_part].add(src_idx)

  return incoming_src_nodes, add_store_srcs_by_prev


def _assign_scc_partitions_greedy(
  dfg: DFGGraph,
  units: List[_SccUnit],
  edge_counts: Dict[Tuple[int, int], int],
  op_resources: Dict[str, int],
  target_size: int,
  min_target_size: int,
  balance_tolerance: float,
  max_loads_per_partition: int,
  max_stores_per_partition: int,
) -> List[List[DFGNode]]:
  """Greedy SCC-aware assignment with hard load/store budget checks."""
  unit_map = {u.idx: u for u in units}
  topo_order = _topological_scc_order(units)
  topo_pos = {scc_idx: i for i, scc_idx in enumerate(topo_order)}
  node_map = {n.idx: n for n in dfg.nodes}

  hard_cap = max(target_size + 5, int(target_size * (1.0 + balance_tolerance)))
  min_fill = min_target_size

  unassigned: Set[int] = set(unit_map.keys())
  assigned_scc: Set[int] = set()
  assigned_part_by_node: Dict[int, int] = {}
  outgoing_store_srcs_by_part: List[Set[int]] = []

  groups: List[List[DFGNode]] = []

  def _ready_sccs(local_curr: Set[int]) -> List[int]:
    ready: List[int] = []
    reachable = assigned_scc | local_curr
    for scc_idx in unassigned:
      if unit_map[scc_idx].preds.issubset(reachable):
        ready.append(scc_idx)
    ready.sort(key=lambda i: topo_pos.get(i, 10**9))
    return ready

  while unassigned:
    part_idx = len(groups)
    outgoing_store_srcs_by_part.append(set())
    curr_units: Set[int] = set()
    curr_nodes: List[DFGNode] = []
    curr_hist: Dict[str, int] = {}
    curr_incoming_srcs: Set[int] = set()

    while True:
      ready = _ready_sccs(curr_units)
      candidates: List[Tuple[int, int, int, _SccUnit, Set[int], Dict[int, Set[int]]]] = []

      for scc_idx in ready:
        unit = unit_map[scc_idx]

        # Do not exceed hard cap once the partition already has useful content.
        if curr_nodes and (len(curr_nodes) + unit.size) > hard_cap:
          continue

        merged_hist = _merge_hist(curr_hist, unit.hist)
        if compute_res_mii(merged_hist, op_resources) > 1:
          continue

        incoming_add, prev_store_add = _estimate_candidate_delta(
          node_map, unit, part_idx, assigned_part_by_node
        )

        if len(curr_incoming_srcs | incoming_add) > max_loads_per_partition:
          continue

        prev_ok = True
        for prev_part, add_srcs in prev_store_add.items():
          if prev_part < 0 or prev_part >= len(outgoing_store_srcs_by_part):
            prev_ok = False
            break
          if len(outgoing_store_srcs_by_part[prev_part] | add_srcs) > max_stores_per_partition:
            prev_ok = False
            break
        if not prev_ok:
          continue

        # Score to reduce cuts: absorb SCCs with high edge affinity to current partition.
        internal_gain = 0
        for c in curr_units:
          internal_gain += edge_counts.get((c, scc_idx), 0)
          internal_gain += edge_counts.get((scc_idx, c), 0)
        size_after = len(curr_nodes) + unit.size
        size_penalty = abs(size_after - target_size)
        incoming_penalty = len(incoming_add)
        score = internal_gain * 1000 - incoming_penalty * 25 - size_penalty

        candidates.append(
          (
            score,
            -unit.size,
            -topo_pos.get(scc_idx, 10**9),
            unit,
            incoming_add,
            prev_store_add,
          )
        )

      if not candidates:
        break

      candidates.sort(reverse=True, key=lambda x: (x[0], x[1], x[2], -x[3].idx))
      _, _, _, chosen, incoming_add, prev_store_add = candidates[0]

      curr_units.add(chosen.idx)
      curr_nodes.extend(node_map[nid] for nid in chosen.node_ids)
      curr_hist = _merge_hist(curr_hist, chosen.hist)
      curr_incoming_srcs |= incoming_add
      for prev_part, add_srcs in prev_store_add.items():
        outgoing_store_srcs_by_part[prev_part] |= add_srcs

      if len(curr_nodes) >= target_size and len(curr_nodes) >= min_fill:
        break

    if not curr_units:
      # Force progress with the earliest ready SCC and validate hard constraints.
      ready = _ready_sccs(set())
      if not ready:
        raise PartitioningError("No ready SCC found during partitioning; graph ordering is inconsistent.")
      forced = unit_map[ready[0]]
      forced_hist = dict(forced.hist)
      if compute_res_mii(forced_hist, op_resources) > 1:
        raise PartitioningError(
          "Cannot place SCC %d with %d nodes in II=1: compute/resource ResMII > 1 for a single SCC."
          % (forced.idx, forced.size)
        )
      curr_units.add(forced.idx)
      curr_nodes.extend(node_map[nid] for nid in forced.node_ids)

    # Commit partition
    groups.append(sorted(curr_nodes, key=lambda n: (n.asap, n.idx)))
    for scc_idx in curr_units:
      assigned_scc.add(scc_idx)
      unassigned.remove(scc_idx)
      for nid in unit_map[scc_idx].node_ids:
        assigned_part_by_node[nid] = part_idx

  return groups


def _segment_stats(
  dfg: DFGGraph,
  node_map: Dict[int, DFGNode],
  node_to_scc: Dict[int, int],
  topo_pos: Dict[int, int],
  ordered_units: List[_SccUnit],
  start: int,
  end: int,
) -> Tuple[Set[int], Set[int], int]:
  """Return (incoming_load_src_nodes, outgoing_store_src_nodes, crossing_edges)."""
  seg_node_ids: Set[int] = set()
  for pos in range(start, end + 1):
    seg_node_ids.update(ordered_units[pos].node_ids)

  incoming_srcs: Set[int] = set()
  outgoing_srcs: Set[int] = set()
  crossing_edges = 0

  for nid in seg_node_ids:
    node = node_map[nid]
    for in_e in node.inputs:
      if in_e.nextiter != 0:
        continue
      src_idx = in_e.src_idx
      scc = node_to_scc.get(src_idx)
      if scc is None:
        continue
      src_pos = topo_pos[scc]
      if src_pos < start:
        incoming_srcs.add(src_idx)

    for out_e in node.outputs:
      if out_e.nextiter != 0:
        continue
      dst_scc = node_to_scc.get(out_e.dst_idx)
      if dst_scc is None:
        continue
      dst_pos = topo_pos[dst_scc]
      if dst_pos > end:
        outgoing_srcs.add(nid)
        crossing_edges += 1

  return incoming_srcs, outgoing_srcs, crossing_edges


def _distribute_unknown_bank_loads(
  known_counts: Dict[int, int],
  unknown_count: int,
  bank_number: int,
) -> List[int]:
  loads = [known_counts.get(b, 0) for b in range(max(0, bank_number))]
  for _ in range(max(0, unknown_count)):
    best_bank = min(range(len(loads)), key=lambda b: (loads[b], b))
    loads[best_bank] += 1
  return loads


def _overflow_penalty(loads: List[int], per_bank_cap: int) -> int:
  cap = max(1, per_bank_cap)
  return sum(max(0, value - cap) for value in loads)


def _estimate_bank_feasibility_penalty(
  incoming_srcs: Set[int],
  outgoing_srcs: Set[int],
  node_map: Dict[int, DFGNode],
  max_loads_per_partition: int,
  max_stores_per_partition: int,
  total_size: int,
  bank_size: int,
  bank_number: int,
) -> int:
  """Estimate bank pressure for a partition; larger is less feasible."""
  if bank_number <= 1 or bank_size <= 0 or total_size <= 0:
    return 0

  incoming_known: Dict[int, int] = {}
  outgoing_known: Dict[int, int] = {}
  incoming_unknown = 0
  outgoing_unknown = 0

  for src_idx in incoming_srcs:
    src_node = node_map.get(src_idx)
    bank = (
      _preferred_bank_for_source_node(
        src_node,
        bank_size=bank_size,
        bank_number=bank_number,
        total_size=total_size,
      )
      if src_node is not None else None
    )
    if bank is None:
      incoming_unknown += 1
    else:
      incoming_known[bank] = incoming_known.get(bank, 0) + 1

  for src_idx in outgoing_srcs:
    src_node = node_map.get(src_idx)
    bank = (
      _preferred_bank_for_source_node(
        src_node,
        bank_size=bank_size,
        bank_number=bank_number,
        total_size=total_size,
      )
      if src_node is not None else None
    )
    if bank is None:
      outgoing_unknown += 1
    else:
      outgoing_known[bank] = outgoing_known.get(bank, 0) + 1

  load_bank_cap = (max(1, max_loads_per_partition) + bank_number - 1) // bank_number
  store_bank_cap = (max(1, max_stores_per_partition) + bank_number - 1) // bank_number

  incoming_loads = _distribute_unknown_bank_loads(
    known_counts=incoming_known,
    unknown_count=incoming_unknown,
    bank_number=bank_number,
  )
  outgoing_loads = _distribute_unknown_bank_loads(
    known_counts=outgoing_known,
    unknown_count=outgoing_unknown,
    bank_number=bank_number,
  )

  incoming_over = _overflow_penalty(incoming_loads, load_bank_cap)
  outgoing_over = _overflow_penalty(outgoing_loads, store_bank_cap)
  return incoming_over + outgoing_over


def _assign_scc_partitions_ordered_mincut(
  dfg: DFGGraph,
  units: List[_SccUnit],
  node_to_scc: Dict[int, int],
  op_resources: Dict[str, int],
  target_size: int,
  min_target_size: int,
  balance_tolerance: float,
  max_loads_per_partition: int,
  max_stores_per_partition: int,
  total_size: int,
  bank_size: int,
  bank_number: int,
) -> List[List[DFGNode]]:
  """Ordered min-cut style segmentation along SCC topological order."""
  topo_order = _topological_scc_order(units)
  if not topo_order:
    return []

  unit_map = {u.idx: u for u in units}
  ordered_units = [unit_map[scc_idx] for scc_idx in topo_order]
  topo_pos = {scc_idx: pos for pos, scc_idx in enumerate(topo_order)}
  node_map = {n.idx: n for n in dfg.nodes}

  hard_cap = max(target_size + 5, int(target_size * (1.0 + balance_tolerance)))
  n = len(ordered_units)
  inf = 10**18
  dp = [inf] * (n + 1)
  prev = [-1] * (n + 1)
  dp[0] = 0

  for i in range(n):
    if dp[i] >= inf:
      continue

    curr_hist: Dict[str, int] = {}
    curr_size = 0
    for j in range(i, n):
      unit = ordered_units[j]
      curr_size += unit.size
      curr_hist = _merge_hist(curr_hist, unit.hist)

      if curr_size > hard_cap:
        break
      if compute_res_mii(curr_hist, op_resources) > 1:
        break

      # Non-last partitions should have at least min_target_size nodes.
      if j < n - 1 and curr_size < min_target_size:
        continue

      incoming_srcs, outgoing_srcs, edge_cut = _segment_stats(
        dfg=dfg,
        node_map=node_map,
        node_to_scc=node_to_scc,
        topo_pos=topo_pos,
        ordered_units=ordered_units,
        start=i,
        end=j,
      )
      load_in = len(incoming_srcs)
      store_out = len(outgoing_srcs)

      if load_in > max_loads_per_partition or store_out > max_stores_per_partition:
        continue

      bank_penalty = _estimate_bank_feasibility_penalty(
        incoming_srcs=incoming_srcs,
        outgoing_srcs=outgoing_srcs,
        node_map=node_map,
        max_loads_per_partition=max_loads_per_partition,
        max_stores_per_partition=max_stores_per_partition,
        total_size=total_size,
        bank_size=bank_size,
        bank_number=bank_number,
      )

      # Weighted objective:
      # - strongly penalize inter-partition producer/consumer pairs
      # - softly penalize balance deviation
      # - lightly penalize raw crossing edges
      # - heavily penalize estimated per-bank pressure overflows
      cost = (
        dp[i]
        + (store_out + load_in) * 100
        + edge_cut * 2
        + abs(curr_size - target_size) * 5
        + bank_penalty * 250
      )
      if cost < dp[j + 1]:
        dp[j + 1] = cost
        prev[j + 1] = i

  if dp[n] >= inf:
    raise PartitioningError("ordered_mincut could not find a feasible segmentation.")

  # Reconstruct segments.
  segments: List[Tuple[int, int]] = []
  pos = n
  while pos > 0:
    i = prev[pos]
    if i < 0:
      raise PartitioningError("ordered_mincut reconstruction failed.")
    segments.append((i, pos - 1))
    pos = i
  segments.reverse()

  groups: List[List[DFGNode]] = []
  for i, j in segments:
    nodes: List[DFGNode] = []
    for p in range(i, j + 1):
      nodes.extend(node_map[nid] for nid in ordered_units[p].node_ids)
    groups.append(sorted(nodes, key=lambda n: (n.asap, n.idx)))

  return groups


def _assign_scc_partitions(
  dfg: DFGGraph,
  units: List[_SccUnit],
  node_to_scc: Dict[int, int],
  edge_counts: Dict[Tuple[int, int], int],
  op_resources: Dict[str, int],
  target_size: int,
  min_target_size: int,
  balance_tolerance: float,
  max_loads_per_partition: int,
  max_stores_per_partition: int,
  total_size: int,
  bank_size: int,
  bank_number: int,
) -> List[List[DFGNode]]:
  """Primary assignment path: ordered min-cut, with greedy fallback."""
  try:
    return _assign_scc_partitions_ordered_mincut(
      dfg=dfg,
      units=units,
      node_to_scc=node_to_scc,
      op_resources=op_resources,
      target_size=target_size,
      min_target_size=min_target_size,
      balance_tolerance=balance_tolerance,
      max_loads_per_partition=max_loads_per_partition,
      max_stores_per_partition=max_stores_per_partition,
      total_size=total_size,
      bank_size=bank_size,
      bank_number=bank_number,
    )
  except PartitioningError:
    return _assign_scc_partitions_greedy(
      dfg=dfg,
      units=units,
      edge_counts=edge_counts,
      op_resources=op_resources,
      target_size=target_size,
      min_target_size=min_target_size,
      balance_tolerance=balance_tolerance,
      max_loads_per_partition=max_loads_per_partition,
      max_stores_per_partition=max_stores_per_partition,
    )


def _preferred_bank_for_source_node(
  src_node: DFGNode,
  bank_size: int,
  bank_number: int,
  total_size: int,
) -> Optional[int]:
  if bank_size <= 0 or bank_number <= 0 or total_size <= 0:
    return None
  if not _is_mem_op(src_node.op):
    return None
  if src_node.offset < 0 or src_node.offset >= total_size:
    return None
  bank = src_node.offset // bank_size
  if bank < 0 or bank >= bank_number:
    return None
  return bank


def _alloc_scratch_addr(
  used_addrs: Set[int],
  element_size_bytes: int,
  total_size: int,
  preferred_bank: Optional[int],
  bank_size: int,
  bank_number: int,
) -> int:
  align = max(4, element_size_bytes)

  def _scan(start: int, end: int) -> Optional[int]:
    pos = ((start + align - 1) // align) * align
    while pos + element_size_bytes <= end:
      if pos not in used_addrs:
        return pos
      pos += align
    return None

  if preferred_bank is not None and bank_size > 0 and bank_number > 0:
    b_start = preferred_bank * bank_size
    b_end = min(total_size, (preferred_bank + 1) * bank_size)
    addr = _scan(b_start, b_end)
    if addr is not None:
      used_addrs.add(addr)
      return addr

  addr = _scan(0, total_size)
  if addr is None:
    raise PartitioningError("Out of scratch memory while assigning inter-partition STORE/LOAD addresses.")
  used_addrs.add(addr)
  return addr


def _apply_fanout_duplication(
  dfg: DFGGraph,
  assignment: Dict[int, int],
  part_dfgs: List[DFGGraph],
  part_node_maps: List[Dict[int, DFGNode]],
  cross_reqs: List[_CrossReq],
  op_resources: Dict[str, int],
) -> List[_CrossReq]:
  """Duplicate select high-fanout ops into destination partitions when it reduces IPC pairs."""
  node_map_global = {n.idx: n for n in dfg.nodes}
  part_hist = [g.op_histogram() for g in part_dfgs]

  changed = True
  while changed:
    changed = False
    grouped: Dict[Tuple[int, int], List[_CrossReq]] = {}
    src_to_dsts: Dict[int, Set[int]] = {}
    for req in cross_reqs:
      key = (req.src_idx, req.dst_part)
      grouped.setdefault(key, []).append(req)
      src_to_dsts.setdefault(req.src_idx, set()).add(req.dst_part)

    existing_pairs = {(r.src_idx, r.dst_part) for r in cross_reqs}

    # Try duplicating one (src, dst_part) per iteration; then recompute.
    for src_idx, dst_parts in sorted(src_to_dsts.items()):
      if len(dst_parts) < 2:
        continue

      src_node = node_map_global.get(src_idx)
      if src_node is None or not _is_dup_candidate_node(src_node):
        continue
      if any(e.nextiter != 0 for e in src_node.inputs):
        continue
      if any(e.nextiter != 0 for e in src_node.outputs):
        continue
      if any(not _is_data_edge(e.edge_type) for e in src_node.inputs):
        continue

      for dst_part in sorted(dst_parts):
        key = (src_idx, dst_part)
        reqs = grouped.get(key, [])
        if not reqs:
          continue
        if any(not _is_data_edge(r.edge_type) for r in reqs):
          continue

        # Check compute resource impact in destination partition.
        dst_hist = dict(part_hist[dst_part])
        dst_hist[src_node.op] = dst_hist.get(src_node.op, 0) + 1
        if compute_res_mii(dst_hist, op_resources) > 1:
          continue

        # Duplication benefit is removing one (src,dst_part) IPC pair.
        # Cost is adding parent->dst_part IPC pairs not already present.
        new_pairs: Set[Tuple[int, int]] = set()
        valid = True
        for in_e in src_node.inputs:
          parent_idx = in_e.src_idx
          parent_part = assignment.get(parent_idx)
          if parent_part is None:
            valid = False
            break
          if parent_part == dst_part:
            continue
          # Keep inter-partition deps forward-only.
          if parent_part >= dst_part:
            valid = False
            break
          pair = (parent_idx, dst_part)
          if pair not in existing_pairs:
            new_pairs.add(pair)

        gain = 1 - len(new_pairs)
        if not valid or gain <= 0:
          continue

        dst_g = part_dfgs[dst_part]
        dst_local_map = part_node_maps[dst_part]
        clone = dst_g.add_node(src_node.op, src_node.asap, src_node.alap, bb=src_node.bb)
        clone.const = src_node.const
        clone.shift = src_node.shift
        clone.offset = src_node.offset
        dst_local_map[clone.idx] = clone

        # Rewire consumer uses in dst partition to the cloned node.
        for req in reqs:
          dst_local_node = dst_local_map.get(req.dst_idx)
          if dst_local_node is None:
            valid = False
            break
          dup_out = DFGEdge(
            src_idx=clone.idx,
            dst_idx=dst_local_node.idx,
            nextiter=0,
            edge_type=req.edge_type,
            npb=req.npb,
          )
          clone.outputs.append(dup_out)
          dst_local_node.inputs.append(dup_out)
        if not valid:
          break

        # Feed cloned node from its original parents.
        for in_e in src_node.inputs:
          parent_idx = in_e.src_idx
          parent_part = assignment[parent_idx]
          if parent_part == dst_part:
            in_edge = DFGEdge(
              src_idx=parent_idx,
              dst_idx=clone.idx,
              nextiter=0,
              edge_type=in_e.edge_type,
              npb=in_e.npb,
            )
            clone.inputs.append(in_edge)
            parent_node = dst_local_map[parent_idx]
            parent_node.outputs.append(in_edge)
          else:
            cross_reqs.append(
              _CrossReq(
                src_idx=parent_idx,
                dst_part=dst_part,
                dst_idx=clone.idx,
                edge_type=in_e.edge_type,
                npb=in_e.npb,
              )
            )

        # Remove original cross uses of src->dst_part, now replaced by local clone.
        cross_reqs = [r for r in cross_reqs if not (r.src_idx == src_idx and r.dst_part == dst_part)]
        part_hist[dst_part] = dst_hist
        changed = True
        break

      if changed:
        break

  return cross_reqs


def _build_partitions(
  dfg: DFGGraph,
  node_groups: List[List[DFGNode]],
  op_resources: Dict[str, int],
  max_loads_per_partition: int,
  max_stores_per_partition: int,
  element_size_bytes: int,
  total_size: int,
  bank_size: int,
  bank_number: int,
  occupied_addrs: Optional[Set[int]] = None,
) -> List["PartitionInfo"]:
  """Build PartitionInfo list from final node assignment and inject STORE/LOAD pairs."""
  num_parts = len(node_groups)
  node_map_global = {n.idx: n for n in dfg.nodes}

  # Build assignment map: orig node_idx -> partition index
  assignment: Dict[int, int] = {}
  for p_idx, group in enumerate(node_groups):
    for n in group:
      assignment[n.idx] = p_idx

  # Collect cross-partition forward edge requirements.
  cross_reqs: List[_CrossReq] = []
  for node in dfg.nodes:
    src_part = assignment.get(node.idx)
    if src_part is None:
      continue
    for e in node.outputs:
      if e.nextiter != 0:
        continue
      dst_part = assignment.get(e.dst_idx)
      if dst_part is None or dst_part <= src_part:
        continue
      cross_reqs.append(
        _CrossReq(
          src_idx=node.idx,
          dst_part=dst_part,
          dst_idx=e.dst_idx,
          edge_type=e.edge_type,
          npb=e.npb,
        )
      )

  # Build per-partition DFGGraph objects
  part_dfgs: List[DFGGraph] = []
  part_node_maps: List[Dict[int, DFGNode]] = []
  cross_in_count = [0 for _ in range(num_parts)]
  cross_out_count = [0 for _ in range(num_parts)]
  load_count = [0 for _ in range(num_parts)]
  store_count = [0 for _ in range(num_parts)]
  incoming_sources: List[Set[int]] = [set() for _ in range(num_parts)]
  outgoing_store_sources: List[Set[int]] = [set() for _ in range(num_parts)]

  for p_nodes in node_groups:
    g = DFGGraph()
    g.iter_stride = dfg.iter_stride
    g.iter_max_count = dfg.iter_max_count
    g.mutex_bbs = dfg.mutex_bbs

    old_idx_set = {n.idx for n in p_nodes}
    local_map: Dict[int, DFGNode] = {}

    for orig_node in p_nodes:
      new_node = DFGNode(
        idx=orig_node.idx,
        asap=orig_node.asap,
        alap=orig_node.alap,
        bb=orig_node.bb,
        const=orig_node.const,
        shift=orig_node.shift,
        offset=orig_node.offset,
        op=orig_node.op,
        rec_parents=list(orig_node.rec_parents),
      )
      g.nodes.append(new_node)
      local_map[orig_node.idx] = new_node

    for orig_node in p_nodes:
      for e in orig_node.outputs:
        if e.dst_idx in old_idx_set:
          new_edge = DFGEdge(
            src_idx=orig_node.idx,
            dst_idx=e.dst_idx,
            nextiter=e.nextiter,
            edge_type=e.edge_type,
            npb=e.npb,
          )
          local_map[orig_node.idx].outputs.append(new_edge)
          local_map[e.dst_idx].inputs.append(new_edge)

    g._next_idx = max((n.idx for n in g.nodes), default=-1) + 1
    part_dfgs.append(g)
    part_node_maps.append(local_map)

  cross_reqs = _apply_fanout_duplication(
    dfg=dfg,
    assignment=assignment,
    part_dfgs=part_dfgs,
    part_node_maps=part_node_maps,
    cross_reqs=cross_reqs,
    op_resources=op_resources,
  )

  # Inject STORE/LOAD pairs for cross-partition edges
  part_mem_alloc: List[Dict[str, int]] = [{} for _ in range(num_parts)]
  store_cache_by_src: Dict[int, Tuple[int, str]] = {}
  load_cache_by_src_dst: Dict[Tuple[int, int], DFGNode] = {}
  used_addrs: Set[int] = set(occupied_addrs or set())
  cross_reqs.sort(key=lambda r: (r.src_idx, r.dst_part, r.dst_idx, r.edge_type, r.npb))

  for req in cross_reqs:
    src_part = assignment.get(req.src_idx)
    if src_part is None:
      continue
    dst_part = req.dst_part
    if dst_part < 0 or dst_part >= num_parts:
      continue
    src_g = part_dfgs[src_part]
    dst_g = part_dfgs[dst_part]
    src_local_map = part_node_maps[src_part]
    dst_local_map = part_node_maps[dst_part]
    src_node = node_map_global.get(req.src_idx)
    if src_node is None:
      continue
    dst_node = dst_local_map.get(req.dst_idx)
    if dst_node is None:
      continue
    cross_out_count[src_part] += 1
    cross_in_count[dst_part] += 1

    src_local_node = src_local_map.get(src_node.idx)
    if src_local_node is None:
      continue

    store_entry = store_cache_by_src.get(req.src_idx)
    if store_entry is None:
      preferred_bank = _preferred_bank_for_source_node(
        src_node, bank_size=bank_size, bank_number=bank_number, total_size=total_size
      )
      scratch_addr = _alloc_scratch_addr(
        used_addrs=used_addrs,
        element_size_bytes=element_size_bytes,
        total_size=total_size,
        preferred_bank=preferred_bank,
        bank_size=bank_size,
        bank_number=bank_number,
      )
      var_name = "__ipc_src_p%d_n%d" % (src_part, src_node.idx)

      store_node = src_g.add_node("STORE", src_local_node.asap + 1, src_local_node.asap + 1)
      store_edge = DFGEdge(
        src_idx=src_local_node.idx, dst_idx=store_node.idx, nextiter=0, edge_type="I1", npb=0
      )
      src_local_node.outputs.append(store_edge)
      store_node.inputs.append(store_edge)
      store_count[src_part] += 1
      outgoing_store_sources[src_part].add(src_local_node.idx)

      part_mem_alloc[src_part][var_name] = scratch_addr
      store_cache_by_src[req.src_idx] = (scratch_addr, var_name)
      store_entry = (scratch_addr, var_name)

    scratch_addr, var_name = store_entry

    load_key = (req.src_idx, dst_part)
    load_node = load_cache_by_src_dst.get(load_key)
    if load_node is None:
      load_node = dst_g.add_node("LOAD", 0, 0)
      dst_local_map[load_node.idx] = load_node
      load_count[dst_part] += 1
      incoming_sources[dst_part].add(req.src_idx)
      part_mem_alloc[dst_part][var_name] = scratch_addr
      load_cache_by_src_dst[load_key] = load_node

    dst_local_node = dst_local_map[dst_node.idx]
    dst_local_node.inputs = [
      e for e in dst_local_node.inputs if not (e.src_idx == req.src_idx and e.nextiter == 0)
    ]
    new_edge = DFGEdge(
      src_idx=load_node.idx,
      dst_idx=dst_local_node.idx,
      nextiter=0,
      edge_type=req.edge_type,
      npb=req.npb,
    )
    load_node.outputs.append(new_edge)
    dst_local_node.inputs.append(new_edge)

  # Recompute ASAP and validate per-partition feasibility
  results: List[PartitionInfo] = []
  for p_idx, g in enumerate(part_dfgs):
    g.recompute_asap()
    hist = g.op_histogram()
    res_mii = compute_res_mii(hist, op_resources)
    rec_mii = compute_rec_mii(g)

    if res_mii > 1:
      raise PartitioningError(
        "Partition %d violates II=1 resource bound after IPC injection: resMII=%d."
        % (p_idx, res_mii)
      )
    if rec_mii > 1:
      raise PartitioningError(
        "Partition %d violates II=1 recurrence bound after IPC injection: recMII=%d."
        % (p_idx, rec_mii)
      )
    if len(incoming_sources[p_idx]) > max_loads_per_partition:
      raise PartitioningError(
        "Partition %d exceeds LOAD budget (%d > %d)."
        % (p_idx, len(incoming_sources[p_idx]), max_loads_per_partition)
      )
    if len(outgoing_store_sources[p_idx]) > max_stores_per_partition:
      raise PartitioningError(
        "Partition %d exceeds STORE budget (%d > %d)."
        % (p_idx, len(outgoing_store_sources[p_idx]), max_stores_per_partition)
      )

    results.append(
      PartitionInfo(
        idx=p_idx,
        dfg=g,
        mem_alloc_additions=part_mem_alloc[p_idx],
        res_mii=res_mii,
        rec_mii=rec_mii,
        cross_edges_in=cross_in_count[p_idx],
        cross_edges_out=cross_out_count[p_idx],
        injected_loads=load_count[p_idx],
        injected_stores=store_count[p_idx],
        incoming_load_count=len(incoming_sources[p_idx]),
        outgoing_store_count=len(outgoing_store_sources[p_idx]),
      )
    )

  return results


def partition_dfg_ii1_recursive(
  dfg: DFGGraph,
  op_resources: Dict[str, int],
  target_size: int = 10,
  min_target_size: int = 5,
  balance_tolerance: float = 0.35,
  max_loads_per_partition: int = 4,
  max_stores_per_partition: int = 4,
  element_size_bytes: int = 4,
  total_size: int = 0,
  bank_size: int = 0,
  bank_number: int = 0,
  occupied_addrs: Optional[Set[int]] = None,
) -> List[PartitionInfo]:
  """Partition DFG into II=1-feasible sub-DFGs with hard load/store budgets.

  If a target size is infeasible after IPC insertion, retries with target_size//2
  down to min_target_size.
  """
  if target_size <= 0:
    raise PartitioningError("target_size must be > 0.")
  if min_target_size <= 0:
    raise PartitioningError("min_target_size must be > 0.")
  if min_target_size > target_size:
    min_target_size = target_size

  if total_size <= 0:
    # Keep a safe default for XML-only static experiments.
    total_size = 1 << 30

  units, node_to_scc, edge_counts = _build_scc_units(dfg)
  curr_target = target_size
  last_err = None
  tried: Set[int] = set()

  while True:
    tried.add(curr_target)
    node_groups = _assign_scc_partitions(
      dfg=dfg,
      units=units,
      node_to_scc=node_to_scc,
      edge_counts=edge_counts,
      op_resources=op_resources,
      target_size=curr_target,
      min_target_size=min_target_size,
      balance_tolerance=balance_tolerance,
      max_loads_per_partition=max_loads_per_partition,
      max_stores_per_partition=max_stores_per_partition,
      total_size=total_size,
      bank_size=bank_size,
      bank_number=bank_number,
    )

    try:
      return _build_partitions(
        dfg=dfg,
        node_groups=node_groups,
        op_resources=op_resources,
        max_loads_per_partition=max_loads_per_partition,
        max_stores_per_partition=max_stores_per_partition,
        element_size_bytes=element_size_bytes,
        total_size=total_size,
        bank_size=bank_size,
        bank_number=bank_number,
        occupied_addrs=occupied_addrs,
      )
    except PartitioningError as exc:
      last_err = exc
      if curr_target <= min_target_size:
        break
      next_target = max(min_target_size, curr_target // 2)
      if next_target in tried or next_target == curr_target:
        break
      curr_target = next_target

  raise PartitioningError(
    "Unable to find feasible partitioning; tried target sizes %s. Last error: %s"
    % (sorted(tried), str(last_err) if last_err else "unknown")
  )
