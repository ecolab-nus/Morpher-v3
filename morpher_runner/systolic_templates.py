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
              simd_lanes: int = 1,
              const_value: int = 0,
              gep_offset: int | None = None) -> str:
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
  attrs = 'idx="%d" ASAP="%d" ALAP="%d" BB="0" CONST="%d" SHIFT="0" OFFSET="0"' % (
      idx, asap, alap, const_value)
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
  if gep_offset is not None:
    parts.append('    <GEPOffset>%d</GEPOffset>' % gep_offset)
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
# Template: gemm_output_stationary
# ---------------------------------------------------------------------------
#
# True output-stationary GEMM systolic mapping:
#
#   - Per cycle, every cell (r, c) accumulates A[r,k] * B[k,c] into its TREG.
#   - A is broadcast across columns: H A-LOADs per cycle (one per row, fanning
#     out to all W cells in that row, type I1).
#   - B is broadcast across rows: W B-LOADs per cycle (one per column, fanning
#     out to all H cells in that column, type I2).
#   - W*H MAC cells, each with I1=A[r], I2=B[c], TREG=accumulator. No stores
#     in the main DFG: the output is drained by a separate epilogue.
#
# Per-iter input count: H + W loads (vs gemm_fanout's H). Output drain is
# deferred to gemm_output_stationary_epilogue, which has W*H one-shot stores.
#
# Simulator caveat: the artifact simulator's MAC accumulates correctly via
# `previousValues_[mac.idx]`, but with no stores the accumulator never reaches
# the `MemoryImage`. An epilogue cannot read the previous DFG's per-node
# accumulator (cross-DFG state is not propagated). For end-to-end functional
# validation today you still need a per-iter STORE (see gemm_fanout). For
# hardware artifact emission and mapping/util numbers, this template is the
# right shape.
# ---------------------------------------------------------------------------


def gemm_output_stationary(dim_x: int, dim_y: int, iter_max: int,
                            op_kind: str = "MAC",
                            useful_cells: int | None = None,
                            simd_lanes: int = 1) -> str:
  """Output-stationary main DFG. `op_kind` should be MAC; ADD is allowed for
  templates that need a non-multiply accumulator. No per-iter stores."""
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []

  # idx layout: a-loads [0..H-1], b-loads [H..H+W-1], cells [H+W..H+W+W*H-1]
  def a_load_idx(r: int) -> int:
    return r

  def b_load_idx(c: int) -> int:
    return H + c

  def cell_idx(r: int, c: int) -> int:
    return H + W + r * W + c

  # A-LOADs (one per row, broadcast across columns).
  for r in range(H):
    outs: List[Tuple[int, str]] = []
    for c in range(W):
      gi = r * W + c
      if gi < useful_cells:
        outs.append((cell_idx(r, c), "I1"))
    nodes.append(_node_xml(a_load_idx(r), _v_op("LOAD", simd_lanes),
                           asap=r, alap=r,
                           inputs=[], outputs=outs,
                           simd_lanes=simd_lanes,
                           base_name="var_a"))

  # B-LOADs (one per column, broadcast across rows).
  for c in range(W):
    outs2: List[Tuple[int, str]] = []
    for r in range(H):
      gi = r * W + c
      if gi < useful_cells:
        outs2.append((cell_idx(r, c), "I2"))
    nodes.append(_node_xml(b_load_idx(c), _v_op("LOAD", simd_lanes),
                           asap=c, alap=c,
                           inputs=[], outputs=outs2,
                           simd_lanes=simd_lanes,
                           base_name="var_b"))

  # Compute cells. No outputs — TREG accumulator stays in-cell across iters
  # (the artifact simulator's MAC reads `previousValues_[node.idx]`).
  for r in range(H):
    for c in range(W):
      gi = r * W + c
      cur_idx = cell_idx(r, c)
      if gi >= useful_cells:
        nodes.append(_node_xml(cur_idx, "NOP", asap=r + c + 1,
                               alap=r + c + 1,
                               inputs=[], outputs=[]))
        continue
      ins = [a_load_idx(r), b_load_idx(c)]
      nodes.append(_node_xml(cur_idx, _v_op(op_kind, simd_lanes),
                             asap=r + c + 1, alap=r + c + 1,
                             inputs=ins, outputs=[],
                             simd_lanes=simd_lanes))

  return _wrap_dfg(nodes, iter_max)


def gemm_output_stationary_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


# ---------------------------------------------------------------------------
# Template: gemm_two_stream_store
# ---------------------------------------------------------------------------
#
# Source-role-compatible GEMM dataflow:
#
#   - H A-LOADs per cycle, one per output row, broadcast across columns.
#   - W B-LOADs per cycle, one per output column, broadcast across rows.
#   - W*H MAC cells, each accumulating one C[r,c].
#   - One STORE per useful cell per cycle, writing the current accumulated C.
#
# This is intentionally more memory-heavy than gemm_fanout, but it exposes the
# three source roles (A/B/C) in the DFG so a source-aware lowering can bind real
# base names and address schedules. It is the mapper-feasibility target for
# 3-side memory architectures.
# ---------------------------------------------------------------------------


def gemm_two_stream_store(dim_x: int, dim_y: int, iter_max: int,
                          op_kind: str = "MAC",
                          useful_cells: int | None = None,
                          simd_lanes: int = 1) -> str:
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []

  def a_load_idx(r: int) -> int:
    return r

  def b_load_idx(c: int) -> int:
    return H + c

  def cell_idx(r: int, c: int) -> int:
    return H + W + r * W + c

  store_base = H + W + W * H
  bytes_per_word = 4 * max(1, simd_lanes)
  base_size = max(256, useful_cells * bytes_per_word)

  for r in range(H):
    outs: List[Tuple[int, str]] = []
    for c in range(W):
      gi = r * W + c
      if gi < useful_cells:
        outs.append((cell_idx(r, c), "I1"))
    nodes.append(_node_xml(a_load_idx(r), _v_op("LOAD", simd_lanes),
                           asap=0, alap=0,
                           inputs=[], outputs=outs,
                           simd_lanes=simd_lanes,
                           base_name="var_a"))

  for c in range(W):
    outs: List[Tuple[int, str]] = []
    for r in range(H):
      gi = r * W + c
      if gi < useful_cells:
        outs.append((cell_idx(r, c), "I2"))
    nodes.append(_node_xml(b_load_idx(c), _v_op("LOAD", simd_lanes),
                           asap=0, alap=0,
                           inputs=[], outputs=outs,
                           simd_lanes=simd_lanes,
                           base_name="var_b"))

  for r in range(H):
    for c in range(W):
      gi = r * W + c
      cur = cell_idx(r, c)
      if gi >= useful_cells:
        nodes.append(_node_xml(cur, "NOP", asap=1, alap=1,
                               inputs=[], outputs=[]))
        continue
      nodes.append(_node_xml(cur, _v_op(op_kind, simd_lanes),
                             asap=1, alap=1,
                             inputs=[a_load_idx(r), b_load_idx(c)],
                             outputs=[(store_base + gi, "I1")],
                             simd_lanes=simd_lanes))

  for i in range(useful_cells):
    nodes.append(_node_xml(store_base + i, _v_op("STORE", simd_lanes),
                           asap=2, alap=2,
                           inputs=[cell_idx(i // W, i % W)],
                           outputs=[],
                           simd_lanes=simd_lanes,
                           base_name="var_c",
                           base_size=base_size))

  return _wrap_dfg(nodes, iter_max)


def gemm_two_stream_store_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


# ---------------------------------------------------------------------------
# Template: batched_dot_output_stationary
# ---------------------------------------------------------------------------
#
# A source-bindable output-stationary grid for independent dot products:
#
#   - one source-value LOAD per active output cell
#   - one shared coefficient LOAD per cycle
#   - one MAC per active output cell, accumulating in that PE's TREG
#   - no steady-state stores; final values require a drain phase
#
# This is the natural transformed shape for matrix-vector products and
# single-filter fixed weighted sums such as conv2d after patch extraction.
# Unlike matvec_parallel, it does not hide the source bandwidth: a full 4x4
# grid has 17 loads/cycle, so a 12-memory-PE architecture cannot sustain II=1
# without an additional line-buffer/data-reuse transformation.
# ---------------------------------------------------------------------------


def batched_dot_output_stationary(dim_x: int, dim_y: int, iter_max: int,
                                  op_kind: str = "MAC",
                                  useful_cells: int | None = None,
                                  simd_lanes: int = 1) -> str:
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  coeff_idx = useful_cells
  cell_base = coeff_idx + 1

  for i in range(useful_cells):
    nodes.append(_node_xml(
        i, _v_op("LOAD", simd_lanes), asap=0, alap=0,
        inputs=[], outputs=[(cell_base + i, "I1")],
        base_name="var_a", simd_lanes=simd_lanes))

  coeff_outputs = [(cell_base + i, "I2") for i in range(useful_cells)]
  # Coefficients are scalar across SIMD output lanes. Vector architectures in
  # this repository advertise SCALAR_BROADCAST, so keep this as a scalar LOAD
  # feeding VMAC I2 rather than incorrectly loading adjacent coefficients.
  nodes.append(_node_xml(
      coeff_idx, "LOAD", asap=0, alap=0,
      inputs=[], outputs=coeff_outputs,
      base_name="var_b", simd_lanes=1))

  for i in range(useful_cells):
    nodes.append(_node_xml(
        cell_base + i, _v_op(op_kind, simd_lanes), asap=1, alap=1,
        inputs=[i, coeff_idx], outputs=[],
        simd_lanes=simd_lanes))

  return _wrap_dfg(nodes, iter_max)


def batched_dot_output_stationary_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


# ---------------------------------------------------------------------------
# Template: gemm_output_stationary_epilogue
# ---------------------------------------------------------------------------
#
# One-shot drain of W*H TREG accumulators to memory.
#
# Each useful cell emits one STORE node. In real hardware, the STORE reads
# the cell PE's TREG and writes it to var_c[r*W+c]. In the DFG XML, we use
# CONST=0 + GEPOffset to make the STORE structurally valid; the actual value
# being drained is the *same PE's* TREG (a hardware-implicit input, not
# representable in the current DFG XML format).
#
# Mapping correctness requirement: the epilogue's STORE nodes must land on
# the same PEs as the corresponding main MACs so that the PE's TREG carries
# the right value. This needs cross-DFG PE assignment, which we do not yet
# enforce (see PLAN.md / REFERENCE.md). The mapper today places each phase
# independently, so the resulting binaries are NOT functionally chained;
# they are emitted for util accounting and hardware-shape reference.
# ---------------------------------------------------------------------------


def gemm_output_stationary_epilogue(dim_x: int, dim_y: int, iter_max: int,
                                     op_kind: str = "MAC",
                                     useful_cells: int | None = None,
                                     simd_lanes: int = 1) -> str:
  _ = op_kind
  _ = iter_max  # one-shot
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  bytes_per_word = 4 * max(1, simd_lanes)
  base_size = max(256, useful_cells * bytes_per_word)

  for i in range(useful_cells):
    parts: List[str] = []
    attrs = ('idx="%d" ASAP="0" ALAP="0" BB="0" CONST="0" SHIFT="0" OFFSET="0"'
             % i)
    if simd_lanes > 1:
      attrs += ' simd_packed="true" simd_lanes="%d"' % simd_lanes
    parts.append('  <Node %s>' % attrs)
    parts.append('    <OP>%s</OP>' % _v_op("STORE", simd_lanes))
    parts.append('    <Inputs/>')
    parts.append('    <Outputs/>')
    parts.append('    <RecParents/>')
    parts.append('    <BasePointerName size="%d">var_c</BasePointerName>'
                 % base_size)
    parts.append('    <GEPOffset>%d</GEPOffset>' % (i * bytes_per_word))
    parts.append('  </Node>')
    nodes.append("\n".join(parts))

  return _wrap_dfg(nodes, iter_max=1)


def gemm_output_stationary_epilogue_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


def gemm_output_stationary_treg_drain_epilogue(
    dim_x: int, dim_y: int, iter_max: int,
    op_kind: str = "MAC",
    useful_cells: int | None = None,
    simd_lanes: int = 1) -> str:
  """Drain PE-local output-stationary accumulators through pinned MAC nodes.

  Each logical output cell emits a MAC with CONST=0 and no inputs. In the
  arch-aware simulator and intended hardware model, that computes
  ``0 * 0 + TREG`` on the pinned PE, preserving the accumulator value and
  forwarding it to a STORE. Correctness requires each drain MAC to be pinned
  to the same PE as the corresponding main-phase MAC.
  """
  _ = dim_y
  _ = iter_max
  _ = op_kind
  total_cells = dim_x * dim_y
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  store_base = useful_cells
  bytes_per_word = 4 * max(1, simd_lanes)
  base_size = max(256, useful_cells * bytes_per_word)

  for i in range(useful_cells):
    nodes.append(_node_xml(
        i, _v_op("MAC", simd_lanes), asap=0, alap=0,
        inputs=[], outputs=[(store_base + i, "I1")],
        simd_lanes=simd_lanes, const_value=0))
  for i in range(useful_cells):
    nodes.append(_node_xml(
        store_base + i, _v_op("STORE", simd_lanes), asap=1, alap=1,
        inputs=[i], outputs=[], base_name="var_c", base_size=base_size,
        simd_lanes=simd_lanes, gep_offset=i * bytes_per_word))

  return _wrap_dfg(nodes, iter_max=1)


def gemm_output_stationary_treg_drain_epilogue_cells(
    dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


def main_phase_cell_node_ids(template: str, dim_x: int, dim_y: int,
                             useful_cells: int) -> tuple[int, ...]:
  useful_cells = max(0, min(useful_cells, dim_x * dim_y))
  if template == "gemm_output_stationary":
    return tuple(dim_y + dim_x + i for i in range(useful_cells))
  if template == "batched_dot_output_stationary":
    return tuple(useful_cells + 1 + i for i in range(useful_cells))
  return ()


def treg_drain_cell_node_ids(template: str, useful_cells: int) -> tuple[int, ...]:
  if template in {
      "gemm_output_stationary",
      "batched_dot_output_stationary",
  }:
    return tuple(range(max(0, useful_cells)))
  return ()


# ---------------------------------------------------------------------------
# Template: gemm_weight_stationary
# ---------------------------------------------------------------------------
#
# Weight-stationary mapping:
#
#   - Each cell holds one weight in its TREG (loaded once by the prologue).
#   - Per cycle: ONE input load (A) per row, multiplied with the cell's
#     stationary weight; outputs stream out per cycle (one STORE per cell
#     per cycle).
#   - iter_max = K input samples streamed through.
#
# Without a 3-input MAC (see REFERENCE.md), the stationary weight is modeled
# as an implicit TREG value preloaded by the prologue. The main DFG's MAC
# nodes read I1=A_load, I2=A_load (degenerate), and TREG=weight. Output is
# MAC's result, sent to STORE.
#
# Simulator caveat: same cross-DFG TREG limitation as OS — the simulator
# can't access the prologue's TREG state from a separate DFG. For shape and
# util reference only.
# ---------------------------------------------------------------------------


def gemm_weight_stationary(dim_x: int, dim_y: int, iter_max: int,
                            op_kind: str = "MAC",
                            useful_cells: int | None = None,
                            simd_lanes: int = 1) -> str:
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []

  # idx layout: input-loads [0..H-1], cells [H..H+W*H-1], stores [H+W*H..]
  def in_load_idx(r: int) -> int:
    return r

  def cell_idx(r: int, c: int) -> int:
    return H + r * W + c

  store_base = H + W * H

  # Input LOADs (one per row, broadcast across columns of cells in that row).
  for r in range(H):
    outs: List[Tuple[int, str]] = []
    for c in range(W):
      gi = r * W + c
      if gi < useful_cells:
        outs.append((cell_idx(r, c), "I1"))
    nodes.append(_node_xml(in_load_idx(r), _v_op("LOAD", simd_lanes),
                           asap=0, alap=0,
                           inputs=[], outputs=outs,
                           simd_lanes=simd_lanes,
                           base_name="var_a"))

  # Compute cells: MAC(input, input, stationary_weight). The weight is in
  # TREG (loaded by the prologue). Each cell's STORE consumes its output.
  for r in range(H):
    for c in range(W):
      gi = r * W + c
      cur_idx = cell_idx(r, c)
      if gi >= useful_cells:
        nodes.append(_node_xml(cur_idx, "NOP", asap=1, alap=1,
                               inputs=[], outputs=[]))
        continue
      # Single-input cell with TREG accumulator: feed LOAD as both I1 and I2
      # so the MAC's product becomes A*A (representative shape; the real
      # hardware behavior is A*weight via the implicit 3rd MAC input).
      ins = [in_load_idx(r)]
      outs2: List[Tuple[int, str]] = [(store_base + gi, "I1")]
      nodes.append(_node_xml(cur_idx, _v_op(op_kind, simd_lanes),
                             asap=1, alap=1,
                             inputs=ins, outputs=outs2,
                             simd_lanes=simd_lanes))

  # STOREs (one per useful cell, drained every cycle — WS streams outputs).
  bytes_per_word = 4 * max(1, simd_lanes)
  for i in range(useful_cells):
    nodes.append(_node_xml(store_base + i, _v_op("STORE", simd_lanes),
                           asap=2, alap=2,
                           inputs=[cell_idx(i // W, i % W)],
                           outputs=[],
                           simd_lanes=simd_lanes,
                           base_name="var_c",
                           base_size=max(256, useful_cells * bytes_per_word)))

  return _wrap_dfg(nodes, iter_max)


def gemm_weight_stationary_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


def gemm_weight_stationary_prologue(dim_x: int, dim_y: int, iter_max: int,
                                     op_kind: str = "MAC",
                                     useful_cells: int | None = None,
                                     simd_lanes: int = 1) -> str:
  """Load one weight per useful cell from var_b into the cell's TREG slot.

  Modeled in the DFG as one LOAD → STORE pair per useful cell, where the
  STORE writes the weight to a designated `var_b_stat` memory location.
  The mapper places each pair on a PE; in hardware the PE's TREG should
  end up holding the loaded weight (requires cross-phase PE assignment;
  not enforced today).
  """
  _ = op_kind
  _ = iter_max  # one-shot
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  bytes_per_word = 4 * max(1, simd_lanes)
  base_size = max(256, useful_cells * bytes_per_word)

  # 2 nodes per cell: LOAD weight from var_b[i] → STORE to var_b_stat[i].
  # var_b_stat is a separate base; on real hardware it would map to the
  # PE-local TREG location for that cell.
  for i in range(useful_cells):
    load_idx = 2 * i
    store_idx = 2 * i + 1
    nodes.append(_node_xml(load_idx, _v_op("LOAD", simd_lanes),
                           asap=0, alap=0,
                           inputs=[], outputs=[(store_idx, "I1")],
                           simd_lanes=simd_lanes,
                           base_name="var_b",
                           base_size=base_size))
    # STORE the loaded weight into the cell's stationary slot.
    parts: List[str] = []
    attrs = ('idx="%d" ASAP="1" ALAP="1" BB="0" CONST="0" SHIFT="0" OFFSET="0"'
             % store_idx)
    if simd_lanes > 1:
      attrs += ' simd_packed="true" simd_lanes="%d"' % simd_lanes
    parts.append('  <Node %s>' % attrs)
    parts.append('    <OP>%s</OP>' % _v_op("STORE", simd_lanes))
    parts.append('    <Inputs>')
    parts.append('      <Input idx="%d"/>' % load_idx)
    parts.append('    </Inputs>')
    parts.append('    <Outputs/>')
    parts.append('    <RecParents/>')
    parts.append('    <BasePointerName size="%d">var_b_stat</BasePointerName>'
                 % base_size)
    parts.append('    <GEPOffset>%d</GEPOffset>' % (i * bytes_per_word))
    parts.append('  </Node>')
    nodes.append("\n".join(parts))

  return _wrap_dfg(nodes, iter_max=1)


def gemm_weight_stationary_prologue_cells(dim_x: int, dim_y: int) -> int:
  return dim_x * dim_y


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


# ---------------------------------------------------------------------------
# Output-stationary accumulator-init prologue
# ---------------------------------------------------------------------------
#
# Emits a one-shot DFG that writes the value 0 to a designated accumulator-init
# memory location for each useful compute cell. Intended to precede a main
# systolic DFG that accumulates partial sums per cell:
#
#   prologue:  for each cell c in useful_cells:
#                 STORE 0 -> var_c[c]      # clear accumulator location
#   main:      <MAC chain that accumulates into var_c[c] across iter_max cycles>
#   epilogue:  (optional drain — see gemm_fanout_epilogue)
#
# Each prologue node is a STORE whose value is the constant 0. The node has no
# data inputs; the simulator's executeNode returns the node's CONST when
# `hasConst && inputs.empty()`. Each STORE targets a distinct GEPOffset within
# `var_c` so the mapper places useful_cells distinct memory ops.
#
# Limitations:
#   - This prologue clears MEMORY, not the PE's TREG. For *true* hardware
#     output-stationary (accumulator lives in TREG), the prologue should also
#     route the const-0 into the same PE's TREG as a downstream MAC. That
#     requires the prologue and main mappings to share PE assignments (a Phase
#     2c-equivalent constraint). Today the artifact simulator's MAC already
#     zero-inits `previousValues_[mac.idx]` on iter 0, so the simulator does
#     not require the prologue for functional correctness; the prologue is
#     emitted primarily as a hardware artifact / utilization reference point.
#   - The mapper does not know that the prologue and main should land on
#     overlapping PEs; today the prologue's util is reported but its mapping
#     is independent.
# ---------------------------------------------------------------------------


def gemm_fanout_prologue(dim_x: int, dim_y: int, iter_max: int,
                         op_kind: str = "ADD", useful_cells: int | None = None,
                         simd_lanes: int = 1) -> str:
  """One-shot accumulator-init: write 0 to var_c[0..useful_cells-1].

  Honors the same (W, H, useful_cells, simd_lanes) shape as `gemm_fanout`
  so the prologue produces one zero-init slot per main-DFG useful cell.
  `op_kind` and `iter_max` are accepted for signature uniformity; the
  prologue itself always runs a single iteration and ignores op_kind.
  """
  _ = op_kind
  _ = iter_max  # prologue is one-shot
  W, H = dim_x, dim_y
  total_cells = W * H
  if useful_cells is None:
    useful_cells = total_cells
  useful_cells = max(0, min(useful_cells, total_cells))

  nodes: List[str] = []
  # `var_c` array size in bytes — must be constant across every node with
  # base_pointer_name=var_c (the mapper's DFG parser asserts on inconsistent
  # sizes per name, see DFG.cpp:148). Sized for `useful_cells` SIMD-words
  # at 4 bytes/lane, padded to the nearest power-of-two slot to give the
  # mapper room.
  bytes_per_word = 4 * max(1, simd_lanes)
  base_size = max(256, useful_cells * bytes_per_word)

  for i in range(useful_cells):
    # STORE node with CONST=0 and no data inputs. The artifact simulator's
    # executeNode returns the node's constant when `hasConst && inputs.empty`
    # (see SimulatorBase.cpp:317), and routes that value into the memory
    # write path because op startswith STORE/VSTORE. Each cell gets a
    # distinct GEPOffset slot — i*bytes_per_word — so the resulting store
    # addresses don't collide. All nodes share size=base_size; the offset
    # field carries the per-cell distinguishing address.
    parts: List[str] = []
    attrs = ('idx="%d" ASAP="0" ALAP="0" BB="0" CONST="0" SHIFT="0" OFFSET="0"'
             % i)
    if simd_lanes > 1:
      attrs += ' simd_packed="true" simd_lanes="%d"' % simd_lanes
    parts.append('  <Node %s>' % attrs)
    parts.append('    <OP>%s</OP>' % _v_op("STORE", simd_lanes))
    parts.append('    <Inputs/>')
    parts.append('    <Outputs/>')
    parts.append('    <RecParents/>')
    parts.append('    <BasePointerName size="%d">var_c</BasePointerName>'
                 % base_size)
    parts.append('    <GEPOffset>%d</GEPOffset>' % (i * bytes_per_word))
    parts.append('  </Node>')
    nodes.append("\n".join(parts))

  return _wrap_dfg(nodes, iter_max=1)


def gemm_fanout_prologue_cells(dim_x: int, dim_y: int) -> int:
  """useful-cell count for the prologue is the same as its main counterpart."""
  return dim_x * dim_y


TEMPLATES = {
    "gemm_fanout": gemm_fanout,
    "gemm_chain": gemm_chain,
    "matvec_row": matvec_row,
    "matvec_parallel": matvec_parallel,
    "gemm_fanout_prologue": gemm_fanout_prologue,
    "gemm_output_stationary": gemm_output_stationary,
    "gemm_two_stream_store": gemm_two_stream_store,
    "batched_dot_output_stationary": batched_dot_output_stationary,
    "gemm_output_stationary_epilogue": gemm_output_stationary_epilogue,
    "gemm_output_stationary_treg_drain_epilogue": (
        gemm_output_stationary_treg_drain_epilogue
    ),
    "gemm_weight_stationary": gemm_weight_stationary,
    "gemm_weight_stationary_prologue": gemm_weight_stationary_prologue,
}

# Public template catalog.  Source-bound templates whose construction needs
# affine source records or mandatory placement constraints are deliberately
# catalogued here without being inserted into TEMPLATES: the generic callable
# API cannot instantiate them correctly.
TEMPLATE_CATALOG = {
    name: {
        "kind": "shape_template",
        "builder": f"morpher_runner.systolic_templates.{fn.__name__}",
        "requires_source_record": False,
        "requires_node_pinning": False,
        "functionally_verified": False,
    }
    for name, fn in TEMPLATES.items()
}
TEMPLATE_CATALOG.update({
    "port_balanced_batched_dot_output_stationary": {
        "kind": "source_bound_policy",
        "builder": (
            "morpher_runner.systolic_templates."
            "batched_dot_output_stationary"
        ),
        "requires_source_record": True,
        "requires_node_pinning": False,
        "functionally_verified": False,
        "notes": (
            "Selects active output lanes from available memory ports; one "
            "source stream per lane plus one broadcast coefficient stream."
        ),
    },
})

# Per-template "useful compute cells when useful_cells=None (full template)".
# This is what the template emits as actual MAC/ADD nodes on the dim_x*dim_y
# compute grid. matvec_row only uses a single row → dim_x cells out of
# dim_x*dim_y. The others fill the full grid.
TEMPLATE_DEFAULT_CELLS = {
    "gemm_fanout": gemm_fanout_cells,
    "gemm_chain": gemm_chain_cells,
    "matvec_row": matvec_row_cells,
    "matvec_parallel": matvec_parallel_cells,
    "gemm_fanout_prologue": gemm_fanout_prologue_cells,
    "gemm_output_stationary": gemm_output_stationary_cells,
    "gemm_two_stream_store": gemm_two_stream_store_cells,
    "batched_dot_output_stationary": batched_dot_output_stationary_cells,
    "gemm_output_stationary_epilogue": gemm_output_stationary_epilogue_cells,
    "gemm_output_stationary_treg_drain_epilogue": (
        gemm_output_stationary_treg_drain_epilogue_cells
    ),
    "gemm_weight_stationary": gemm_weight_stationary_cells,
    "gemm_weight_stationary_prologue": gemm_weight_stationary_prologue_cells,
}


# Maps each main-template to its prologue template. Used by the orchestrator
# when --emit-prologue is set: the main template selects a paired prologue.
# Absent entries mean "no prologue defined for that main shape".
PROLOGUE_FOR_TEMPLATE = {
    "gemm_fanout": "gemm_fanout_prologue",
    "gemm_chain": "gemm_fanout_prologue",  # same accumulator-init shape
    "matvec_parallel": "gemm_fanout_prologue",
    # matvec_row uses a 1D accumulator line; for now a row-scoped prologue
    # would help, but the gemm_fanout_prologue's per-cell zero-init is a
    # superset (writes more slots than needed, which is harmless).
    "matvec_row": "gemm_fanout_prologue",
    # Output-stationary main needs the same zero-init prologue: clear every
    # cell's accumulator slot before the K-cycle stream begins.
    "gemm_output_stationary": "gemm_fanout_prologue",
    "batched_dot_output_stationary": "gemm_fanout_prologue",
    # Weight-stationary main needs a *different* prologue: load weights from
    # var_b into each cell's stationary slot, not zero them out.
    "gemm_weight_stationary": "gemm_weight_stationary_prologue",
}


# Maps each main-template to its epilogue template. `--emit-epilogue` maps
# the listed epilogue as a third phase after main (or second when no
# prologue). Today only output-stationary has an epilogue; the streaming
# main templates (gemm_fanout, gemm_chain, matvec_*) drain via per-iter
# stores already, and weight-stationary streams outputs out per cycle.
EPILOGUE_FOR_TEMPLATE = {
    "gemm_output_stationary": "gemm_output_stationary_treg_drain_epilogue",
    "batched_dot_output_stationary": "gemm_output_stationary_treg_drain_epilogue",
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
