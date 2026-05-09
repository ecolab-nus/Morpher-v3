# Morpher Features and Code Guide

This guide explains what each part of the repository does in plain language.

## 1) What Morpher Does

Morpher converts a C loop kernel into a DFG, maps that DFG to a CGRA architecture, and can validate the mapping in a cycle-accurate simulator.

Pipeline stages:

1. DFG generation (`Morpher_DFG_Generator`)
2. Mapping (`Morpher_CGRA_Mapper`)
3. Simulation (`hycube_simulator`, optional)

The Python entrypoint `run_morpher.py` orchestrates these stages.

## 2) Core User Features

- Single-kernel run from C source
- XML input mode for pre-generated DFGs
- Config-driven run modes (`runall`, `dfg_gen_only`, `mapper_only`, `sim_only`)
- Dynamic memory-layout generation (`memory_mode: auto`)
- Static architecture mapping (`memory_mode: static`)
- Optional simulation (`enable_simulation`)
- SIMD-capable mapping even when the selected unroll factor is narrower than the SIMD lane count
- Batch benchmark execution with parallel workers
- Cleanup and policy-check scripts for generated artifacts

## 3) Inputs and Outputs

### C Input Mode

Command shape:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

Expected source location:

```text
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

### XML Input Mode

Command shape:

```bash
python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]
```

Rules:

- XML mode supports only `mapper_only` and `runall`
- `function` defaults to filename-derived kernel name
- If `memory_mode: auto`, a mem-alloc file is required

### Main Generated Files

DFG generator folder (`Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/`):

- `<kernel>_PartPredDFG.xml`
- `<kernel>_PartPredDFG.dot` and `.pdf`
- `<kernel>_mem_alloc.txt`
- `memtraces/<kernel>_trace_*.txt` (if simulation enabled)

Mapper folder (`Morpher_CGRA_Mapper/benchmarks/<mapper_subfolder>/<suite>/<kernel>/`):

- `<kernel>_PartPredDFG.xml` (staged input)
- `<kernel>_mem_alloc.txt` (staged input)
- `<kernel>_arch_runtime.json` (in `memory_mode: auto`)
- `*.bin` mapping binaries
- `*.mappingwithlatency.txt`
- `result.txt` (mapper summary)

Simulator folder (`hycube_simulator/benchmarks/<suite>/<kernel>/`):

- staged `*.bin`
- staged `<kernel>_mem_alloc.txt`
- `sim_result.txt`

## 4) Code Map (What Each File/Folder Owns)

## `run_morpher.py`

- Parses CLI arguments and validates modes
- Supports C mode and XML mode
- Builds shared context and calls stage runners

## `morpher_runner/`

- `context.py`: loads YAML config and resolves paths
- `dfg_generator.py`: LLVM compile/opt, DFG pass run, instrumentation, trace generation
- `mapper.py`: architecture resolution, optional mem-layout update, mapper execution
- `simulator.py`: trace sampling and simulator execution loop
- `utils.py`: helper commands and DFG failure diagnostics
- `exec.py`: typed subprocess wrapper
- `fs_ops.py`: file copy/remove helpers

## `config/`

- `default.yaml`: default run config
- `xml_mapper_static.yaml`: static-mode config useful for XML-only mapping

## `scripts/`

- `run_all_benchmarks.py`: benchmark discovery + filtered parallel runs + CSV/log output
- `clean_generated.sh`: removes generated artifacts
- `check_no_generated_tracked.sh`: CI-style policy check for tracked generated files
- `bin_to_trace.py`, `hycube_config.py`: legacy/utility trace helpers

## `Morpher_DFG_Generator/` (C++)

- LLVM pass infrastructure and DFG generation logic
- Key areas:
  - `src/dfggenPass.cpp`
  - `src/common/`
  - `src/dfg/`
  - `src/instrumentation/`

## `Morpher_CGRA_Mapper/` (C++)

- DFG-to-architecture mapping engine
- Key areas:
  - `src/CGRA_xml_compiler.cpp` (main binary)
  - `src/mapper/` (PathFinder, Heuristic, SimulatedAnnealing)
  - `src/arch/`, `src/dfg/`
- `update_mem_alloc.py`: injects mem-allocation into architecture template for auto mode

## `hycube_simulator/` (C++)

- Cycle-accurate simulator
- Key files:
  - `src/hycube_sim.cpp`
  - `src/CGRA.cpp`
  - `src/CGRATile.cpp`

## `dfgPartition/`

- Optional partitioning workflow for large DFGs (separate experimental utility set)

## 5) Config Keys (Plain-Language Reference)

All keys are loaded in `morpher_runner/context.py`.

| Key | Purpose |
|---|---|
| `memory_mode` | `auto` generates runtime JSON from mem-alloc, `static` uses template directly |
| `arch_template_json` | Architecture JSON filename (or absolute path) |
| `mapper_subfolder` | Subdirectory name under mapper benchmark outputs |
| `init_II` | Initial initiation interval for mapper |
| `xdim`, `ydim` | CGRA dimensions passed to mapper/simulator |
| `numberofbanks`, `banksize` | Memory model parameters used across stages |
| `max_test_samples` | Trace generation/sample cap for simulation |
| `mapping_method` | Mapper algorithm selection flag |
| `backtrack_limit` | Backtracking limit for mapping search |
| `max_mapper_iter` | Max mapper iterations |
| `max_II` | Maximum II limit |
| `ii_restarts` | II restart attempts |
| `skip_ii_estimation` | Skip II bound analysis and start from `init_II` |
| `print_dfg` | Print DFG details in mapper logs |
| `sa_seed` | Seed for SA mapper mode |
| `sa_restarts` | Number of SA restarts |
| `sa_moves_per_temp` | SA moves per temperature |
| `sa_temp_max`, `sa_temp_min` | SA temperature range |
| `sa_candidate_topk` | SA candidate limit |
| `enable_simulation` | Run instrumentation + simulator when true |
| `spm_mode` | Mapper SPM handling mode |
| `llvm_debug_type` | LLVM debug mode field consumed by config loader |
| `remove_agi` | Pass-through flag for DFG generation (`-remove-agi`) |

Legacy keys still accepted (with warning):

- `json_arch`
- `json_arch_before_memupdate`

## 6) Useful Environment Variables

- `MORPHER_VERBOSE_CMDS=1`: prints command lines for mapper/simulator stages
- `MORPHER_MAX_TRACES=<N>`: set internally from `max_test_samples` during instrumentation
- `MORPHER_DUMP_VERBOSE_JSON=1`: extra mapper JSON debugging output

## 7) Batch Runner Behavior

`scripts/run_all_benchmarks.py`:

- Auto-discovers benchmark C files in required normalized layout
- Supports regex filters and benchmark count limits
- Runs jobs in parallel (`--jobs`)
- Stores per-benchmark logs and a summary CSV
- Extracts II and sim mismatch info from logs/results

## 8) Maintenance and Policy

- `scripts/clean_generated.sh` keeps benchmark trees clean while preserving `.gitkeep`
- `scripts/check_no_generated_tracked.sh` enforces tracked-file policy

## 9) Typical Workflow

1. Build once (`bash build_all.sh`)
2. Run a benchmark (`python3 -u run_morpher.py ...`)
3. Inspect mapper outputs (`*.mappingwithlatency.txt`, `result.txt`, `*.bin`)
4. Run batch regression with `scripts/run_all_benchmarks.py`
5. Clean generated artifacts before commit
