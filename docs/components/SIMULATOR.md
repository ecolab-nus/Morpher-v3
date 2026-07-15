# Simulator Logic (`simulator/artifact_simulator`)

This document explains the validation simulator stage and how results are checked.

## Goal

Validate a mapped DFG artifact against memory traces recorded from an instrumented
native run of the source kernel. One binary supports two validation modes,
selected by the `validation_mode` config key:

- `functional`: arch-unaware golden checker. Parses the DFG XML, `mem_alloc`,
  and trace, and executes pure dataflow semantics (`FunctionalSimulator`),
  comparing stored values against the trace.
- `event-driven`: cycle-accurate. Elaborates the fabric from
  `<kernel>_arch_runtime.json`, compiles `mapping.json` + II into per-PE
  configuration memory, and runs `CycleSimulator` on the mapped schedule,
  including multi-phase runs and PE-local state.

## Key Files

- `src/main.cpp`: entrypoint; dispatches functional vs cycle-accurate on `--arch`
- `src/functional/`: arch-unaware dataflow replay
- `src/morpher/CycleSimulator.cpp`: cycle-accurate fabric execution
- `src/morpher/MappingCompiler.cpp`: mapping.json + II → per-PE configuration memory
- `src/morpher/NodeSemantics.cpp`: op semantics shared by both modes
- `src/morpher/BoundaryContract.cpp`: scalar/SIMD omitted-boundary contract handling

## Stage Inputs

- `--dfg <xml>`: mapped DFG (`<kernel>_PartPredDFG.xml`)
- `--mapping <mappingwithlatency.txt>`: mapper latency artifact (locator for its sibling `mapping.json`)
- `--mem-alloc <mem_alloc.txt>`: memory base-address map
- `--trace <trace.txt>` (+ optional `--addr-trace`): instrumented memory trace
- `--simd-manifest <json>` (optional): omitted-boundary simulation contract
- `--boundary-manifest <json>` (optional): scalar loop-state seed/drain contract
- `--arch <arch.json>` + `--result <result.txt>` (event-driven mode only)

## Integration with Python Runner

`morpher_runner/artifact_simulator.py` collects `<kernel>_trace_N.txt` files,
samples up to `max_test_samples`, runs the binary per sample, and aggregates
`Matches/Mismatches` from stdout. Missing traces skip the stage with a warning
(traces are only produced when `enable_simulation: true` during DFG generation).
Exit code 0 means all sampled traces matched; 2 means mismatches.

Scalar boundary manifests use schema v2. Their `launch.actions` are executable
cycle-simulator configuration actions: a TREG reset is applied to the shared
physical per-PE accumulator map before every loop invocation. Cycle simulation
also rejects such an action unless the elaborated architecture declares
`ARCH.CAPABILITIES.TREG_RESET_ON_LAUNCH: true`; an empty initial state is not
treated as evidence of reset behavior.
