"""Parameterized systolic DFG templates.

Each template is a function that returns a complete DFG XML string given the
PE-array dimensions, the steady-state iteration count (`max_count`), the
compute op identity (`op_kind`), and an optional `useful_cells` count (for
kernels that don't fill the full W*H grid; the remaining cells are emitted as
isolated NOP nodes).

The XML format mirrors `Morpher_DFG_Generator/benchmarks/xml_inputs/2gemm_systolic.xml`
(MutexBB + ITER + DFG block with Node/Inputs/Outputs/RecParents).

Honest note: these templates are structurally representative — they capture
the dataflow shape that a systolic mapping would use. They are NOT functionally
equivalent to any specific source kernel. Per user direction, op identity is
flexible; what matters is the network topology. See REFERENCE.md.
"""

from __future__ import annotations

from typing import Iterable, List, Tuple


# ---------------------------------------------------------------------------
# Low-level XML node builder
# ---------------------------------------------------------------------------


def _node_xml(idx: int, op: str, asap: int, alap: int,
              inputs: Iterable[int],
              outputs: Iterable[Tuple[int, str]],
              extra_outputs: Iterable[Tuple[int, str, int]] = (),
              rec_parents: Iterable[Tuple[int, str]] = (),
              base_name: str = "",
              base_size: int = 256,
              simd_lanes: int = 1) -> str:
  """Emit one <Node> block.

  inputs:  iterable of parent idx (Input).
  outputs: iterable of (dst_idx, port_type) for current-iter outputs.
  extra_outputs: iterable of (dst_idx, port_type, nextiter) for non-default
                 outputs — used for self-Output edges with nextiter=1 that
                 the mapper's minII calc detects as recurrences.
  rec_parents: iterable of (parent_idx, port_type) for <RecParent> entries.
               Used for self-recurrence (parent_idx == idx) so the mapper
               knows the value flows back to this node's input via TREG.
  simd_lanes: when >1, emit `simd_packed="true" simd_lanes=N` attributes on
              the <Node>. Caller is responsible for using V-prefix ops
              (VLOAD/VMAC/VADD/VSTORE) so the arch's vector FU is selected.
  """
  attrs = 'idx="%d" ASAP="%d" ALAP="%d" BB="0" CONST="0" SHIFT="0" OFFSET="0"' % (
      idx, asap, alap)
  if simd_lanes > 1:
    attrs += ' simd_packed="true" simd_lanes="%d"' % simd_lanes
  parts = [
      '  <Node %s>' % attrs,
      "    <OP>%s</OP>" % op,
  ]
  ins = list(inputs)
  if ins:
    parts.append("    <Inputs>")
    for i in ins:
      parts.append('      <Input idx="%d"/>' % i)
    parts.append("    </Inputs>")
  else:
    parts.append("    <Inputs/>")
  outs = list(outputs)
  extras = list(extra_outputs)
  if outs or extras:
    parts.append("    <Outputs>")
    for dst, t in outs:
      parts.append('      <Output idx="%d" nextiter="0" type="%s"/>' % (dst, t))
    for dst, t, ni in extras:
      parts.append('      <Output idx="%d" nextiter="%d" type="%s"/>' % (dst, ni, t))
    parts.append("    </Outputs>")
  else:
    parts.append("    <Outputs/>")
  recs = list(rec_parents)
  if recs:
    parts.append("    <RecParents>")
    for ridx, rtype in recs:
      parts.append('      <RecParent idx="%d" type="%s"/>' % (ridx, rtype))
    parts.append("    </RecParents>")
  else:
    parts.append("    <RecParents/>")
  # BasePointerName: required by the artifact_simulator to link LOAD/STORE
  # nodes to their mem_alloc base entry (otherwise the comparison loop's
  # isStoreCovered() filter skips all expected addresses). The mapper's
  # DFG parser also REQUIRES a `size` attribute (asserts `size != -1`).
  if base_name:
    parts.append('    <BasePointerName size="%d">%s</BasePointerName>'
                 % (base_size, base_name))
  parts.append("  </Node>")
  return "\n".join(parts)


def _wrap_dfg(nodes_xml: List[str], iter_max: int) -> str:
  body = "\n".join(nodes_xml)
  return (
      "<MutexBB>\n</MutexBB>\n"
      '<ITER stride="1" max_count="%d">\n</ITER>\n' % iter_max
      + '<DFG count="%d">\n%s\n</DFG>\n' % (len(nodes_xml), body)
  )


# SIMD op mapping: when the target arch is vec*, the scalar op identities
# our templates emit (LOAD/MAC/ADD/STORE/NOP) must be V-prefixed so the
# mapper picks the vector FU. NOP stays scalar (the dummy NOP FU only
# accepts scalar NOP in our vec arches).
_SCALAR_TO_VECTOR_OP = {
    "LOAD":  "VLOAD",
    "STORE": "VSTORE",
    "ADD":   "VADD",
    "MUL":   "VMUL",
    "MAC":   "VMAC",
    # NOP intentionally omitted — keep scalar.
}


def _v_op(op: str, simd_lanes: int) -> str:
  """Return the V-prefixed equivalent of `op` when simd_lanes > 1, else the
  original op. NOP stays NOP because the FU_DUMMY OPS block in vec arches
  only declares scalar NOP.
  """
  if simd_lanes <= 1:
    return op
  return _SCALAR_TO_VECTOR_OP.get(op, op)


# ---------------------------------------------------------------------------
# Template: gemm_fanout (mirrors 2gemm_systolic.xml)
# ---------------------------------------------------------------------------
#
#   W LOADs (one per "row" of the systolic array)
#   W*H compute grid (op_kind, typically ADD or MAC)
#   W STOREs at the bottom
#
#   Each LOAD r fans out to all W compute nodes in row r (I2 input).
#   Each compute node (r, c) for r>=1 also receives from compute (r-1, c) (I1).
#   The bottom row feeds STORE c.
# ---------------------------------------------------------------------------


def gemm_fanout(dim_x: int, dim_y: int, iter_max: int,
                op_kind: str = "ADD", useful_cells: int | None = None,
                simd_lanes: int = 1) -> str:
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  # idx layout:
  #   LOAD_r       at idx = r * (W + 1)
  #   COMPUTE_r_c  at idx = r * (W + 1) + 1 + c
  # so row r occupies idxs r*(W+1) .. r*(W+1)+W. STOREs come after the grid.
  def load_idx(r: int) -> int:
    return r * (W + 1)

  def cell_idx(r: int, c: int) -> int:
    return r * (W + 1) + 1 + c

  store_base = H * (W + 1)

  # First emit LOADs and compute cells row-by-row (interleaved to match the
  # template's idx order in 2gemm_systolic.xml).
  cells_emitted = 0
  for r in range(H):
    # LOAD r fans out to all useful cells in row r. Output type: row-0 cells
    # are single-input passthroughs (op=ADD), so LOAD's output to them must
    # be type "I1" — otherwise the simulator's edge-resolution overrides the
    # cell's input to type "I2", which makes firstDataInput and
    # inputByType("I2") return the same value, causing ADD to compute 2*v
    # instead of v. Row-1+ cells take a second input from the previous row
    # cell (type "I1"); LOAD is then the "I2" operand of the MAC.
    load_type = "I1" if r == 0 else "I2"
    load_out: List[Tuple[int, str]] = []
    for c in range(W):
      gi = r * W + c
      if gi < useful_cells:
        load_out.append((cell_idx(r, c), load_type))
    nodes.append(_node_xml(load_idx(r), _v_op("LOAD", simd_lanes),
                           asap=0, alap=r,
                           inputs=[], outputs=load_out,
                           simd_lanes=simd_lanes))

    # Compute cells in row r.
    for c in range(W):
      gi = r * W + c
      cur_idx = cell_idx(r, c)
      if gi >= useful_cells:
        # NOP filler: isolated node, no inputs, no outputs. NOP stays scalar
        # because the dummy/NOP FU in vec arches only declares scalar NOP.
        nodes.append(_node_xml(cur_idx, "NOP", asap=r, alap=r,
                               inputs=[], outputs=[]))
        continue
      # Inputs: from LOAD r (always); from cell (r-1, c) if r >= 1.
      ins: List[int] = [load_idx(r)]
      if r >= 1 and (r - 1) * W + c < useful_cells:
        ins.append(cell_idx(r - 1, c))
      # Outputs: to cell (r+1, c) if r+1 < H AND that next cell is useful;
      #          else to STORE c if at bottom of useful column.
      outs: List[Tuple[int, str]] = []
      next_r = r + 1
      next_useful = next_r < H and next_r * W + c < useful_cells
      if next_useful:
        outs.append((cell_idx(next_r, c), "I1"))
      else:
        outs.append((store_base + c, "I1"))
      # Cell op: MAC needs two operands (a, b) + the implicit accumulator
      # from TREG (a hardware-level contract; no DFG self-edge — the
      # mapper's recurrence routing hits assertions for intra-PE self-
      # loops). Row-0 cells only have one input (the LOAD) — emit them
      # as ADD which the simulator treats as identity for single-input
      # nodes.
      cell_op = op_kind if len(ins) >= 2 else "ADD"
      extras: List[Tuple[int, str, int]] = []
      recs: List[Tuple[int, str]] = []
      nodes.append(_node_xml(cur_idx, _v_op(cell_op, simd_lanes),
                             asap=r + 1, alap=r + 1,
                             inputs=ins, outputs=outs,
                             extra_outputs=extras, rec_parents=recs,
                             simd_lanes=simd_lanes))
      cells_emitted += 1

  # STOREs (one per column).
  for c in range(W):
    # STORE consumes from the deepest useful compute cell in column c.
    deepest = None
    for r in range(H - 1, -1, -1):
      if r * W + c < useful_cells:
        deepest = r
        break
    ins = [cell_idx(deepest, c)] if deepest is not None else []
    nodes.append(_node_xml(store_base + c, _v_op("STORE", simd_lanes),
                           asap=H, alap=H,
                           inputs=ins, outputs=[],
                           simd_lanes=simd_lanes))

  return _wrap_dfg(nodes, iter_max)


# ---------------------------------------------------------------------------
# Template: gemm_chain (mirrors gemm_systolic_1.xml)
# ---------------------------------------------------------------------------
#   Linear chain: each LOAD passes through a chain of W compute nodes per row,
#   then forwards diagonally to the next row.
# ---------------------------------------------------------------------------


def gemm_chain(dim_x: int, dim_y: int, iter_max: int,
               op_kind: str = "ADD", useful_cells: int | None = None,
               simd_lanes: int = 1) -> str:
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []

  def load_idx(r: int) -> int:
    return r * (W + 1)

  def cell_idx(r: int, c: int) -> int:
    return r * (W + 1) + 1 + c

  store_base = H * (W + 1)

  for r in range(H):
    # LOAD feeds only the leftmost useful cell in row r.
    # Type "I1" so it's distinguishable from the vertical-down "I2" edge
    # arriving at the same consumer cell (r, 0) when r>=1. For r=0, c=0
    # the cell is single-input (passthrough ADD); "I1" lets firstDataInput
    # return LOAD's value while inputByType("I2") returns invalid → no
    # double-counting bug.
    load_outs: List[Tuple[int, str]] = []
    if 0 < useful_cells - r * W:  # at least the first cell in this row is useful
      load_outs.append((cell_idx(r, 0), "I1"))
    nodes.append(_node_xml(load_idx(r), _v_op("LOAD", simd_lanes),
                           asap=0, alap=r,
                           inputs=[], outputs=load_outs,
                           simd_lanes=simd_lanes))

    for c in range(W):
      gi = r * W + c
      cur_idx = cell_idx(r, c)
      if gi >= useful_cells:
        nodes.append(_node_xml(cur_idx, "NOP", asap=r, alap=r,
                               inputs=[], outputs=[]))
        continue

      ins: List[int] = []
      if c == 0:
        ins.append(load_idx(r))
      else:
        ins.append(cell_idx(r, c - 1))
      if r >= 1 and (r - 1) * W + c < useful_cells:
        ins.append(cell_idx(r - 1, c))

      outs: List[Tuple[int, str]] = []
      next_c = c + 1
      if next_c < W and r * W + next_c < useful_cells:
        outs.append((cell_idx(r, next_c), "I1"))
      next_r = r + 1
      if next_r < H and next_r * W + c < useful_cells:
        outs.append((cell_idx(next_r, c), "I2"))
      # Bottom-row useful cells additionally feed their column's STORE so the
      # XML remains internally consistent (every STORE input has a matching
      # cell Output edge).
      bottom_useful = (r == H - 1) or (
          r + 1 < H and (r + 1) * W + c >= useful_cells)
      if bottom_useful:
        outs.append((store_base + c, "I1"))

      # Note: MAC's implicit accumulator is a *hardware-level* contract.
      # We do NOT emit a DFG self-edge here — the mapper's recurrence
      # routing through TREG hits two separate assertions
      # (checkRecParentViolation expects RecParent.rootDP != NULL;
      # assignPath expects the first routing-port to be named "T"). The
      # MAC FU's internal register is invisible to the mapper, similar to
      # how a real silicon MAC accumulates `a*b + acc` with acc kept
      # inside the PE between cycles. Documented in REFERENCE.md.
      cell_op = op_kind if len(ins) >= 2 else "ADD"
      extras: List[Tuple[int, str, int]] = []
      recs: List[Tuple[int, str]] = []
      nodes.append(_node_xml(cur_idx, _v_op(cell_op, simd_lanes),
                             asap=r + c + 1,
                             alap=r + c + 1, inputs=ins, outputs=outs,
                             extra_outputs=extras, rec_parents=recs,
                             simd_lanes=simd_lanes))

  for c in range(W):
    deepest = None
    for r in range(H - 1, -1, -1):
      if r * W + c < useful_cells:
        deepest = r
        break
    ins = [cell_idx(deepest, c)] if deepest is not None else []
    nodes.append(_node_xml(store_base + c, _v_op("STORE", simd_lanes),
                           asap=H + W, alap=H + W,
                           inputs=ins, outputs=[],
                           simd_lanes=simd_lanes))

  return _wrap_dfg(nodes, iter_max)


# ---------------------------------------------------------------------------
# Template: matvec_row (1D systolic line)
# ---------------------------------------------------------------------------
#   For 1-parallel/1-reduction kernels. One row of compute nodes.
#   dim_x compute cells, 1 LOAD per cell, 1 STORE at end.
# ---------------------------------------------------------------------------


def matvec_row(dim_x: int, dim_y: int, iter_max: int,
               op_kind: str = "ADD", useful_cells: int | None = None,
               simd_lanes: int = 1) -> str:
  # dim_y unused; we synthesize a single row of dim_x cells.
  _ = dim_y
  W = dim_x
  if useful_cells is None:
    useful_cells = W
  useful_cells = max(0, min(useful_cells, W))

  nodes: List[str] = []
  # Layout:
  #   LOAD c       at idx = 2*c
  #   COMPUTE c    at idx = 2*c + 1
  #   STORE        at idx = 2*W
  for c in range(W):
    li = 2 * c
    ci = 2 * c + 1
    if c >= useful_cells:
      nodes.append(_node_xml(li, "NOP", 0, 0, [], []))
      nodes.append(_node_xml(ci, "NOP", 0, 0, [], []))
      continue
    # LOAD output type: cell 0 is single-input (passthrough ADD) so LOAD
    # must use "I1" to avoid the simulator's edge-resolution double-counting
    # bug (same root cause as gemm_fanout row-0 fix). Cells c>=1 have a
    # second input from prev cell (type "I1"); LOAD becomes the "I2"
    # operand of the MAC.
    load_type = "I1" if c == 0 else "I2"
    nodes.append(_node_xml(li, _v_op("LOAD", simd_lanes), 0, c,
                           [], [(ci, load_type)],
                           simd_lanes=simd_lanes))
    ins = [li]
    if c >= 1:
      ins.append(2 * (c - 1) + 1)
    outs: List[Tuple[int, str]] = []
    if c + 1 < useful_cells:
      outs.append((2 * (c + 1) + 1, "I1"))
    else:
      outs.append((2 * W, "I1"))
    cell_op = op_kind if len(ins) >= 2 else "ADD"
    extras: List[Tuple[int, str, int]] = []
    recs: List[Tuple[int, str]] = []
    nodes.append(_node_xml(ci, _v_op(cell_op, simd_lanes),
                           c + 1, c + 1, ins, outs,
                           extra_outputs=extras, rec_parents=recs,
                           simd_lanes=simd_lanes))

  store_in = (2 * (useful_cells - 1) + 1) if useful_cells >= 1 else None
  nodes.append(_node_xml(2 * W, _v_op("STORE", simd_lanes), W + 1, W + 1,
                         [store_in] if store_in is not None else [], [],
                         simd_lanes=simd_lanes))
  return _wrap_dfg(nodes, iter_max)


# ---------------------------------------------------------------------------
# Template: matvec_parallel (1D-output kernel tiled across the full grid)
# ---------------------------------------------------------------------------
#
# Represents the strategy of computing multiple matvec outputs (or one matvec
# output across a tiled reduction) in parallel so a 1D kernel can fill a 2D
# array. Topologically uses the same gemm_fanout shape: W LOADs along the
# top, W*H compute cells, W STOREs at the bottom. The user-facing difference
# from gemm_fanout is intent: this template is the recommended target for
# kernels with 1 parallel output dim if you want the full grid utilized.
#
# Honest note: the synthesized DFG is structural-only (op identity is
# flexible). To make this functionally correspond to a real matvec, the
# scheduler would need to broadcast a single x-vector across rows and
# distribute A across the array — a lowering choice this script does not
# make.
# ---------------------------------------------------------------------------


def matvec_parallel(dim_x: int, dim_y: int, iter_max: int,
                    op_kind: str = "ADD",
                    useful_cells: int | None = None,
                    simd_lanes: int = 1) -> str:
  return gemm_fanout(dim_x, dim_y, iter_max, op_kind=op_kind,
                     useful_cells=useful_cells, simd_lanes=simd_lanes)


def gemm_fanout_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


def gemm_chain_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


def matvec_row_cells(dim_x: int, dim_y: int) -> int:
  # Only one row is used by the 1D matvec systolic line.
  _ = dim_y
  return dim_x


def matvec_parallel_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


TEMPLATES = {
    "gemm_fanout": gemm_fanout,
    "gemm_chain": gemm_chain,
    "matvec_row": matvec_row,
    "matvec_parallel": matvec_parallel,
}

# Per-template "useful compute cells when useful_cells=None (full template)".
# This is what the template emits as actual MAC/ADD nodes on the dim_x*dim_y
# compute grid. matvec_row only uses a single row → dim_x cells out of
# dim_x*dim_y. The others fill the full grid.
TEMPLATE_DEFAULT_CELLS = {
    "gemm_fanout": gemm_fanout_cells,
    "gemm_chain": gemm_chain_cells,
    "matvec_row": matvec_row_cells,
    "matvec_parallel": matvec_parallel_cells,
}


def pick_template(loop_classes: list, mac_pairs: int, stencil_pairs: int) -> str | None:
  """Heuristic: pick a MAC-shape template, or None if the kernel is not
  systolic-suitable (e.g. stencils, no-reduction adds-only kernels).

  Systolic templates here are strictly MAC-style — they model
  inner-product-style accumulation. Stencil-only kernels (no MAC, only
  load+load adds) don't belong on a MAC-style systolic array and are
  skipped; the orchestrator reports them as not_synthesizable.
  """
  n_parallel = sum(1 for c in loop_classes if c.get("kind") == "PARALLEL")
  n_reduction = sum(1 for c in loop_classes if c.get("kind") == "REDUCTION")
  if mac_pairs == 0:
    return None  # stencil-only or non-systolic body
  if n_parallel >= 2 and n_reduction >= 1:
    return "gemm_fanout"
  if n_parallel == 1 and n_reduction >= 1:
    # Default to matvec_row (honest 1-row utilization). Use the --template
    # override or pick_template_aggressive for matvec_parallel (full grid).
    return "matvec_row"
  if n_parallel >= 2 and n_reduction == 0:
    # multi-store MAC kernels like bicg: use the chain layout
    return "gemm_chain"
  return None
