# DFG Generator Logic (`Morpher_DFG_Generator`)

This document explains how the DFG generation backend works and what files it produces.

## Goal

Convert LLVM IR for a target loop/function into:

- Morpher DFG XML
- Graph DOT/PDF
- memory allocation metadata (`*_mem_alloc.txt`)
- optional runtime traces for simulation

## Key Directories

- `src/dfggenPass.cpp`: LLVM pass entrypoint and top-level flow
- `src/dfg/`: DFG construction, control/data edges, predication
- `src/common/`: core DFG data structures and graph utilities
- `src/instrumentation/`: runtime instrumentation for trace generation

## High-Level Flow

1. Front-end compiles kernel C file to LLVM IR.
2. Optimization pipeline canonicalizes IR.
3. Morpher DFG pass analyzes the selected function and loop structure.
4. Pass emits DFG and memory placement information.
5. Optional instrumentation executes the kernel to generate trace files.

## Core Logic Notes

## `src/dfggenPass.cpp`

Main responsibilities:

- Parse pass parameters (function name, banks, bank size, policy flags).
- Validate unsupported memory-access patterns early.
- Traverse loop/basic-block structure and build mapping units.
- Construct DFG nodes/edges, memory and control semantics.

Important behavior:

- Explicit validation rejects unsupported effective array dimensions/pointer depth.
- `-externalize-access=agi` removes address-generation nodes before
  instrumentation and emits an AGI manifest.
- `-externalize-access=agi_iter` additionally removes an isolated iterator
  control component after instrumentation and emits a reindex manifest. It
  fails when that component reaches remaining computation, memory, predicate,
  recurrence, or reduction behavior.
- `-simd-pack=<lanes>` runs after instrumentation, packs only complete closed
  lane-isomorphic subgraphs, and emits a SIMD manifest for trace remapping.
  The v1 path rejects control, recurrence, reduction, broadcast, partial-lane,
  and scalar/vector-boundary cases rather than changing their semantics.

## `src/dfg/DFGPartPred.cpp`

Main responsibilities:

- Build partial-predication control edges.
- Handle `PHI` and branch-derived control semantics.
- Introduce merge/select-like behavior for control-divergent regions.

Why this exists:

- Morpher supports control divergence in loops through predicated graph structure.

## `src/common/dfg.cpp`

Main responsibilities:

- DFG node/edge insertion, lookup, and connectivity management.
- Basic-block connectivity resolution.
- Helper routines for root/leaf detection and graph operations.

## `src/instrumentation/instrumentation.cpp`

Main responsibilities:

- Runtime hooks to dump memory-related behavior for simulation validation.
- Supports environment-controlled cap (`MORPHER_MAX_TRACES`).

## Inputs and Outputs

Input:

- LLVM IR for the target kernel/function.

Primary outputs per kernel:

- `<kernel>_PartPredDFG.xml`
- `<kernel>_PartPredDFG.dot`
- `<kernel>_PartPredDFG.pdf` (rendered via Graphviz)
- `<kernel>_mem_alloc.txt`
- `memtraces/<kernel>_trace_*.txt` (if instrumentation/simulation enabled)

## Typical Failure Modes

- Memory bank allocation failure:
  - array too large for configured bank size
  - runner now auto-retries with a larger effective `banksize` when the selected memory/SPM mode can absorb the change safely
  - if auto-growth is unsafe or still insufficient, increase `banksize` and/or `numberofbanks`
- Unsupported array/pointer dimensionality:
  - currently limited to patterns equivalent to up to 2D arrays
