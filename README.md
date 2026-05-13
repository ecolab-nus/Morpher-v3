# Morpher

Morpher is an open-source CGRA compilation and simulation framework. It takes a C kernel, generates a dataflow graph (DFG), maps it onto a configurable CGRA architecture, and optionally validates the mapping with cycle-accurate simulation.

## Supported Platform

Linux

## Features

- End-to-end flow from C kernel to mapped binary (`run_morpher.py`)
- XML input mode: skip DFG generation and feed a pre-generated DFG directly to the mapper
- Two memory modes: `auto` (generate architecture JSON at runtime) or `static` (use a pre-authored JSON)
- Auto-unroll search: sweeps candidate unroll factors and picks the one minimising effective II per iteration (`auto_unroll`)
- Architecture-driven SIMD packing: SIMD width and vector ops are inferred from the architecture JSON, then eligible unrolled DFG nodes are packed before mapping
- AGI pruning: removes address-generation instructions from the DFG, assuming an external scalar core handles loop addressing (`remove_agi`)
- Iteration pruning: removes loop-control and PHI-merge nodes from a SIMD-packed DFG, keeping only the core data kernel (`remove_iter`)
- Optional post-mapping data-layout solver with read-only data replication (`layout_solver`, `replicate_data`)
- Functional and event-driven simulation validation (`validation_mode`)
- Artifact simulator: validates SIMD-packed mappings against instrumented memory traces (`runall_artifact_sim`)
- Parallel benchmark automation with auto-discovery (`scripts/run_all_benchmarks.py`)
- Mapper regression suite runner with per-benchmark II bounds (`scripts/run_regression_benchmarks.py`)
- Architecture/config sweep runner (`scripts/run_arch_sweeps.py`)

## Repository Layout

```
run_morpher.py                    # single benchmark entry point
morpher_runner/                   # Python pipeline (DFG gen, mapper, simulator)
config/                           # run configurations
scripts/                          # batch runner and maintenance utilities
Morpher_DFG_Generator/            # LLVM-based DFG generator
  benchmarks/                     # benchmark source tree (auto-discovered)
    <suite>/<kernel>/<kernel>.c
Morpher_CGRA_Mapper/              # CGRA mapper (PathFinder + SA)
simulator/hycube_simulator/       # cycle-accurate simulator
dfgPartition/                     # experimental DFG partitioning (optional)
```

Additional documentation:

- [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md) — full feature and code walkthrough
- [`docs/DIRECTORY_LOGIC_GUIDE.md`](docs/DIRECTORY_LOGIC_GUIDE.md) — directory-by-directory logic
- [`docs/components/`](docs/components/) — component-level docs (runner pipeline, DFG generator, mapper, simulator)

## Requirements

- LLVM/Clang 18 toolchain with `clang`, `opt`, `llvm-link`, `llc`
- This branch targets LLVM 18 and is no longer treated as a multi-version compatibility branch
- CMake + Make
- Python 3 + pip
- `gcc-multilib` / `g++-multilib` (32-bit instrumentation path)
- Graphviz (`dot`) for DFG PDF generation

Example install on Ubuntu:

```bash
  sudo apt-get update
  sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-18 llvm-18 llvm-18-dev llvm-18-tools lld-18 \
  gcc-multilib g++-multilib graphviz
```

If your distro exposes versioned binaries, you can either place them on `PATH` or use:

- `MORPHER_LLVM_BIN_DIR=/path/to/llvm/bin`
- `MORPHER_LLVM_LIB_DIR=/path/to/llvm/lib` when the host has conflicting LLVM installs
- `MORPHER_LLVM_DIR=/path/to/lib/cmake/llvm` for `build_all.sh`
- `MORPHER_CMAKE_ARGS="..."` for extra CMake configure flags

## Setup

```bash
git clone https://github.com/pruhnuhv/Dataflow-arch-mapper.git
cd Dataflow-arch-mapper
python3 -m pip install --upgrade pip
python3 -m pip install -r python_requirements.txt
export MORPHER_LLVM_BIN_DIR=/usr/lib/llvm-18/bin
export MORPHER_LLVM_LIB_DIR=/usr/lib/llvm-18/lib
export MORPHER_LLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
bash build_all.sh
```

Sanity check:

```bash
ls Morpher_DFG_Generator/build/src/libdfggenPass.so
ls Morpher_CGRA_Mapper/build/src/cgra_xml_mapper
ls simulator/hycube_simulator/src/build/hycube_simulator
```

## Quick Start

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

General form:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode] [options]
```

Options:

| Option | Accepted values | Default | Description |
|--------|-----------------|---------|-------------|
| `--run-tag <tag>` | Non-empty tag string; sanitized to `[A-Za-z0-9._-]` | timestamp if omitted | Reuse a stable tag under `workspace/morpher_runs/<tag>/`; batch runners use this to avoid collisions. |
| `--output-root <path>` | Directory path | `generated` | For direct single-kernel C runs, write `result.csv` and copied `config.yaml` under `<root>/<suite>/<kernel>/<timestamp>/`. Ignored when `--run-tag` is supplied. |
| `--mem-alloc <path>` | Existing file path | none | XML input mode only: explicit mapper memory-allocation file. |

`runmode` values (default: `runall`):

| Mode | What runs |
|------|-----------|
| `runall` | DFG generation → mapper → simulator (if enabled) |
| `dfg_gen_only` | DFG generation only |
| `mapper_only` | Mapper only (requires existing DFG) |
| `sim_only` | Simulator only (requires existing mapping) |
| `artifact_sim_only` | Artifact simulator only (requires existing mapping + traces) |
| `runall_artifact_sim` | DFG generation → mapper → artifact simulator |

## XML Input Mode

Pass a `.xml` file as the first argument to skip DFG generation and map a pre-generated DFG:

```bash
python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]
```

Defaults in XML mode: function derived from filename, config `config/default.yaml`, runmode `mapper_only`.

Notes:
- XML mode supports `mapper_only`, `runall`, and `artifact_sim_only`.
- In `memory_mode: auto`, a mem-alloc file is required: place `<kernel>_mem_alloc.txt` or `<xml-stem>_mem_alloc.txt` beside the XML, or pass `--mem-alloc <path>`.
- Without a mem-alloc file, use a static config such as `config/xml_arch.yaml`.

```bash
python3 -u run_morpher.py gemm_systolic.xml
python3 -u run_morpher.py gemm_systolic.xml gemm_systolic config/xml_arch.yaml
```

## Configuration

Default config: `config/default.yaml`. Provided configs include:

| Config | Description |
|--------|-------------|
| `config/default.yaml` | Standard HyCUBE auto-memory config, `balanced` mapper preset, event-driven validation, AGI removal, no auto-unroll. |
| `config/scalar.yaml` | Fast scalar baseline with layout solver enabled, functional validation, no auto-unroll. |
| `config/scalar_no_solver.yaml` | Same scalar baseline with the layout solver disabled. |
| `config/scalar_edgemem.yaml` | Scalar flow on the edge-memory HyCUBE architecture. |
| `config/scalar_edgemem_unroll.yaml` | Edge-memory scalar flow with a fixed auto-unroll factor of 2. |
| `config/simd4_noiter.yaml` | 4×4 HyCUBE flow with AGI and iterator pruning; SIMD activates only if the selected architecture exposes SIMD-capable PEs. |
| `config/hycube_vectorization.yaml` | Vectorized architecture config with auto-unroll factor 3, layout solver enabled. |
| `config/regression_gemm_simd4_unroll.yaml` | Focused GEMM SIMD/unroll regression config. |
| `config/regression_8x8.yaml` | 8×8 HyCUBE regression config with simulation disabled. |
| `config/xml_arch.yaml` | Static architecture config useful for XML-only mapping. |

### Core knobs

| Key | Accepted values | Default | Description |
|-----|-----------------|---------|-------------|
| `memory_mode` | `auto`, `static` | inferred from legacy keys if omitted | `auto` generates a runtime architecture JSON from DFG memory allocation; `static` maps against `arch_template_json` directly. Legacy `json_arch` / `json_arch_before_memupdate` still parse with a warning. |
| `arch_template_json` | Bundled JSON filename, repo-relative path, or absolute path | required | Architecture JSON filename resolved from `Morpher_CGRA_Mapper/json_arch/` first, then repo root, unless absolute. |
| `mapper_subfolder` | Non-empty string | required | Output grouping under `workspace/morpher_runs/<tag>/mapper/<mapper_subfolder>/...`. |
| `mapper_preset` | `fast`, `balanced`, `quality`, `exhaust` | `balanced` | Mapper tuning preset. Explicit tuning keys override the preset with a warning. |
| `xdim`, `ydim` | Integers `>= 1` | inferred from architecture JSON | CGRA dimensions. Explicit values must match the JSON when dimensions are inferable. |
| `numberofbanks`, `banksize` | Integers `>= 1` | inferred from architecture JSON | SPM bank count and bank size. Explicit values must match architecture SPM metadata when present. In `memory_mode: auto`, DFG-time sizing can increase `banksize` for the runtime JSON. |
| `init_II`, `max_II` | Positive integers | preset/config dependent | Mapper II search bounds. |
| `ii_restarts` | Integer `>= 0` | preset dependent | Number of II restart attempts. |
| `skip_ii_estimation` | Boolean: `true` or `false` | preset dependent | Skip II bound analysis and start from `init_II`. |
| `multihop` | Integer `>= 1` | `4` | Maximum routing hop budget forwarded to the mapper. |
| `mapping_method` | Mapper integer mode; presets currently use `2` | preset dependent | Mapper algorithm selection flag. Treat as advanced unless changing mapper internals. |
| `backtrack_limit`, `max_mapper_iter` | Integers `>= 0` | preset dependent | Mapper search effort limits. |
| `print_dfg` | Boolean: `true` or `false` | preset dependent | Ask the mapper to print DFG details in logs. |
| `sa_seed`, `sa_restarts`, `sa_moves_per_temp`, `sa_candidate_topk` | Integers; restart/move/top-k values should be `>= 0` | preset dependent | Simulated-annealing mapper controls. |
| `sa_temp_max`, `sa_temp_min` | Numbers | preset dependent | Simulated-annealing temperature range. |
| `proximity_sort` | Boolean: `true` or `false` | preset dependent | Sort candidate PE placements by Manhattan distance to parent centroid; usually reduces routing pressure. |
| `node_pin` | Empty string or `node_idx:X:Y` with integer fields | empty | Debug override forcing one DFG node to one PE. |
| `enable_simulation` | Boolean: `true` or `false` | `true` | Generate traces and run validation in modes that include simulation. |
| `validation_mode` | `functional`, `event_driven`, `event-driven` | `functional` | Simulation check strategy. `event-driven` is normalized internally to `event_driven`. |
| `max_test_samples` | Integer `>= 0` | required in provided configs | Trace-generation limit and simulator sample cap. |
| `spm_mode` | `off`, `on` | `on` if omitted | Scratchpad memory mode passed to DFG generator/mapper. Static `spm_mode: on` cannot auto-grow banks. |
| `remove_agi` | Boolean: `true` or `false` | `false` | Remove address-generation nodes from the DFG; assumes a scalar/AGU path handles addresses. |
| `remove_iter` | Boolean: `true` or `false` | `false` | Remove loop-control and PHI-merge nodes from packed DFGs; assumes external iteration/control hardware. If SIMD packing is inactive, this knob is ignored with a warning. |
| `layout_solver` | Boolean: `true` or `false` | `false` | Enable post-mapping memory-layout solving and trace remapping. |
| `replicate_data` | Boolean: `true` or `false` | `false` | With `layout_solver: true`, allow read-only array replication across banks. |

Bundled architecture JSON options currently include:

```text
hycube_4regf.json
hycube_5x5_memedge_updatemem.json
hycube_8x8.json
hycube_edgePEMEMs.json
hycube_edgePEMEMs_2sides.json
hycube_edgePEMEMs_3sides.json
hycube_edgePEMEMs_3sides_noRouter.json
hycube_edgePEMEMs_4regf.json
hycube_edgePEMEMs_noRouter.json
hycube_original.json
hycube_original_RC.json
hycube_original_updatemem.json
hycube_original_updatemem8x8.json
hycube_original_updatemem_RC.json
hycube_vectorized.json
n2n.json
n2n_new.json
n2n_verbose.json
stdnoc.json
stdnoc_allmem.json
stdnoc_original.json
stdnoc_verbose.json
```

### Auto-unroll (`auto_unroll.*`)

For `runall` C-source runs, auto-unroll sweeps candidate factors, maps each candidate, and keeps the candidate with the best `II / unroll_factor` score. Factors are tried high-to-low after de-duplication.

| Key | Accepted values | Default | Description |
|-----|-----------------|---------|-------------|
| `auto_unroll` | Mapping or boolean | `{}` | A bare boolean is accepted as shorthand for `auto_unroll.enabled`. |
| `auto_unroll.enabled` | Boolean: `true` or `false` | `false` | Enable the search. |
| `auto_unroll.factors` | Non-empty list of integers `>= 1` | `[1, 2, 4, 8]` | Candidate factors to try; duplicate factors are ignored and candidates are run high-to-low. |
| `auto_unroll.fuse_innermost_loops` | Boolean: `true` or `false` | `true` | Before unrolling, attempt a conservative fusion/flattening of the annotated innermost loop with its direct parent when the nest is canonical and constant-trip. |
| `auto_unroll.stop_on_degradation` | Boolean: `true` or `false` | `true` | Stop searching early if effective II worsens. |

Removed keys: `auto_unroll.remainder_policy` and `auto_unroll.outer_factor` now raise errors. Remainders are always preserved; size benchmarks so the mapped trip counts are valid multiples of the desired unroll/SIMD factor.

### Architecture-Driven SIMD

SIMD is no longer configured through top-level `simd.*` YAML knobs. The runner infers SIMD capability from `arch_template_json`:

- Compute PE descriptions with `SIMD_WIDTH > 1` and vector FU ops enable SIMD packing.
- All SIMD compute PEs must expose one uniform SIMD width.
- The packer activates only when `unroll_factor >= inferred SIMD width`; narrower unroll candidates map as scalar DFGs on the same architecture.
- `SCALAR_BROADCAST` and vector op names are also inferred from the architecture JSON.
- Deprecated top-level `simd` config data is ignored by the current runner except for the deprecation warning.

The SIMD packer rewrites eligible scalar DFG XML nodes into vector nodes and can prune scalar lane outputs and iterator/control nodes according to `remove_agi` and `remove_iter`.

### Mapper Presets

| Preset | Use case |
|--------|----------|
| `fast` | Short runs and smoke tests. Lower backtracking, mapper iterations, and SA effort. |
| `balanced` | Default starting point. Moderate search effort and DFG logging. |
| `quality` | Higher mapping effort, more SA restarts/moves, larger II search range. |
| `exhaust` | Largest built-in search budget; useful for difficult kernels, but slow. |

Advanced tuning keys available for overriding presets: `mapping_method`, `backtrack_limit`, `max_mapper_iter`, `max_II`, `ii_restarts`, `skip_ii_estimation`, `print_dfg`, `sa_seed`, `sa_restarts`, `sa_moves_per_temp`, `sa_temp_max`, `sa_temp_min`, `sa_candidate_topk`, and `proximity_sort`.

### Environment toggles

| Variable | Accepted values | Description |
|----------|-----------------|-------------|
| `MORPHER_LLVM_BIN_DIR` | Directory path | Resolve `clang`, `opt`, `llvm-link`, and `llc` from a specific LLVM install. |
| `MORPHER_LLVM_LIB_DIR` | Directory path | Make LLVM runtime libraries visible to LLVM tools and plugins. |
| `MORPHER_LLVM_SUFFIX` | Tool suffix string, such as `-18` | Append a suffix when resolving LLVM tools from `PATH`. |
| `MORPHER_LLVM_DIR` | CMake LLVM package directory path | CMake LLVM package path for `build_all.sh`. |
| `MORPHER_CMAKE_ARGS` | Space-separated CMake flags | Extra CMake configure flags for `build_all.sh`. |
| `MORPHER_VERBOSE_CMDS` | Truthy unless empty, `0`, `false`, `off`, or `no` | Print underlying shell commands in mapper/simulator stages. |
| `MORPHER_DUMP_VERBOSE_JSON` | Any non-empty value | Dump expanded mapper architecture JSON for debugging. |
| `MORPHER_PRINT_DFG` | Any non-empty value | Force mapper-side DFG printing. |
| `MORPHER_MAX_TRACES` | Integer trace cap | Cap instrumented trace generation; normally set internally from `max_test_samples`. |
| `ARTIFACT_SIM_DEBUG` | Any non-empty value | Enable artifact simulator debug output. |

Additional code-oriented docs: [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md).

## Outputs

Stage artifacts live under tagged workspaces:

```text
workspace/morpher_runs/<tag>/
  dfg/<suite>/<kernel>/
  mapper/<mapper_subfolder>/<suite>/<kernel>/
  simulator/<suite>/<kernel>/
```

Direct single-kernel C runs without `--run-tag` also create a summary folder:

```text
generated/<suite>/<kernel>/<timestamp>/
  result.csv
  config.yaml
```

Batch, regression, and sweep scripts also write user-facing summaries under `generated/` unless configured otherwise. Mapper outputs include `result.txt`, `*.mappingwithlatency.txt`, `*.bin` when binary emission is enabled, and `<kernel>_arch_runtime.json` in `memory_mode: auto`.

## Batch Benchmark Runs

Run all auto-discovered benchmarks in parallel:

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
```

Useful flags:

```bash
# Select config and run mode
python3 scripts/run_all_benchmarks.py --config config/scalar.yaml --runmode runall

# Preview what would run without executing
python3 scripts/run_all_benchmarks.py --dry-run

# Filter by regex on the benchmark path (repeatable, OR semantics)
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --jobs 8
python3 scripts/run_all_benchmarks.py --filter 'fir' --filter 'conv2'

# Limit to the first N benchmarks after filtering
python3 scripts/run_all_benchmarks.py --max-benches 10

# Per-benchmark timeout and custom output path
python3 scripts/run_all_benchmarks.py --timeout-sec 120 --output-csv results.csv

# Change where per-kernel run folders are written
python3 scripts/run_all_benchmarks.py --output-root generated/my_batch

# Prefix workspace run tags when multiple batches may run concurrently
python3 scripts/run_all_benchmarks.py --run-tag-prefix experiment_a
```

By default the summary CSV is `benchmark_run_results.csv`. Per-kernel logs and per-kernel `result.csv` files are written under `<output-root>/<suite>/<kernel>/<timestamp>/`, where `--output-root` defaults to `generated`.

Batch runner options:

| Option | Accepted values | Default |
|--------|-----------------|---------|
| `--repo-root` | Repository directory path | Parent of `scripts/run_all_benchmarks.py` |
| `--config` | YAML config path relative to `--repo-root` | `config/default.yaml` |
| `--runmode` | `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`, `artifact_sim_only`, `runall_artifact_sim` | `runall` |
| `--jobs` | Integer `>= 1` | `min(8, CPU count)` |
| `--timeout-sec` | Integer seconds; `0` means no timeout | `0` |
| `--output-root` | Directory path relative to `--repo-root`, unless absolute | `generated` |
| `--filter` | Python regex on benchmark source path; repeatable with OR semantics | none |
| `--max-benches` | Integer; `0` means all selected benchmarks | `0` |
| `--output-csv` | CSV path relative to `--repo-root`, unless absolute | `benchmark_run_results.csv` |
| `--dry-run` | Flag | off |
| `--run-tag-prefix` | String prefix sanitized to `[A-Za-z0-9._-]` | empty |

## Adding New Benchmarks

Benchmarks are auto-discovered — no registration step required. Drop a source file following this layout:

```
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

Rules enforced at discovery time:

- The `.c` filename must exactly match its containing folder name.
- `<kernel>` must be a valid C identifier (`[A-Za-z_][A-Za-z0-9_]*`).
- The path must be exactly three levels deep relative to `benchmarks/`: `<suite>/<kernel>/<kernel>.c`. Extra nesting or files at wrong depths are rejected with a clear error.
- No duplicate entries are allowed.

The function name passed to the DFG generator is derived from the file stem, so the top-level C function in `<kernel>.c` must be named `<kernel>`.

**Example** — add a new `gemv` kernel to the `polybench` suite:

```
Morpher_DFG_Generator/benchmarks/polybench/gemv/gemv.c
```

Verify discovery before running:

```bash
python3 scripts/run_all_benchmarks.py --dry-run --filter 'gemv'
```

Run just the new kernel:

```bash
python3 scripts/run_all_benchmarks.py --filter 'polybench/gemv' --jobs 1
# or directly:
python3 -u run_morpher.py polybench/gemv/gemv.c gemv
```

To include it in a regression suite, add an entry under the relevant suite in `scripts/regression_benchmarks.yaml`:

```yaml
- benchmark: polybench/gemv/gemv.c
  function: gemv
  max_ii: 6
```

## Mapper Regression Suites

Run the default 4×4 regression suite (used for pre-commit checks):

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_4x4
```

Run the focused 4×4 configuration suites:

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_4x4_scalar_agi
python3 scripts/run_regression_benchmarks.py --suite ci_4x4_simd4_noiter
python3 scripts/run_regression_benchmarks.py --suite ci_4x4_gemm_simd4_unroll
```

Run the 8×8 suite (slower; not part of default CI):

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_8x8
```

Suite definitions, per-suite configs, and per-benchmark maximum II bounds live in `scripts/regression_benchmarks.yaml`. Regression outputs are written under `generated/morpher_runs/regression/<suite>/`. The runner fails if any benchmark exceeds its `max_ii` bound.

Useful regression flags:

```bash
python3 scripts/run_regression_benchmarks.py --dry-run
python3 scripts/run_regression_benchmarks.py --config config/scalar.yaml
python3 scripts/run_regression_benchmarks.py --runmode mapper_only
python3 scripts/run_regression_benchmarks.py --jobs 1 --timeout-sec 300
python3 scripts/run_regression_benchmarks.py --output-dir generated/morpher_runs/regression/custom
```

Available regression suites from `scripts/regression_benchmarks.yaml`:

```text
ci_4x4
ci_4x4_scalar_agi
ci_4x4_simd4_noiter
ci_4x4_gemm_simd4_unroll
ci_8x8
```

Regression runner options:

| Option | Accepted values | Default |
|--------|-----------------|---------|
| `--repo-root` | Repository directory path | Parent of `scripts/run_regression_benchmarks.py` |
| `--manifest` | Regression manifest YAML path | `scripts/regression_benchmarks.yaml` |
| `--suite` | Suite name from the manifest | `ci_4x4` |
| `--config` | YAML config path | Suite `config` |
| `--runmode` | `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`, `artifact_sim_only`, `runall_artifact_sim` | Suite `runmode`, usually `runall` |
| `--jobs` | Integer `>= 1` | Suite `jobs` |
| `--timeout-sec` | Integer seconds; `0` means no timeout | Suite `timeout_sec` |
| `--output-dir` | Directory path relative to `--repo-root`, unless absolute | `generated/morpher_runs/regression/<suite>` |
| `--dry-run` | Flag | off |

## Architecture Sweeps

`scripts/run_arch_sweeps.py` expands a YAML manifest suite into architecture/config experiments, writes temporary configs, runs each experiment through `scripts/run_all_benchmarks.py`, and emits `summary.csv` plus `combined_results.csv`.

```bash
python3 scripts/run_arch_sweeps.py --suite edge_mem_arch_sweep --dry-run
python3 scripts/run_arch_sweeps.py --suite edge_mem_arch_sweep --experiment-jobs 2
```

Useful sweep flags:

```bash
python3 scripts/run_arch_sweeps.py --manifest scripts/arch_sweeps.yaml
python3 scripts/run_arch_sweeps.py --experiment-filter 'unroll_simd4'
python3 scripts/run_arch_sweeps.py --benchmark-filter '^polybench/' --max-benches 5
python3 scripts/run_arch_sweeps.py --jobs 4 --timeout-sec 600 --runmode runall
python3 scripts/run_arch_sweeps.py --output-dir generated/morpher_runs/arch_sweeps/custom
```

Manifest knobs live in `scripts/arch_sweeps.yaml`: suite-level `base_config`, `runmode`, `jobs`, `timeout_sec`, `filters`, `max_benches`, `common_overrides`, `arches`, and per-variant `name`, `description`, `base_config`, `overrides`. The sweep script deep-merges config overrides and sets `arch_template_json` for each architecture.

Available sweep suites from `scripts/arch_sweeps.yaml`:

```text
edge_mem_arch_sweep
```

Sweep runner options:

| Option | Accepted values | Default |
|--------|-----------------|---------|
| `--repo-root` | Repository directory path | Parent of `scripts/run_arch_sweeps.py` |
| `--manifest` | Sweep manifest YAML path | `scripts/arch_sweeps.yaml` |
| `--suite` | Suite name from the manifest | `edge_mem_arch_sweep` |
| `--output-dir` | Directory path relative to `--repo-root`, unless absolute | `generated/morpher_runs/arch_sweeps/<suite>` |
| `--experiment-filter` | Python regex on expanded experiment names; repeatable with OR semantics | none |
| `--benchmark-filter` | Python regex passed to `run_all_benchmarks.py`; repeatable with OR semantics | Suite `filters` |
| `--max-benches` | Integer; overrides suite `max_benches` | Suite `max_benches`, or `0` |
| `--jobs` | Integer `>= 1`; benchmark workers per experiment | Suite `jobs` |
| `--experiment-jobs` | Integer `>= 1`; concurrent experiments | `1` |
| `--timeout-sec` | Integer seconds; overrides suite `timeout_sec` | Suite `timeout_sec`, or `0` |
| `--runmode` | `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`, `artifact_sim_only`, `runall_artifact_sim` | Suite `runmode`, or `mapper_only` |
| `--dry-run` | Flag | off |

Sweep manifest fields:

| Field | Accepted values |
|-------|-----------------|
| `suites.<name>.base_config` | YAML config path |
| `suites.<name>.runmode` | `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`, `artifact_sim_only`, `runall_artifact_sim` |
| `suites.<name>.jobs` | Integer `>= 1` |
| `suites.<name>.timeout_sec` | Integer seconds; `0` means no timeout |
| `suites.<name>.filters` | List of Python regex strings |
| `suites.<name>.max_benches` | Integer; `0` means all selected benchmarks |
| `suites.<name>.common_overrides` | YAML mapping merged into every generated config |
| `suites.<name>.arches` | Non-empty list of architecture JSON filenames or paths |
| `suites.<name>.variants[].name` | Non-empty unique string |
| `suites.<name>.variants[].description` | String |
| `suites.<name>.variants[].base_config` | YAML config path, optional |
| `suites.<name>.variants[].overrides` | YAML mapping merged over the selected base config |

## Maintenance

Clean workspace outputs:

```bash
bash scripts/clean_workspace.sh
bash scripts/clean_workspace.sh --dry-run
```

This removes generated non-`.c` files from `Morpher_DFG_Generator/benchmarks`, clears `Morpher_CGRA_Mapper/benchmarks` while preserving `.gitkeep`, and removes `workspace/morpher_runs`.

Verify that no generated artifacts are tracked by git:

```bash
bash scripts/check_no_generated_tracked.sh
```

## Cite

**Reference Publication**

- ICCAD 2025: [Building an Open CGRA Ecosystem for Agile Innovation](https://arxiv.org/abs/2508.19090)

**Additional Publications**

- HotChips 2024: [PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications](https://ieeexplore.ieee.org/abstract/document/10665106)
- WOSET 2022: [Morpher: An Open-Source Integrated Compilation and Simulation Framework for CGRA](https://www.comp.nus.edu.sg/~tulika/WOSET_MORPHER_2022.pdf)
- DAC 2022: [PANORAMA: Divide-and-Conquer Approach for Mapping Complex Loop Kernels on CGRA](https://www.comp.nus.edu.sg/~tulika/DAC22.pdf)
- HPCA 2022: [LISA: Graph Neural Network based Portable Mapping on Spatial Accelerators](https://www.comp.nus.edu.sg/~tulika/HPCA_LISA_2022.pdf)
- ASPLOS 2022: [REVAMP: A Systematic Framework for Heterogeneous CGRA Realization](https://www.comp.nus.edu.sg/~tulika/asplos22.pdf)
- TCAD 2022: [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap-TCAD.pdf)
- TCAD 2022: [ChordMap: Automated Mapping of Streaming Applications onto CGRA](https://ieeexplore.ieee.org/document/9351547)
- Book Chapter: [Coarse-Grained Reconfigurable Array (CGRA)](https://www.comp.nus.edu.sg/~tulika/CGRA-Survey.pdf)
