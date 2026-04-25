# Morpher

Morpher is an open-source CGRA compilation and simulation framework. It takes a C kernel, generates a dataflow graph (DFG), maps it onto a configurable CGRA architecture, and optionally validates the mapping with cycle-accurate simulation.

## Supported Platform

Linux

## Features

- End-to-end flow from C kernel to mapped binary (`run_morpher.py`)
- XML input mode: skip DFG generation and feed a pre-generated DFG directly to the mapper
- Two memory modes: `auto` (generate architecture JSON at runtime) or `static` (use a pre-authored JSON)
- Auto-unroll search: sweeps candidate unroll factors and picks the one minimising effective II per iteration (`auto_unroll`)
- SIMD vectorization: packs scalar DFG nodes into multi-lane SIMD nodes before mapping, with configurable reduction lowering (`simd`)
- AGI pruning: removes address-generation instructions from the DFG, assuming an external scalar core handles loop addressing (`remove_agi`)
- Iteration pruning: removes loop-control and PHI-merge nodes from a SIMD-packed DFG, keeping only the core data kernel (`remove_iter`)
- Functional and event-driven simulation validation (`validation_mode`)
- Artifact simulator: validates SIMD-packed mappings against instrumented memory traces (`runall_artifact_sim`)
- Parallel benchmark automation with auto-discovery (`scripts/run_all_benchmarks.py`)
- Mapper regression suite runner with per-benchmark II bounds (`scripts/run_regression_benchmarks.py`)

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
hycube_simulator/                 # cycle-accurate simulator
dfgPartition/                     # experimental DFG partitioning (optional)
```

Additional documentation:

- [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md) — full feature and code walkthrough
- [`docs/DIRECTORY_LOGIC_GUIDE.md`](docs/DIRECTORY_LOGIC_GUIDE.md) — directory-by-directory logic
- [`docs/components/`](docs/components/) — component-level docs (runner pipeline, DFG generator, mapper, simulator)

## Requirements

- LLVM/Clang 10.0.0 tools on `PATH`: `clang`, `opt`, `llvm-link`, `llc`
- CMake + Make
- Python 3 + pip
- `gcc-multilib` / `g++-multilib` (32-bit instrumentation path)
- Graphviz (`dot`) for DFG PDF generation

Example install on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-10 llvm-10 llvm-10-dev llvm-10-tools \
  gcc-multilib g++-multilib graphviz
```

If your distro exposes versioned binaries (e.g. `clang-10`), ensure unversioned names (`clang`, `opt`, `llvm-link`, `llc`) are on your `PATH`.

## Setup

```bash
git clone https://github.com/pruhnuhv/Dataflow-arch-mapper.git
cd Dataflow-arch-mapper
python3 -m pip install --upgrade pip
python3 -m pip install -r python_requirements.txt
bash build_all.sh
```

Sanity check:

```bash
ls Morpher_DFG_Generator/build/src/libdfggenPass.so
ls Morpher_CGRA_Mapper/build/src/cgra_xml_mapper
ls hycube_simulator/src/build/hycube_simulator
```

## Quick Start

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

General form:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

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

Defaults in XML mode: function derived from filename, config `config/default_config.yaml`, runmode `mapper_only`.

Notes:
- Only `mapper_only` and `runall` are supported in XML mode.
- In `memory_mode: auto`, a mem-alloc file is required: place `<kernel>_mem_alloc.txt` beside the XML or pass `--mem-alloc <path>`. Without one, use `config/xml_mapper_static.yaml`.

```bash
python3 -u run_morpher.py gemm_systolic.xml
python3 -u run_morpher.py gemm_systolic.xml gemm_systolic config/xml_mapper_static.yaml
```

## Configuration

Default config: `config/default.yaml`. Several preset configs are provided:

| Config | Description |
|--------|-------------|
| `config/default.yaml` | 4×4 fabric, SIMD 8-lane, auto-unroll enabled — the standard starting point |
| `config/scalar.yaml` | 4×4 fabric, no SIMD, no unroll — baseline scalar flow |
| `config/simd4_noiter.yaml` | 4×4, SIMD 4-lane with iteration pruning — for kernels run on an external scalar control path |
| `config/regression_8x8.yaml` | 8×8 fabric, simulation off — used by the `ci_8x8` regression suite |

### Core knobs

| Key | Description |
|-----|-------------|
| `memory_mode` | `auto` or `static` |
| `arch_template_json` | Architecture template (resolved from `Morpher_CGRA_Mapper/json_arch/` unless absolute) |
| `ydim`, `xdim` | CGRA grid dimensions |
| `mapper_subfolder` | Output grouping under the mapper benchmark directory |
| `enable_simulation` | Whether to run instrumentation + simulation |
| `validation_mode` | Simulation check strategy: `functional` (data correctness) or `event-driven` (cycle-accurate) |
| `spm_mode` | Scratchpad memory mode: `on` or `off` |
| `numberofbanks`, `banksize` | Memory model for DFG pass and mapper |
| `max_test_samples` | Trace generation cap and simulator sample cap |
| `init_II`, `max_II`, `ii_restarts`, `skip_ii_estimation` | Mapper II search behavior |
| `mapping_method` | Mapper strategy selection |
| `proximity_sort` | Sort candidate PE placements by Manhattan distance to parent centroid (reduces routing pressure) |
| `node_pin` | Force a specific DFG node to a PE: `idx:X:Y` — useful for debugging placement |
| `remove_agi` | Remove address-generation (GEP/iterator) nodes from the DFG; assumes a scalar core handles loop addressing externally |
| `remove_iter` | Remove loop-control and PHI-merge nodes from a SIMD-packed DFG, keeping only the core data kernel; requires `simd.enabled: true` |

### Auto-unroll (`auto_unroll.*`)

Sweeps a list of unroll factors and runs the full DFG-generation + mapping pipeline for each. The factor with the best `II / unroll_factor` score (lowest effective II per original iteration) is kept.

| Key | Default | Description |
|-----|---------|-------------|
| `auto_unroll.enabled` | `true` | Enable the search |
| `auto_unroll.factors` | `[1, 2, 4, 8]` | Candidate factors to try, in order |
| `auto_unroll.remainder_policy` | `preserve` | How to handle loops whose trip count is not a multiple of the unroll factor: `preserve` emits a remainder loop; `assume-multiple` skips it (faster, unsafe if not guaranteed) |
| `auto_unroll.stop_on_degradation` | `true` | Stop searching early if effective II worsens |

When `simd.enabled: true`, factors smaller than `simd.lanes` are automatically skipped because the SIMD packer requires at least one full vector width of unrolled iterations.

### SIMD vectorization (`simd.*`)

After DFG generation, the SIMD packer rewrites the DFG XML: it identifies groups of scalar nodes that carry the SIMD lane annotation inserted by the unroll pass and merges them into a single multi-lane SIMD node. This reduces DFG size and allows the mapper to treat the packed node as a single placement unit.

| Key | Default | Description |
|-----|---------|-------------|
| `simd.enabled` | `true` | Enable SIMD packing |
| `simd.lanes` | `8` | Number of scalar operations packed into one SIMD node |
| `simd.pack_load_store` | `true` | Also pack memory (LOAD/STORE) nodes, not just compute nodes |
| `simd.reduction_policy` | `packed-psum` | How associative reduction chains (e.g. loop-carried sums) are lowered when unrolling — see below |

**`reduction_policy` options** — both apply only to associative reduction patterns (add/OR/AND/XOR loop-carried accumulators):

- `tree`: performs a balanced binary-tree reduction inside the loop body. Each group of `lanes` terms is reduced in ceil(log₂(lanes)) add-depth steps, then feeds back into a partial-sum accumulator. Shorter dependency chains; works well when the number of reduction terms is small.
- `packed-psum` *(default)*: keeps one independent running accumulator per SIMD lane throughout the loop body; the cross-lane horizontal add is deferred to a single step after the loop exits. This is the SIMD-friendly form — all lanes are independent inside the loop so the packer can merge their accumulators into a single SIMD node with no intra-node data dependency.

### Environment toggles

- `MORPHER_VERBOSE_CMDS=1` — print underlying shell commands in mapper/simulator stages
- `MORPHER_DUMP_VERBOSE_JSON=1` — dump expanded mapper architecture JSON for debugging

Full config reference: [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md).

## Batch Benchmark Runs

Run all auto-discovered benchmarks in parallel:

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
```

Useful flags:

```bash
# Preview what would run without executing
python3 scripts/run_all_benchmarks.py --dry-run

# Filter by regex on the benchmark path (repeatable, OR semantics)
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --jobs 8
python3 scripts/run_all_benchmarks.py --filter 'fir' --filter 'conv2'

# Limit to the first N benchmarks after filtering
python3 scripts/run_all_benchmarks.py --max-benches 10

# Per-benchmark timeout and custom output path
python3 scripts/run_all_benchmarks.py --timeout-sec 120 --output-csv results.csv
```

Results are written to `benchmark_run_results.csv` and per-benchmark logs to `benchmark_run_logs/`.

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

Run the 4×4 regression suite (used for pre-commit checks):

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_4x4
```

Run the 8×8 suite (slower; not part of default CI):

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_8x8
```

Suite definitions and per-benchmark maximum II bounds live in `scripts/regression_benchmarks.yaml`. The runner fails if any benchmark exceeds its `max_ii` bound.

## Maintenance

Clean workspace outputs:

```bash
bash scripts/clean_workspace.sh
bash scripts/clean_workspace.sh --dry-run
```

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
