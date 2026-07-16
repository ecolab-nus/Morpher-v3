# Morpher Code Guide

Code-orientation for contributors: how data flows through the stack, which directory owns what, and where to make a given change. Feature and configuration reference lives in the [README](../README.md); per-directory detail lives in [`docs/components/`](components/) (linked from the code map below).

## End-to-end data flow

```
C kernel ──► DFG generation ──► mapping ──► validation
             (LLVM pass)        (PathFinder  (artifact simulator,
                                 + SA)        functional or cycle-accurate)
```

1. `run_morpher.py` compiles the kernel C to LLVM IR and runs the optimization pipeline.
2. The Morpher LLVM pass (`libdfggenPass.so`) emits DFG XML + memory-allocation metadata, and (when simulation is enabled) an instrumented run emits memory traces.
3. The mapper consumes DFG XML + architecture JSON and produces the mapping artifact.
4. The artifact simulator replays the mapped DFG against the traces and reports `Matches::N, Mismatches::M`.

The Python runner (`run_morpher.py` + `morpher_runner/`) is the control plane that sequences these stages per `runmode`.

## Inputs and outputs

**C input mode** — source must live at `Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c`:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

**XML input mode** — skip DFG generation and map a pre-generated DFG. Supports `mapper_only` (default), `runall`, `sim_only`; `function` defaults to the filename stem; in `memory_mode: auto` a mem-alloc file is required (beside the XML or via `--mem-alloc`):

```bash
python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]
```

**Generated files** (under the tagged workspace / benchmark trees):

| Stage | Files |
|---|---|
| DFG generator | `<kernel>_PartPredDFG.xml`, `.dot` + `.pdf`, `<kernel>_mem_alloc.txt`, `memtraces/<kernel>_trace_*.txt` |
| Mapper | staged `.xml` + `_mem_alloc.txt`, `<kernel>_arch_runtime.json` (auto mode), `*.mappingwithlatency.txt` (+ sibling `mapping.json`), `result.txt` |

Artifact simulation consumes the mapped DFG, latency mapping, mem-alloc, and traces directly; no hardware binary is emitted or staged.

## Code map

| Directory / file | Owns | Detail |
|---|---|---|
| `run_morpher.py` | CLI parsing (C + XML mode), context build, stage dispatch by `runmode` | [RUNNER_PIPELINE](components/RUNNER_PIPELINE.md) |
| `morpher_runner/` | Python control plane: `context.py` (config load/validate, legacy keys), `dfg_generator.py` (LLVM compile/opt, pass run, instrumentation, traces), `mapper.py` (arch resolution, `update_mem_alloc.py`, mapper exec), `artifact_simulator.py` (trace sampling + sim loop), `simd_packer.py` / `trace_partition.py` / `utilization.py`, and `utils.py` / `exec.py` / `fs_ops.py` helpers | [RUNNER_PIPELINE](components/RUNNER_PIPELINE.md) |
| `Morpher_DFG_Generator/` (C++) | LLVM DFG pass: `src/dfggenPass.cpp` (entry, access-pattern validation, transform flags), `src/dfg/DFGPartPred.cpp` (partial predication), `src/common/dfg.cpp` (DFG structure, iterator-control removal + SIMD packing manifests), `src/instrumentation/` (trace hooks) | [DFG_GENERATOR](components/DFG_GENERATOR.md) |
| `Morpher_CGRA_Mapper/` (C++) | Mapping engine: `src/CGRA_xml_compiler.cpp` (entry, method select, II search/restart), `src/mapper/PathFinderMapper.cpp` (placement/routing), `HeuristicMapper.cpp` (min-II estimation), `SimulatedAnnealingMapper.cpp` (SA), `update_mem_alloc.py` (auto-mode runtime JSON) | [MAPPER](components/MAPPER.md) |
| `simulator/artifact_simulator/` (C++) | Validation: `src/functional/` (arch-unaware golden checker), `src/morpher/CycleSimulator.cpp` (cycle-accurate), `MappingCompiler.cpp`, `NodeSemantics.cpp`, `BoundaryContract.cpp` | [SIMULATOR](components/SIMULATOR.md) |
| `scripts/` | `run_all_benchmarks.py`, `run_regression_benchmarks.py`, `run_arch_sweeps.py`, `regression_gate.py`, `clean_workspace.sh`, `check_no_generated_tracked.sh`, legacy `bin_to_trace.py` / `hycube_config.py` | [SCRIPTS_CONFIG](components/SCRIPTS_CONFIG.md) |
| `config/` | Run configurations; `config/README.md` describes each | [SCRIPTS_CONFIG](components/SCRIPTS_CONFIG.md) |

## Where to start for common changes

| Goal | Start here |
|---|---|
| CLI behavior or stage order | `run_morpher.py` |
| Config schema / default behavior | `morpher_runner/context.py` + `config/*.yaml` |
| DFG generation diagnostics | `morpher_runner/utils.py` + generator pass code |
| Mapping algorithm behavior | `Morpher_CGRA_Mapper/src/mapper/*` |
| Simulation semantics / stopping behavior | `simulator/artifact_simulator/src/*` |
| Benchmark automation / reporting | `scripts/run_all_benchmarks.py` |
| Supported DFG transform contracts | `Morpher_DFG_Generator/TRANSFORM_LIMITATIONS.md` |
