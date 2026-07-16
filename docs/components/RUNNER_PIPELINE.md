# Runner and Pipeline Logic

This document explains the Python control flow that glues all Morpher stages together.

## Scope

- `run_morpher.py`
- `morpher_runner/context.py`
- `morpher_runner/dfg_generator.py`
- `morpher_runner/mapper.py`
- `morpher_runner/artifact_simulator.py`
- shared helpers in `morpher_runner/utils.py`, `exec.py`, `fs_ops.py`

## Pipeline at a Glance

1. Parse CLI arguments and determine input mode:
   - C source mode
   - XML mode
2. Load YAML config and resolve workspace paths.
3. Run one or more stages based on `runmode`:
   - DFG generation
   - Mapper
   - Simulator

## `run_morpher.py`

Main responsibilities:

- Parse arguments for C mode and XML mode.
- Stage XML/mem-alloc inputs for mapper in XML mode.
- Build a `RunnerContext`.
- Execute selected stages in order.

Run modes:

- `runall`
- `dfg_gen_only`
- `mapper_only`
- `sim_only`

XML mode details:

- accepts `.xml` as first argument
- defaults to `mapper_only`
- supports optional `--mem-alloc` file

## `context.py`

`RunnerConfig`:

- Loads config YAML and validates/normalizes keys.
- Supports legacy keys with deprecation warning.

`RunnerContext`:

- Computes all stage paths from repo root + benchmark path.
- Ensures required output directories exist.
- Carries stage parameters (`kernel`, `memory_mode`, banks, II, etc.).

## `dfg_generator.py`

Stage logic:

1. Clean previous local outputs in benchmark folder.
2. Build LLVM IR from C source using non-vectorized flags.
3. Run optimization pass chain.
4. Run Morpher DFG pass (`libdfggenPass.so`) to generate:
   - DFG XML
   - DFG DOT
   - mem allocation file
5. Render DOT to PDF.
6. Validate any AGI/iterator/SIMD manifests, remap trace metadata, and copy
   DFG XML and mem-alloc to mapper workspace.
7. If simulation enabled:
   - build instrumented binary
   - run binary to emit memory traces
   - copy trace/mem-alloc to simulator workspace

Error handling:

- Raises detailed error when mem-bank allocation fails.
- Raises detailed error for unsupported >2D array/pointer patterns.

## `mapper.py`

Stage logic:

1. Resolve architecture template path.
2. Build mapper command args from config.
3. If `memory_mode: auto`:
   - run `update_mem_alloc.py`
   - generate `<kernel>_arch_runtime.json`
4. Run `cgra_xml_mapper`.
5. Artifact simulation consumes the staged DFG and mapper JSON artifact;
   hardware binary emission is disabled. The empty mapper latency-named file
   remains only as a locator for its sibling mapping JSON.

## `artifact_simulator.py`

Stage logic:

1. Collect per-kernel trace files.
2. Select up to `max_test_samples`.
3. Run `artifact_simulator` for each selected trace (functional or event-driven per `validation_mode`).
4. Aggregate match/mismatch counts.

Current behavior expectation:

- Missing traces skip the stage with a warning (traces exist only when `enable_simulation: true` during DFG generation).

## Shared Helpers

`utils.py`:

- LLVM command assembly.
- DFG diagnostics and array-size scanning.

`exec.py`:

- typed subprocess wrapper.

`fs_ops.py`:

- glob-based file copy and cleanup helpers.
