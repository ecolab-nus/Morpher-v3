# Morpher

Morpher is an open-source CGRA compilation and simulation framework. It takes a C kernel, generates a dataflow graph (DFG), maps it onto a configurable CGRA architecture, and validates the mapping with a trace-driven simulator.

Supported platform: Linux.

## How a run works

```
C kernel ──► DFG generation ──► DFG transforms ──► mapping ──► validation
             (LLVM pass)        (unroll search,     (PathFinder   (functional or
                                 AGI/iterator        + SA)         cycle-accurate,
                                 removal, SIMD,                    against recorded
                                 systolic, ...)                    memory traces)
```

The number that matters at the end is the **initiation interval (II)**: a new loop iteration starts every II cycles, so lower is better. When the unroll search is on, candidates are scored by **`II / unroll_factor`** — useful source iterations per cycle — because an unrolled graph does more work per II. Validation then confirms the mapped graph still computes the right values (`Mismatches::0`).

Everything you configure pulls on one of four levers:

1. **The architecture** — what hardware exists to map onto (grid size, memory PEs, SIMD width, special capabilities). Sets the floor on achievable II.
2. **Mapper effort** — how hard the search tries before giving up and raising II. Trades compile time for mapping quality.
3. **The graph that gets mapped** — transforms that shrink or reshape the DFG before mapping. Usually the biggest II wins come from here, not from mapper effort.
4. **Validation** — how thoroughly the result is checked.

Each lever has a section under [What you control](#what-you-control).

## Setup

Requirements:

- LLVM/Clang 18 toolchain with `clang`, `opt`, `llvm-link`, `llc` (this branch targets LLVM 18 only)
- CMake + Make, Python 3 + pip
- `gcc-multilib` / `g++-multilib` (32-bit instrumentation path)
- Graphviz (`dot`) for DFG PDF generation

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-18 llvm-18 llvm-18-dev llvm-18-tools lld-18 \
  gcc-multilib g++-multilib graphviz
```

Build:

```bash
git clone https://github.com/pruhnuhv/Dataflow-arch-mapper.git
cd Dataflow-arch-mapper
python3 -m pip install -r python_requirements.txt
export MORPHER_LLVM_BIN_DIR=/usr/lib/llvm-18/bin
export MORPHER_LLVM_LIB_DIR=/usr/lib/llvm-18/lib
export MORPHER_LLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
bash build_all.sh
```

Sanity check that all three binaries exist:

```bash
ls Morpher_DFG_Generator/build/src/libdfggenPass.so \
   Morpher_CGRA_Mapper/build/src/cgra_xml_mapper \
   simulator/artifact_simulator/build/src/artifact_simulator
```

If your distro exposes only versioned LLVM binaries, `MORPHER_LLVM_SUFFIX=-18` resolves suffixed tools from `PATH`; `MORPHER_CMAKE_ARGS` passes extra CMake flags to `build_all.sh`.

## Quick start

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

General form:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode] [options]
```

- `config_path` defaults to `config/default.yaml`
- `runmode` (default `runall`): `runall` = DFG gen → map → simulate; `dfg_gen_only`, `mapper_only`, `sim_only` run one stage against existing artifacts
- `--run-tag <tag>` reuses a stable workspace folder instead of a timestamp; `--output-root <path>` redirects the `result.csv` summary folder (default `generated/`)

Where results land:

```text
generated/<suite>/<kernel>/<timestamp>/result.csv     # II, status, timings — read this first
workspace/morpher_runs/<tag>/dfg/...                  # DFG XML, .pdf, memory traces
workspace/morpher_runs/<tag>/mapper/...               # result.txt, *.mappingwithlatency.txt, runtime arch JSON
workspace/morpher_runs/<tag>/simulator/...            # validation output
```

When the unroll search runs, `auto_unroll_results.csv` (next to `result.csv`) lists every candidate tried with its factors, kind, and score — useful for seeing *why* the winner won.

## What you control

### 1. The architecture (`arch_template_json`)

The architecture JSON defines the PE grid, which PEs can access memory, per-PE supported ops, SIMD width, and declared capabilities. It is the strongest determinant of what II is achievable. Names resolve against `Morpher_CGRA_Mapper/json_arch/` first, then the repo root; browse that directory for the full set. Notable options:

| Architecture | What it gives you |
|---|---|
| `edge_mem_4x4/scalar_3side.json` (default) | 4×4 scalar grid, memory PEs on three edges, declares `TREG_RESET_ON_LAUNCH` (enables native stateful-MAC lowering) |
| `hycube_original.json` / `hycube_original_updatemem8x8.json` | Classic HyCUBE 4×4 / 8×8 |
| `hycube_edgePEMEMs*.json` | Edge-memory variants (1/2/3 sides, with/without router) |
| `hycube_edgePEMEMs_3sides_vec4_vred.json` (and `vec8`, non-`vred` variants) | SIMD architectures: `SIMD_WIDTH` > 1 enables vector packing; `VRED` enables vector-reduction lowering |

Effects worth knowing:

- `xdim`/`ydim`, `numberofbanks`/`banksize` are **inferred from the JSON**; setting them explicitly only validates against it.
- SIMD is **architecture-driven**: there are no `simd.*` config keys. Packing activates when the architecture declares a SIMD width and the unroll factor reaches it.
- `ARCH.CAPABILITIES.TREG_RESET_ON_LAUNCH: true` lets zero-seeded accumulators lower to a single stateful MAC node instead of an explicit recurrence — fewer nodes, easier mapping.

### 2. Mapper effort (`mapper_preset`)

| Preset | What it means for you |
|---|---|
| `trusted_pf` (default) | Deterministic PathFinder-only baseline: no SA fallback, no candidate pruning. Reproducible runs; the reference for comparisons. |
| `fast` | Smoke tests. Low backtracking and iteration budgets; may settle for a higher II. |
| `balanced` | Moderate search effort with SA/hybrid fallback. |
| `quality` | More SA restarts/moves, larger II search range. Slower, sometimes lower II. |
| `exhaust` | Largest budget, no starting-II headroom cap. For difficult kernels; slow. |

The unroll search's initial candidate beam also comes from the preset: `fast` fully maps 1 candidate, `trusted_pf`/`balanced` 2, `quality` 4, `exhaust` all — see [Auto-unroll](#3-the-graph-that-gets-mapped).

Per-II-attempt PathFinder guards are progress-aware (progress = a new minimum unmapped-node count): `fast` allows 60 s without progress / 300 s absolute, `trusted_pf` and `balanced` 120/600, `quality` 180/1200, `exhaust` 300/3600. A failed or stagnating attempt retries the same II with a perturbed node order (`ii_restarts` times) before escalating to II+1.

Any preset value can be overridden by an explicit config key (with a warning): `mapping_method` (0 = PathFinder, 1 = SA, 2 = hybrid), `init_II`/`max_II`, `ii_restarts`, `skip_ii_estimation`, `backtrack_limit`, `max_mapper_iter`, `proximity_sort`, `pf_candidate_topk` / `off_spm_mem_candidate_topk` (`0` uncapped, `auto_fast`, `auto_balanced`), and the `sa_*` family (`sa_seed`, `sa_restarts`, `sa_moves_per_temp`, `sa_temp_max`, `sa_temp_min`, `sa_candidate_topk`).

### 3. The graph that gets mapped

These transforms typically matter more for II than mapper effort does. All of them are validated: an unsupported shape falls back to the untransformed graph rather than mapping something unverified.

**Auto-unroll search (`auto_unroll.enabled`, on by default).** Evaluates bounded unroll factors (internally `[1, 2, 4, 8]`, override with `auto_unroll.factors`), original vs LLVM-legal interchanged loop orders (`try_loop_interchange`, default on), linear/tree scalar reduction forms, and — for a supported perfect three-level GEMM-like nest — two-axis `systolic_jam` candidates whose exact-divisor factors are bounded by the architecture dimensions. Duplicate DFGs are removed, candidates are ranked by an optimistic recurrence/resource bound, the preset beam is fully mapped, and remaining candidates run only if their bound can still beat the incumbent's `II / unroll_factor` (systolic: `II / (factor0 × factor1)`). More candidates = more compile time; `auto_unroll.stop_on_degradation: true` stops the adaptive beam expansion once an incumbent exists.

Systolic candidates get an explicit first-attempt placement (row streams on left memory PEs, column streams on bottom, MAC `(r,c)` at that compute coordinate) and start at full-occupancy II; a failed structured attempt retries unrestricted at the same II. `auto_unroll.fp_nondeterminism` (default `true`) permits reassociating a floating-point `C += product` memory recurrence into a zero-seeded steady-state accumulator plus one boundary update; set `false` to preserve the original FP operation order at the cost of losing the systolic form. The supported nest contract lives in `Morpher_DFG_Generator/TRANSFORM_LIMITATIONS.md`.

**Access externalization (`dfg_pipeline.externalize_access`, default `agi_iter` in `config/default.yaml`).** `agi` removes address-generation (AGI) nodes before instrumentation — addresses are supplied from recorded traces instead of computed on the fabric. `agi_iter` additionally removes the iterator/control component when it has no edge into surviving work. The mapped DFG becomes the steady-state compute body: often dramatically smaller, hence lower II. The transform refuses (with an identity manifest) whenever iterator-derived predicates, data, recurrences, or reductions still reach surviving work. `off` keeps everything on the fabric.

**Accumulator boundary canonicalization (`dfg_pipeline.boundary_canonicalization`, default `verified`).** Recognizes canonical `i32` scalar loop-carried accumulators (constant seed, direct ADD update, exact CMERGE/SELECT lifecycle, one scalar live-out), strips their PHI launch/exit scaffolding from the mapped graph, and emits a validated boundary manifest that the simulator checks. Zero seeds fuse MUL+ADD into a native stateful MAC when the architecture declares MAC + `TREG_RESET_ON_LAUNCH` (`dfg_pipeline.treg_reset_on_launch` is auto-enabled by that declaration; explicit `false` disables it, explicit `true` still requires the declaration). Unsupported accumulator shapes pass through unchanged.

**SIMD packing (`dfg_pipeline.simd_pack: verified`, default `off`).** Packs a complete lane-isomorphic scalar subgraph into vector ops after instrumentation, when the architecture declares a SIMD width and the unroll factor reaches it. Requires `externalize_access: agi` or `agi_iter`. Supports lane-isomorphic recurrences and (with `SCALAR_BROADCAST: true`) one shared scalar input; on a `VRED` architecture, integer ADD/AND/OR/XOR reductions lower to per-lane partial sums with a single exit-only horizontal reduction that the simulator checks against traced values. Partial or unsupported groups remain scalar.

**Data layout (`layout_solver`, `replicate_data`, both default `false`).** Post-mapping memory-layout solving with trace remapping; `replicate_data` additionally allows read-only arrays to be replicated across banks. Note the current simulator does not model bank contention, so layout differences do not yet show up in cycle counts.

### 4. Memory model

| Key | Effect |
|---|---|
| `memory_mode: auto` (default) | Generates a runtime architecture JSON from the kernel's actual memory allocation. Bank sizes can grow to fit. |
| `memory_mode: static` | Maps directly against the pre-authored `arch_template_json`. Required when there is no mem-alloc information; noticeably slower in practice. |
| `spm_mode` | `on`/`off` scratchpad modeling in DFG generator and mapper. Static mode cannot auto-grow banks. |

### 5. Validation

| Key | Effect |
|---|---|
| `enable_simulation` (default `true`) | Generate instrumented memory traces and validate the mapping against them. Turn off only for mapping-speed experiments. |
| `validation_mode` | `functional` (default): golden-reference value check. `event_driven`: cycle-accurate simulation of the mapped configuration — slower, but exercises timing. |
| `max_test_samples` | How many trace samples to validate. More samples = more confidence, more runtime. |

A clean run prints `Matches::N, Mismatches::0`. Boundary/SIMD manifests extend what the simulator checks (seeds, exit-only reductions, live-outs) so removed scaffolding is still verified, not just dropped.

## Config reference

Default config: `config/default.yaml` (4×4 edge-memory scalar arch, `trusted_pf`, auto-unroll on, `agi_iter` externalization, functional validation). `config/README.md` describes the other provided configs (scalar baselines, SIMD-4 variants, XML mapping).

Keys not covered above:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `mapper_subfolder` | string | required | Output grouping under `workspace/.../mapper/<subfolder>/` |
| `multihop` | int ≥ 1 | `4` | Max routing hop budget |
| `print_dfg` | bool | preset | Print DFG details in mapper logs |
| `node_pin` | `node_idx:X:Y` | empty | Debug: force one DFG node onto one PE |
| `bank_affinity_cost` | int ≥ 0 | `0` (off) | Experimental soft mapper cost toward the dominant bank of an array's placed accesses; never excludes candidates |

Removed keys that now raise errors: `auto_unroll.remainder_policy`, `auto_unroll.outer_factor` (remainders are always preserved — size trip counts as valid multiples of the target factor). Legacy `json_arch` / `json_arch_before_memupdate` still parse with a warning. Deprecated top-level `simd.*` data is ignored.

RecMin (the recurrence-based II lower bound) has no configuration: the mapper measures it from the final transformed DFG's `nextiter`/`RecParents` return paths, starting conservatively at II 1 when paths are missing.

### Environment toggles

| Variable | Effect |
|---|---|
| `MORPHER_LLVM_BIN_DIR` / `MORPHER_LLVM_LIB_DIR` / `MORPHER_LLVM_SUFFIX` / `MORPHER_LLVM_DIR` | LLVM toolchain resolution (see Setup) |
| `MORPHER_CMAKE_ARGS` | Extra CMake flags for `build_all.sh` |
| `MORPHER_VERBOSE_CMDS=1` | Print underlying shell commands per stage |
| `MORPHER_DUMP_VERBOSE_JSON=1` | Dump expanded mapper architecture JSON |
| `MORPHER_PRINT_DFG=1` | Force mapper-side DFG printing |
| `MORPHER_MAX_TRACES=<N>` | Cap trace generation (normally set internally from `max_test_samples`) |
| `ARTIFACT_SIM_DEBUG=1` | Artifact simulator debug output |

## XML input mode

Pass a `.xml` file as the first argument to skip DFG generation and map a pre-generated DFG:

```bash
python3 -u run_morpher.py gemm_systolic.xml                                  # mapper_only, config/default.yaml
python3 -u run_morpher.py gemm_systolic.xml gemm_systolic config/xml_arch.yaml
```

- Supports `mapper_only` (default), `runall`, and `sim_only`; the function name defaults to the filename stem.
- In `memory_mode: auto` a mem-alloc file is required: place `<kernel>_mem_alloc.txt` (or `<xml-stem>_mem_alloc.txt`) beside the XML, or pass `--mem-alloc <path>`. Otherwise use a static config such as `config/xml_arch.yaml`.

## Regression gate (run this after mapper/DFG/simulator changes)

```bash
python3 scripts/regression_gate.py
```

Builds the mapper, runs a small set of fast kernels end-to-end with `config/regression.yaml`, and fails on any II regression, validation mismatch, or >3× mapper-time blowup against `scripts/regression_expectations.yaml`. Flags: `--filter`, `--no-build`, `--strict`, `--timeout-sec`.

## Batch runs, regression suites, and sweeps

All three runners take `--help` for their full flag lists.

**All benchmarks in parallel** (`scripts/run_all_benchmarks.py`) — auto-discovers everything under `Morpher_DFG_Generator/benchmarks/`, writes per-kernel logs plus a summary CSV (default `benchmark_run_results.csv`):

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --timeout-sec 120 --output-csv results.csv
python3 scripts/run_all_benchmarks.py --config config/scalar.yaml --dry-run
```

**Curated suites with II bounds** (`scripts/run_regression_benchmarks.py`) — suite definitions and per-benchmark `max_ii` live in `scripts/regression_benchmarks.yaml`; the runner fails if any benchmark exceeds its bound:

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_4x4
# other suites: ci_4x4_scalar_agi, ci_4x4_scalar_agi_pruned, ci_4x4_gemm_simd4_unroll
```

**Architecture/config sweeps** (`scripts/run_arch_sweeps.py`) — expands a manifest suite (`scripts/arch_sweeps.yaml`: arches × variants × config overrides) into experiments and emits `summary.csv` + `combined_results.csv`:

```bash
python3 scripts/run_arch_sweeps.py --suite edge_mem_arch_sweep --dry-run
python3 scripts/run_arch_sweeps.py --suite edge_mem_arch_sweep --experiment-jobs 2
```

## Adding new benchmarks

Benchmarks are auto-discovered — drop a source file at exactly:

```
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

Rules: the `.c` filename must match its folder, `<kernel>` must be a valid C identifier, the path must be exactly three levels deep, and the top-level function in the file must be named `<kernel>`. Mark the loop to map with `please_map_me()`.

```bash
python3 scripts/run_all_benchmarks.py --dry-run --filter 'gemv'   # verify discovery
python3 -u run_morpher.py polybench/gemv/gemv.c gemv              # run it
```

To include it in a regression suite, add it under the suite in `scripts/regression_benchmarks.yaml` with a `max_ii` bound.

## Maintenance

```bash
bash scripts/clean_workspace.sh              # remove generated artifacts (add --dry-run to preview)
bash scripts/check_no_generated_tracked.sh   # verify no generated files are tracked by git
```

## Repository layout and further documentation

```
run_morpher.py                    # single-benchmark entry point
morpher_runner/                   # Python pipeline (DFG gen, mapper, simulator stages)
config/                           # run configurations
scripts/                          # regression gate, batch/sweep runners, maintenance
Morpher_DFG_Generator/            # LLVM-based DFG generator + benchmarks/<suite>/<kernel>/
Morpher_CGRA_Mapper/              # CGRA mapper (PathFinder + SA) + json_arch/
simulator/artifact_simulator/     # functional / cycle-accurate validation simulator
```

- [`docs/CODE_GUIDE.md`](docs/CODE_GUIDE.md) — code map, data flow, and where to make changes
- [`docs/components/`](docs/components/) — per-component detail (runner pipeline, DFG generator, mapper, simulator, scripts/config)
- [`Morpher_DFG_Generator/TRANSFORM_LIMITATIONS.md`](Morpher_DFG_Generator/TRANSFORM_LIMITATIONS.md) — supported C++ transform contracts

## Cite

**Reference publication**

- ICCAD 2025: [Building an Open CGRA Ecosystem for Agile Innovation](https://arxiv.org/abs/2508.19090)

**Additional publications**

- HotChips 2024: [PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications](https://ieeexplore.ieee.org/abstract/document/10665106)
- WOSET 2022: [Morpher: An Open-Source Integrated Compilation and Simulation Framework for CGRA](https://www.comp.nus.edu.sg/~tulika/WOSET_MORPHER_2022.pdf)
- DAC 2022: [PANORAMA: Divide-and-Conquer Approach for Mapping Complex Loop Kernels on CGRA](https://www.comp.nus.edu.sg/~tulika/DAC22.pdf)
- HPCA 2022: [LISA: Graph Neural Network based Portable Mapping on Spatial Accelerators](https://www.comp.nus.edu.sg/~tulika/HPCA_LISA_2022.pdf)
- ASPLOS 2022: [REVAMP: A Systematic Framework for Heterogeneous CGRA Realization](https://www.comp.nus.edu.sg/~tulika/asplos22.pdf)
- TCAD 2022: [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap-TCAD.pdf)
- TCAD 2022: [ChordMap: Automated Mapping of Streaming Applications onto CGRA](https://ieeexplore.ieee.org/document/9351547)
- Book chapter: [Coarse-Grained Reconfigurable Array (CGRA)](https://www.comp.nus.edu.sg/~tulika/CGRA-Survey.pdf)
