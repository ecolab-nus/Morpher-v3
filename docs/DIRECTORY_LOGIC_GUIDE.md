# Morpher Directory Logic Guide

This document explains the logic of each major directory in the repository.

It is written for contributors who want to understand how data flows through the stack and where to make changes.

## Detailed Component Docs

- `docs/components/RUNNER_PIPELINE.md`
- `docs/components/DFG_GENERATOR.md`
- `docs/components/MAPPER.md`
- `docs/components/SIMULATOR.md`
- `docs/components/SCRIPTS_CONFIG.md`

## End-to-End Data Flow

1. C kernel is compiled and transformed to LLVM IR.
2. LLVM pass generates DFG XML + memory allocation.
3. Mapper consumes DFG XML + architecture JSON and produces mapped binaries.
4. Simulator consumes mapped binary + traces/mem-allocation and reports matches/mismatches.

The Python runner (`run_morpher.py`) glues these stages together.

## `run_morpher.py` (Root Entrypoint)

Main logic:

- Parses CLI in C-source mode or XML mode.
- Builds run context (paths + loaded config).
- Executes stages based on `runmode`:
  - `dfg_gen_only`
  - `mapper_only`
  - `sim_only`
  - `runall`
- In XML mode, stages the input XML (and mem-alloc when needed) into mapper workspace.

Why it matters:

- This is the best place for pipeline-level behavior changes, validation, and stage ordering.

## `morpher_runner/` (Python Orchestration Layer)

This folder is the control plane for the full flow.

### `context.py`

- Loads YAML config into `RunnerConfig`.
- Handles compatibility with legacy config keys.
- Resolves canonical working directories for generator, mapper, simulator.

### `dfg_generator.py`

- Builds LLVM IR and optimized IR.
- Runs Morpher LLVM DFG pass.
- Validates required DFG outputs exist.
- Optionally instruments executable and generates memory traces.
- Copies artifacts into mapper/simulator stage folders.

### `mapper.py`

- Resolves architecture template path.
- In `memory_mode: auto`:
  - runs `Morpher_CGRA_Mapper/update_mem_alloc.py`
  - generates per-kernel runtime architecture JSON
- Runs `cgra_xml_mapper` with config-derived arguments.
- Copies emitted `*.bin` into simulator folder when simulation is enabled.

### `simulator.py`

- Collects trace files from DFG-generator trace folder.
- Samples traces (`max_test_samples`).
- Runs `hycube_simulator` per sample.
- Aggregates match/mismatch counts.

### `utils.py`

- Shared helpers for LLVM/clang command generation.
- Friendly diagnostics for DFG errors:
  - bank-size allocation failures
  - unsupported array dimensionality

### `exec.py`, `fs_ops.py`

- Lightweight subprocess and filesystem helper wrappers used across stages.

## `Morpher_DFG_Generator/` (LLVM DFG Generator)

Purpose:

- Transform loop kernels into Morpher DFG representation and memory-layout metadata.

Core logic areas:

- `src/dfggenPass.cpp`
  - LLVM pass entrypoint.
  - validates supported array/pointer dimensions.
  - controls pass options (function name, banks, bank size, AGI removal).
- `src/dfg/DFGPartPred.cpp`
  - partial predication logic.
  - branch/phi handling.
  - control/data edge construction for conditional flow.
- `src/common/dfg.cpp`
  - core DFG data structure operations.
  - node/edge management, basic-block connectivity handling.
- `src/instrumentation/instrumentation.cpp`
  - runtime tracing hooks.
  - controlled by `MORPHER_MAX_TRACES`.

Outputs produced by this directory:

- `<kernel>_PartPredDFG.xml`
- `<kernel>_PartPredDFG.dot` (+ PDF rendered by `dot`)
- `<kernel>_mem_alloc.txt`
- `memtraces/<kernel>_trace_*.txt` (when simulation is enabled)

## `Morpher_CGRA_Mapper/` (Mapping Backend)

Purpose:

- Map DFG nodes/edges to architecture resources and routing paths across II/time dimension.

Core logic areas:

- `src/CGRA_xml_compiler.cpp`
  - mapper executable entrypoint.
  - argument parsing integration.
  - architecture + DFG load.
  - selects mapper method (`PathFinder`, `SA`, `Hybrid`).
  - controls II search/restart strategy.
- `src/mapper/PathFinderMapper.cpp`
  - placement/routing search engine.
  - A*-like routing with congestion/conflict awareness.
  - backtracking and mutex/conflict checks.
- `src/mapper/HeuristicMapper.cpp`
  - resource-based minimum-II estimation and mapping logs.
- `src/mapper/SimulatedAnnealingMapper.cpp`
  - SA-based exploration for placement/routing quality.
  - temperature schedule, acceptance, best-state restore.
- `update_mem_alloc.py`
  - injects per-bank `DATA_LAYOUT` into architecture wrappers from mem-alloc file.
  - used by `memory_mode: auto`.

Key output artifacts:

- `*.mappingwithlatency.txt`
- `*.mapping.csv`, route/congestion logs
- `*.bin` mapped configuration binaries
- `result.txt` mapper summary

## `hycube_simulator/` (Cycle-Accurate Simulation)

Purpose:

- Execute mapped configuration and compare observed memory outputs to expected values.

Core logic areas:

- `src/hycube_sim.cpp`
  - executable entrypoint and argument parsing.
  - loads CMEM + DMEM/mem-alloc.
  - executes cycles until completion condition, then epilogue cycles.
  - writes final simulation summary.
- `src/CGRA.cpp`
  - full-array orchestration.
  - parses CMEM/DMEM formats.
  - executes per-cycle tile updates.
  - computes match/mismatch and writes `sim_result.txt`.
- `src/CGRATile.cpp`
  - per-tile instruction semantics (ALU ops, load/store, predicate behavior, routing signals).

Primary output:

- `sim_result.txt` as `matches,mismatches`

## `scripts/` (Automation + Repo Hygiene)

### `run_all_benchmarks.py`

- Discovers benchmarks from canonical layout:
  - `Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c`
- Runs many kernels in parallel.
- Captures logs and writes `benchmark_run_results.csv`.
- Extracts achieved II and sim match/mismatch from logs/artifacts.

### `clean_generated.sh`

- Removes generated benchmark artifacts from generator/mapper/simulator trees.
- Keeps placeholder `.gitkeep` files.

### `check_no_generated_tracked.sh`

- CI-style policy enforcement to ensure generated files are not tracked in git.

### `bin_to_trace.py`, `hycube_config.py`

- Legacy helper tooling for trace/memory conversion workflows.

## `config/` (Runtime Tuning)

- `default_config.yaml`: default integrated flow behavior.
- `xml_mapper_static.yaml`: XML-mode static mapping example.

Important dimensions this folder controls:

- memory mode (`auto` vs `static`)
- mapper strategy + II behavior
- simulator enable/disable and sample count
- architecture template selection

## `dfgPartition/` (Optional Experimental Partitioner)

Purpose:

- Partition large DFGs into clusters with additional load/store/select nodes to preserve communication correctness across clusters.

When to use:

- Research workflows where one big DFG must be split into manageable mapped clusters.

Main entry files:

- `main.py`
- `panorama.py`
- local `README.md` in that folder for algorithm notes

## Where to Start for Common Changes

- Change CLI behavior or stage order: `run_morpher.py`
- Change config schema/default behavior: `morpher_runner/context.py` + `config/*.yaml`
- Improve DFG generation diagnostics: `morpher_runner/utils.py` + generator pass code
- Improve mapping algorithm behavior: `Morpher_CGRA_Mapper/src/mapper/*`
- Change simulation semantics or stopping behavior: `hycube_simulator/src/*`
- Change benchmark automation/reporting: `scripts/run_all_benchmarks.py`
