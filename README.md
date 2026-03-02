# Morpher

Morpher is an open-source CGRA compilation and simulation framework.

It takes a C kernel, maps it to a user-defined CGRA architecture, and validates the mapped kernel with cycle-accurate simulation.

## Supported Platform

- Linux

## Repository Layout

- `Morpher_DFG_Generator/`: Front-end and DFG generation flow
- `Morpher_CGRA_Mapper/`: CGRA mapping backend
- `hycube_simulator/`: Cycle-accurate simulator
- `config/`: Default and custom run configurations
- `scripts/`: Utility and automation scripts
- `run_morpher.py`: Single-benchmark entrypoint

## Requirements

- LLVM/Clang 10.0.0 (`clang`, `opt`, `llvm-link`, `llc`)
- CMake and Make
- Python 3
- `gcc-multilib` and `g++-multilib` (for 32-bit instrumentation flow)
- Graphviz (`dot`) for DFG PDF generation

Example install on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-10 llvm-10 llvm-10-dev llvm-10-tools \
  gcc-multilib g++-multilib graphviz
```

If your system uses versioned LLVM binaries (for example `clang-10`), ensure the unversioned names expected by Morpher (`clang`, `opt`, `llvm-link`, `llc`) are on `PATH`.

## Setup

```bash
git clone --recurse-submodules https://github.com/ecolab-nus/Morpher.git
cd Morpher
git submodule update --init --remote
pip3 install -r python_requirements.txt
bash build_all.sh
```

## Usage

### Run a Single Benchmark

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

- `config_path` default: `config/default_config.yaml`
- `runmode`: `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`

Key config knobs in `config/default_config.yaml`:

- `memory_mode`: `auto` or `static`
  - `auto`: generate per-kernel runtime arch JSON from `arch_template_json` + `<kernel>_mem_alloc.txt`
    at `Morpher_CGRA_Mapper/benchmarks/<mapper_subfolder>/<suite>/<kernel>/<kernel>_arch_runtime.json`
  - `static`: use `arch_template_json` directly with no mem-layout regeneration
    (the JSON must already contain any required memory wrapper/data-layout definitions)
- `arch_template_json`: architecture template JSON (resolved from `Morpher_CGRA_Mapper/json_arch/` unless absolute path is provided)
- `max_test_samples`: also used as trace-generation cap for instrumentation (`MORPHER_MAX_TRACES`) so trace generation and simulator sampling stay aligned
- `skip_ii_estimation`: when `true`, mapper skips Res/Rec II bound analysis and starts directly from `init_II` (faster startup)
- `print_dfg`: when `true`, mapper prints parsed DFG details (debug only; slows down startup)
- `enable_simulation`: when `false`, skips DFG instrumentation/trace generation and skips the simulator stage

Deprecated config keys still accepted (with warning):

- `json_arch`
- `json_arch_before_memupdate`

Mapper startup diagnostics:

- Mapper prints startup timings for JSON parse/preprocess/CGRA construction.
- Verbose expanded-JSON dump is now disabled by default for speed.
  Set `MORPHER_DUMP_VERBOSE_JSON=1` if you need `<arch>_verbose.json` for debugging.

Example:

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

### Run Mapper Directly from an Existing DFG XML

If the first argument is an `.xml` file, `run_morpher.py` skips DFG generation and stages that XML directly for mapper.

```bash
python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]
```

- XML mode defaults:
  - `function`: derived from XML filename
  - `config_path`: `config/default_config.yaml`
  - `runmode`: `mapper_only`
- XML mode supports run modes: `mapper_only`, `runall`
- If `memory_mode: auto`, mapper also needs `<kernel>_mem_alloc.txt`
  - place it next to the XML, or pass `--mem-alloc <path>`
  - or use `config/xml_mapper_static.yaml` (static mode, uses `hycube_original.json`)

Example with the XML in repo root:

```bash
python3 -u run_morpher.py 2gemm_systolic.xml
```

Example with static XML-only config (no mem-alloc file needed):

```bash
python3 -u run_morpher.py 2gemm_systolic.xml 2gemm_systolic config/xml_mapper_static.yaml
```

### Run All Benchmarks

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
```

Useful options:

```bash
# Show discovered benchmarks without running
python3 scripts/run_all_benchmarks.py --dry-run

# Run a subset
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --jobs 8
python3 scripts/run_all_benchmarks.py --max-benches 10
```

Auto-discovery expects:

```text
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

### Add a New Benchmark Suite

1. Create a suite folder under `Morpher_DFG_Generator/benchmarks/`.
2. Add kernels in this normalized layout: `Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c`.
3. Ensure function names are valid C identifiers.

No runner script changes are required.

## Maintenance

### Clean Generated Artifacts

```bash
bash scripts/clean_generated.sh
```

Preview only:

```bash
bash scripts/clean_generated.sh --dry-run
```

Check tracked-file policy (same check as CI):

```bash
bash scripts/check_no_generated_tracked.sh
```

Current policy:

- `Morpher_DFG_Generator/benchmarks`: keep only `*.c` tracked.
- `Morpher_CGRA_Mapper/benchmarks`: generated-only; keep only `benchmarks/.gitkeep` tracked.
- `hycube_simulator/benchmarks`: generated-only; keep only `benchmarks/.gitkeep` tracked.
- Build outputs under `build/` and `src/build/` are ignored for mapper/simulator.

### Submodule Commit Workflow

When changes touch submodules, commit in this order:

1. Commit inside each changed submodule.
2. Commit top-level submodule pointer updates.
3. Verify pointers before push with `git submodule status`.

Example commands:

```bash
git -C Morpher_DFG_Generator status
git -C Morpher_DFG_Generator add -A
git -C Morpher_DFG_Generator commit -m "..."

git -C Morpher_CGRA_Mapper status
git -C Morpher_CGRA_Mapper add -A
git -C Morpher_CGRA_Mapper commit -m "..."

git -C hycube_simulator status
git -C hycube_simulator add -A
git -C hycube_simulator commit -m "..."

git status
git add Morpher_DFG_Generator Morpher_CGRA_Mapper hycube_simulator
git commit -m "Update submodule pointers"

git submodule status
```

## Precision Configuration

Default precision is `int32`.

To switch to 16-bit mode, uncomment `#define ARCHI_16BIT` in:

- `Morpher_DFG_Generator/include/morpherdfggen/common/ArchPrecision.h`
- `hycube_simulator/src/ArchPrecision.h`

## References

### Core Paper

- ICCAD 2025: [Building an Open CGRA Ecosystem for Agile Innovation](https://arxiv.org/abs/2508.19090)

BibTeX:

```bibtex
@misc{juneja2025buildingopencgraecosystem,
  title={Building an Open CGRA Ecosystem for Agile Innovation},
  author={Rohan Juneja and Pranav Dangi and Thilini Kaushalya Bandara and Zhaoying Li and Dhananjaya Wijerathne and Li-Shiuan Peh and Tulika Mitra},
  year={2025},
  eprint={2508.19090},
  archivePrefix={arXiv},
  primaryClass={cs.AR},
  url={https://arxiv.org/abs/2508.19090}
}
```

### Additional Publications

- HotChips 2024: [PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications](https://ieeexplore.ieee.org/abstract/document/10665106)
- WOSET 2022: [Morpher: An Open-Source Integrated Compilation and Simulation Framework for CGRA](https://www.comp.nus.edu.sg/~tulika/WOSET_MORPHER_2022.pdf)
- DAC 2022: [PANORAMA: Divide-and-Conquer Approach for Mapping Complex Loop Kernels on CGRA](https://www.comp.nus.edu.sg/~tulika/DAC22.pdf)
- HPCA 2022: [LISA: Graph Neural Network based Portable Mapping on Spatial Accelerators](https://www.comp.nus.edu.sg/~tulika/HPCA_LISA_2022.pdf)
- ASPLOS 2022: [REVAMP: A Systematic Framework for Heterogeneous CGRA Realization](https://www.comp.nus.edu.sg/~tulika/asplos22.pdf)
- TCAD 2022: [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap-TCAD.pdf)
- TCAD 2022: [ChordMap: Automated Mapping of Streaming Applications onto CGRA](https://ieeexplore.ieee.org/document/9351547)
- Book Chapter: [Coarse-Grained Reconfigurable Array (CGRA)](https://www.comp.nus.edu.sg/~tulika/CGRA-Survey.pdf)
