"""Reference trace generator for synthesized systolic DFGs.

Given a synthesized DFG XML, generate deterministic test inputs at LOAD
addresses, run a Python ref-sim that mirrors the DFG's per-cycle execution
(ADD = pass-through for single-input cells, MAC = a*b + prev_acc, STORE =
write data), and emit `<kernel>_mem_alloc.txt`, `<kernel>_addr_trace.txt`,
and `<kernel>_trace.txt` files that the artifact_simulator consumes for
functional verification.

Honest note: this is end-to-end verification of the *synthesized DFG's
semantics*, not of the original source kernel. The DFG's math is the
systolic-template math, not the polybench kernel math. A matching simulator
output confirms the mapper + simulator correctly executed the DFG; it does
NOT confirm correctness for the source-level kernel.
"""

from __future__ import annotations

import os
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Dict, List, Tuple


@dataclass
class TraceNode:
  idx: int
  op: str
  inputs: List[int] = field(default_factory=list)
  outputs: List[int] = field(default_factory=list)


def parse_dfg(xml_path: str) -> Tuple[List[TraceNode], int]:
  """Parse a synthesized DFG XML. Returns (nodes_in_idx_order, iter_max)."""
  # Synthesized XML has multiple top-level elements (MutexBB, ITER, DFG)
  # without a wrapping root. Wrap it so ElementTree accepts it.
  with open(xml_path, "r", encoding="utf-8") as f:
    text = f.read()
  root = ET.fromstring("<root>" + text + "</root>")

  iter_max = 1
  iter_elem = root.find("ITER")
  if iter_elem is not None and iter_elem.get("max_count"):
    iter_max = int(iter_elem.get("max_count"))

  nodes: Dict[int, TraceNode] = {}
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("No <DFG> element in " + xml_path)
  for n in dfg.findall("Node"):
    idx = int(n.get("idx"))
    op = (n.findtext("OP") or "").strip()
    inputs = []
    inp_block = n.find("Inputs")
    if inp_block is not None:
      for inp in inp_block.findall("Input"):
        inputs.append(int(inp.get("idx")))
    nodes[idx] = TraceNode(idx=idx, op=op, inputs=inputs)
  # Outputs link forward — build for completeness.
  for n in dfg.findall("Node"):
    idx = int(n.get("idx"))
    outs = n.find("Outputs")
    if outs is not None:
      for o in outs.findall("Output"):
        # only current-iter outputs are needed for forward sim
        if int(o.get("nextiter") or "0") == 0:
          nodes[idx].outputs.append(int(o.get("idx")))

  ordered = [nodes[k] for k in sorted(nodes.keys())]
  return ordered, iter_max


def _strip_v(op: str) -> str:
  """Strip the V-prefix from a SIMD op so the scalar ref-sim semantics apply
  (mirrors how the C++ simulator's evalBinary normalizes V-prefixed ops).
  Note: the ref-sim still produces SCALAR values per iter. Full lane-wise
  verification is not implemented here; --run-sim should be limited to
  scalar mode when used with these synthesized DFGs.
  """
  if len(op) > 1 and op[0] == "V":
    return op[1:]
  return op


def is_load(op: str) -> bool:
  o = _strip_v(op)
  return o == "LOAD" or o == "LOADH" or o == "LOADB"


def is_store(op: str) -> bool:
  o = _strip_v(op)
  return o == "STORE" or o == "STOREH" or o == "STOREB"


def gen_input_value(node_idx: int, iter_idx: int) -> int:
  """Deterministic input: (node_idx + iter_idx + 1) & 0xFF.
  Small uint8 range keeps accumulator products in range without overflow
  surprises on the 32-bit simulator path.
  """
  return ((node_idx + iter_idx + 1)) & 0xFF


def simulate(nodes: List[TraceNode], iter_max: int) -> Dict[Tuple[int, int], int]:
  """Run the ref-sim. Returns map from (node_idx, iter_idx) to output value
  (uint32). LOADs get deterministic input values. ADD with 1 input = pass-
  through. ADD with 2 inputs = a + b. MAC = a * b + prev_acc. STOREs see
  the value of their first input.
  """
  values: Dict[Tuple[int, int], int] = {}
  prev_outputs: Dict[int, int] = {}  # idx → previous-iter output
  for t in range(iter_max):
    fresh_prev = dict(prev_outputs)
    for node in nodes:
      if is_load(node.op):
        v = gen_input_value(node.idx, t)
      elif is_store(node.op):
        if not node.inputs:
          continue
        a_idx = node.inputs[0]
        v = values.get((a_idx, t), 0)
      elif node.op == "NOP":
        v = 0
      elif _strip_v(node.op) == "ADD":
        if len(node.inputs) == 0:
          v = 0
        elif len(node.inputs) == 1:
          v = values.get((node.inputs[0], t), 0)
        else:
          a = values.get((node.inputs[0], t), 0)
          b = values.get((node.inputs[1], t), 0)
          v = (a + b) & 0xFFFFFFFF
      elif _strip_v(node.op) == "MAC":
        if len(node.inputs) < 2:
          # Should not happen with the fixed template, but fall back to
          # ADD-with-accumulator just in case.
          a = values.get((node.inputs[0], t), 0) if node.inputs else 0
          acc = fresh_prev.get(node.idx, 0)
          v = (a + acc) & 0xFFFFFFFF
        else:
          a = values.get((node.inputs[0], t), 0)
          b = values.get((node.inputs[1], t), 0)
          acc = fresh_prev.get(node.idx, 0)
          v = (a * b + acc) & 0xFFFFFFFF
      else:
        v = 0  # unsupported op
      values[(node.idx, t)] = v
      prev_outputs[node.idx] = v
  return values


@dataclass
class TraceArtifacts:
  mem_alloc_path: str
  addr_trace_path: str
  trace_path: str


def emit_artifacts(xml_path: str, output_dir: str, kernel: str,
                   word_bytes: int = 4) -> TraceArtifacts:
  """Generate mem_alloc, addr_trace, and trace files for a synthesized DFG.

  Memory layout: each LOAD node gets a base address `node_idx * 1024`; each
  STORE node gets a base at `0x10000 + node_idx * 1024`. LOAD reads address
  base + iter * word_bytes; STORE writes to base + iter * word_bytes.
  """
  nodes, iter_max = parse_dfg(xml_path)
  values = simulate(nodes, iter_max)

  loads = [n for n in nodes if is_load(n.op)]
  stores = [n for n in nodes if is_store(n.op)]

  os.makedirs(output_dir, exist_ok=True)
  mem_alloc_path = os.path.join(output_dir, kernel + "_mem_alloc.txt")
  addr_trace_path = os.path.join(output_dir, kernel + "_addr_trace.txt")
  trace_path = os.path.join(output_dir, kernel + "_trace.txt")

  with open(mem_alloc_path, "w") as f:
    for n in loads:
      f.write("load_%d,%d\n" % (n.idx, n.idx * 1024))
    for n in stores:
      f.write("store_%d,%d\n" % (n.idx, 0x10000 + n.idx * 1024))

  with open(addr_trace_path, "w") as f:
    f.write("node_idx,iter,addr,size,is_store\n")
    for t in range(iter_max):
      for n in loads:
        addr = n.idx * 1024 + t * word_bytes
        f.write("%d,%d,%d,%d,0\n" % (n.idx, t, addr, word_bytes))
      for n in stores:
        addr = 0x10000 + n.idx * 1024 + t * word_bytes
        f.write("%d,%d,%d,%d,1\n" % (n.idx, t, addr, word_bytes))

  with open(trace_path, "w") as f:
    f.write("var_name,offset,pre,post\n")
    for t in range(iter_max):
      for n in loads:
        addr = n.idx * 1024 + t * word_bytes
        # The simulator's trace records initial vs final byte values per
        # address. For LOADs the input data sits in memory at the load
        # address — we set pre = input value (low byte), post = same
        # (LOADs don't write back).
        v = gen_input_value(n.idx, t)
        f.write("load_%d,%d,%d,%d\n" % (n.idx, t * word_bytes, v & 0xFF, v & 0xFF))
      for n in stores:
        v = values.get((n.idx, t), 0)
        # post = expected value at the store address after the DFG runs.
        f.write("store_%d,%d,%d,%d\n" % (n.idx, t * word_bytes, 0, v & 0xFF))

  return TraceArtifacts(mem_alloc_path=mem_alloc_path,
                        addr_trace_path=addr_trace_path,
                        trace_path=trace_path)
