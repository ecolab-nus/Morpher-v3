# Morpher

Morpher is an open-source CGRA compilation and simulation framework.

It takes a C kernel, generates a DFG, maps it onto a configurable CGRA architecture, and can validate the mapping with cycle-accurate simulation.

## Supported Platform

- Linux

## Main Features

- End-to-end flow from C kernel to mapped binary (`run_morpher.py`)
- Optional XML input mode (skip DFG generation and map a pre-generated DFG)
- Two memory modes:
  - `auto`: generate runtime architecture JSON from template + mem-allocation
  - `static`: use a pre-authored architecture JSON directly
- Optional simulation stage (`enable_simulation`)
- Parallel benchmark automation (`scripts/run_all_benchmarks.py`)
- Maintenance scripts for cleanup and generated-artifact tracking policy checks

For a full plain-language code and feature walkthrough, see:

- [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md)
- [`docs/DIRECTORY_LOGIC_GUIDE.md`](docs/DIRECTORY_LOGIC_GUIDE.md) (directory-by-directory logic)
- Component docs:
  - [`docs/components/RUNNER_PIPELINE.md`](docs/components/RUNNER_PIPELINE.md)
  - [`docs/components/DFG_GENERATOR.md`](docs/components/DFG_GENERATOR.md)
  - [`docs/components/MAPPER.md`](docs/components/MAPPER.md)
  - [`docs/components/SIMULATOR.md`](docs/components/SIMULATOR.md)
  - [`docs/components/SCRIPTS_CONFIG.md`](docs/components/SCRIPTS_CONFIG.md)

## Repository Layout

- `run_morpher.py`: single benchmark entry point
- `morpher_runner/`: Python pipeline implementation (DFG generation, mapper, simulator)
- `config/`: run configurations
- `scripts/`: batch runner and maintenance utilities
- `Morpher_DFG_Generator/`: LLVM-based DFG generator
- `Morpher_CGRA_Mapper/`: CGRA mapper
- `hycube_simulator/`: cycle-accurate simulator
- `dfgPartition/`: optional DFG partitioning utilities (experimental workflow)

## Requirements

- LLVM/Clang 10.0.0 tools on `PATH`:
  - `clang`, `opt`, `llvm-link`, `llc`
- CMake + Make
- Python 3 + pip
- `gcc-multilib` and `g++-multilib` (32-bit instrumentation path)
- Graphviz (`dot`) for DFG PDF generation

Example install on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-10 llvm-10 llvm-10-dev llvm-10-tools \
  gcc-multilib g++-multilib graphviz
```

If your distro exposes versioned binaries (for example `clang-10`), ensure unversioned commands (`clang`, `opt`, `llvm-link`, `llc`) are available on your `PATH`.

## Setup

Fresh clone:

```bash
git clone https://github.com/pruhnuhv/Dataflow-arch-mapper.git
cd Dataflow-arch-mapper
```

Install Python requirements and build all components:

```bash
python3 -m pip install --upgrade pip
python3 -m pip install -r python_requirements.txt
bash build_all.sh
```

Optional build sanity check:

```bash
ls Morpher_DFG_Generator/build/src/libdfggenPass.so
ls Morpher_CGRA_Mapper/build/src/cgra_xml_mapper
ls hycube_simulator/src/build/hycube_simulator
```

## Quick Start

Run one benchmark:

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

CLI format:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

`runmode` values:

- `runall` (default): DFG generation + mapper + simulator (if enabled in config)
- `dfg_gen_only`
- `mapper_only`
- `sim_only`

## XML Input Mode

If the first argument ends in `.xml`, Morpher skips DFG generation and stages that XML directly for mapper:

```bash
python3 -u run_morpher.py <dfg.xml> [function] [config_path] [runmode] [--mem-alloc <path>]
```

XML mode defaults:

- `function`: derived from XML filename
- `config_path`: `config/default_config.yaml`
- `runmode`: `mapper_only`

Notes:

- XML mode supports only `mapper_only` and `runall`
- In `memory_mode: auto`, mapper needs a mem-alloc file:
  - put `<kernel>_mem_alloc.txt` beside the XML, or
  - pass `--mem-alloc <path>`
- If you do not have a mem-alloc file, use static mode:
  - `config/xml_mapper_static.yaml` (provided in this repo)

Examples:

```bash
python3 -u run_morpher.py 2gemm_systolic.xml
python3 -u run_morpher.py 2gemm_systolic.xml 2gemm_systolic config/xml_mapper_static.yaml
```

## Configuration

Default config is `config/default_config.yaml`.

Most-used knobs:

- `memory_mode`: `auto` or `static`
- `arch_template_json`: architecture template (resolved from `Morpher_CGRA_Mapper/json_arch/` unless absolute path)
- `mapper_subfolder`: output grouping under mapper benchmark directory
- `enable_simulation`: whether to run instrumentation + simulator
- `numberofbanks`, `banksize`: memory model used by DFG pass and mapper
- `max_test_samples`: trace generation cap and simulator sample cap
- `init_II`, `max_II`, `ii_restarts`, `skip_ii_estimation`: mapper II behavior
- `mapping_method`: mapper strategy selection

Environment toggles:

- `MORPHER_VERBOSE_CMDS=1`: print underlying commands in mapper/simulator stages
- `MORPHER_DUMP_VERBOSE_JSON=1`: dump expanded mapper architecture JSON for debugging

Complete config field reference:

- [`docs/FEATURES_AND_CODE_GUIDE.md`](docs/FEATURES_AND_CODE_GUIDE.md)

## Batch Benchmark Runs

Run all auto-discovered benchmarks in parallel:

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
```

Useful options:

```bash
python3 scripts/run_all_benchmarks.py --dry-run
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --jobs 8
python3 scripts/run_all_benchmarks.py --max-benches 10
```

## Mapper Regression Suites

Run the fixed 4x4 regression set with 8 workers and fail if any benchmark
regresses past its expected achieved II:

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_4x4
```

Run the smaller 8x8 regression set manually when you need coverage on the
larger fabric. It is intentionally not part of default CI because 8x8 mapping
is much slower:

```bash
python3 scripts/run_regression_benchmarks.py --suite ci_8x8
```

Suites and expected max IIs live in `scripts/regression_benchmarks.yaml`. The
8x8 suite uses `config/regression_8x8.yaml`.

Discovery rule:

```text
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

## Maintenance

Clean generated artifacts:

```bash
bash scripts/clean_generated.sh
```

Preview clean:

```bash
bash scripts/clean_generated.sh --dry-run
```

Verify generated-artifact tracking policy:

```bash
bash scripts/check_no_generated_tracked.sh
```

## Precision Configuration

Default precision is `int32`.

To switch to 16-bit mode, uncomment `#define ARCHI_16BIT` in:

- `Morpher_DFG_Generator/include/morpherdfggen/common/ArchPrecision.h`
- `hycube_simulator/src/ArchPrecision.h`

## References

### Core Paper

- ICCAD 2025: [Building an Open CGRA Ecosystem for Agile Innovation](https://arxiv.org/abs/2508.19090)

### Additional Publications

- HotChips 2024: [PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications](https://ieeexplore.ieee.org/abstract/document/10665106)
- WOSET 2022: [Morpher: An Open-Source Integrated Compilation and Simulation Framework for CGRA](https://www.comp.nus.edu.sg/~tulika/WOSET_MORPHER_2022.pdf)
- DAC 2022: [PANORAMA: Divide-and-Conquer Approach for Mapping Complex Loop Kernels on CGRA](https://www.comp.nus.edu.sg/~tulika/DAC22.pdf)
- HPCA 2022: [LISA: Graph Neural Network based Portable Mapping on Spatial Accelerators](https://www.comp.nus.edu.sg/~tulika/HPCA_LISA_2022.pdf)
- ASPLOS 2022: [REVAMP: A Systematic Framework for Heterogeneous CGRA Realization](https://www.comp.nus.edu.sg/~tulika/asplos22.pdf)
- TCAD 2022: [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap-TCAD.pdf)
- TCAD 2022: [ChordMap: Automated Mapping of Streaming Applications onto CGRA](https://ieeexplore.ieee.org/document/9351547)
- Book Chapter: [Coarse-Grained Reconfigurable Array (CGRA)](https://www.comp.nus.edu.sg/~tulika/CGRA-Survey.pdf)
