![morpher_cover](https://user-images.githubusercontent.com/12274945/198943201-17e9ff67-62b3-445f-bd04-feac08da1601.png)

# Morpher: An Open-Source Tool for CGRA Accelerators 

[![Actions Status](https://github.com/ecolab-nus/morpher/workflows/Build%20and%20Test/badge.svg)](https://github.com/ecolab-nus/morpher/actions)
[![GitHub Clones](https://img.shields.io/badge/dynamic/json?color=success&label=Clone&query=count&url=https://gist.githubusercontent.com/Dhananjayadmd/5baff68360b70cb31e4d861cf11f219e/raw/clone.json&logo=github)](https://github.com/MShawon/github-clone-count-badge)

Morpher is a powerful, integrated compilation and simulation framework, that can assist design space exploration and application-level developments of CGRA based systems. Morpher can take an application with a compute intensive kernel as input, compile the kernel onto a user-provided CGRA architecture, and automatically validate the compiled kernels through cycle-accurate simulation using test data extracted from the application. Morpher can handle real-world application kernels without being limited to simple toy kernels through its feature-rich compiler. Morpher architecture description language
lets users easily specify architectural features such as complex interconnects, multi-hop routing, and memory organizations. 





![framework](https://user-images.githubusercontent.com/12274945/198694251-ab21d639-8999-424a-bc5a-3e7921c638a0.png)

More information:
    [ICCAD 2025 paper](https://arxiv.org/abs/2508.19090)
    
![ezgif com-gif-maker(2)](https://user-images.githubusercontent.com/12274945/198826823-5230947d-86eb-4493-a6fc-5f43d61ab2b4.gif)


## Getting Started

Morpher is supported on Linux.

### Requirements

- LLVM/Clang 10.0.0 (`clang`, `opt`, `llvm-link`, `llc`)
- CMake and Make
- Python 3
- `gcc-multilib` and `g++-multilib` (for 32-bit build/link in instrumentation flow)
- Graphviz (`dot`) for DFG PDF generation

Example Ubuntu package install:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip \
  clang-10 llvm-10 llvm-10-dev llvm-10-tools \
  gcc-multilib g++-multilib graphviz
```

If your system installs versioned LLVM binaries (for example `clang-10`), make sure
the unversioned names used by Morpher (`clang`, `opt`, `llvm-link`, `llc`) are on `PATH`.

### Clone

```bash
git clone --recurse-submodules https://github.com/ecolab-nus/Morpher.git
cd Morpher
git submodule update --init --remote
```

### Build

Install Python dependencies:

```bash
pip3 install -r python_requirements.txt
```

Build all components:

```bash
bash build_all.sh
```

### Run a single benchmark

`run_morpher.py` usage:

```bash
python3 -u run_morpher.py <suite/kernel/kernel.c> <function> [config_path] [runmode]
```

- `config_path` default: `config/default_config.yaml`
- `runmode` choices: `runall`, `dfg_gen_only`, `mapper_only`, `sim_only`

Example:

```bash
python3 -u run_morpher.py morpher_benchmarks/array_add/array_add.c array_add
```

### Run all benchmarks

Use the maintained all-benchmarks runner:

```bash
python3 scripts/run_all_benchmarks.py --jobs 8
```

Useful options:

```bash
# Show discovered benchmark list without running
python3 scripts/run_all_benchmarks.py --dry-run

# Run a subset
python3 scripts/run_all_benchmarks.py --filter '^polybench/' --jobs 8
python3 scripts/run_all_benchmarks.py --max-benches 10
```

`scripts/run_all_benchmarks.py` discovers benchmarks from:

```text
Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c
```

### Adding new benchmark suites

To add new benchmarks that are automatically included in the all-benchmarks run:

1. Create a new suite directory under `Morpher_DFG_Generator/benchmarks/`.
2. Add kernels in the normalized layout:
   `Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c`
3. Use function names that match valid C identifiers.

No runner script changes are required.



## Miscellaneous

### Repository Hygiene

Use the cleanup script to remove generated benchmark artifacts while preserving tracked inputs/policies:

```
bash scripts/clean_generated.sh
```

Preview mode:

```
bash scripts/clean_generated.sh --dry-run
```

Run the tracking guard locally (same check used in CI):

```
bash scripts/check_no_generated_tracked.sh
```

Current policy:
- `Morpher_DFG_Generator/benchmarks`: keep only `*.c` tracked.
- `Morpher_CGRA_Mapper/benchmarks`: generated-only, keep only `benchmarks/.gitkeep` tracked.
- `hycube_simulator/benchmarks`: generated-only, keep only `benchmarks/.gitkeep` tracked.
- Build outputs under `build/` and `src/build/` are ignored for mapper/simulator.

### Submodule Commit Flow

When changes touch submodules, commit in this order:

1. Commit inside each changed submodule:

```
git -C Morpher_DFG_Generator status
git -C Morpher_DFG_Generator add -A
git -C Morpher_DFG_Generator commit -m "..."
```

```
git -C Morpher_CGRA_Mapper status
git -C Morpher_CGRA_Mapper add -A
git -C Morpher_CGRA_Mapper commit -m "..."
```

```
git -C hycube_simulator status
git -C hycube_simulator add -A
git -C hycube_simulator commit -m "..."
```

2. Commit top-level submodule pointer updates:

```
git status
git add Morpher_DFG_Generator Morpher_CGRA_Mapper hycube_simulator
git commit -m "Update submodule pointers"
```

3. Verify pointers before push:

```
git submodule status
```

### Change to 16 bit
The default precision is int32. To change to 16 bit, uncomment "#define ARCHI_16BIT" in line 4 of "Morpher_DFG_Generator/include/morpherdfggen/common/ArchPrecision.h" and "hycube_simulator/src/ArchPrecision.h"

## Publications

**If you use Morpher in your work, please cite this paper.**

[ICCAD] [Building an Open CGRA Ecosystem for Agile Innovation](https://arxiv.org/abs/2508.19090)\
Rohan Juneja, Pranav Dangi, Thilini Kaushalya Bandara, Zhaoying Li, Dhananjaya Wijerathne, Li-Shiuan Peh, Tulika Mitra\
(to appear in ICCAD 2025)
        
        @misc{juneja2025buildingopencgraecosystem,
        title={Building an Open CGRA Ecosystem for Agile Innovation}, 
        author={Rohan Juneja and Pranav Dangi and Thilini Kaushalya Bandara and Zhaoying Li and Dhananjaya Wijerathne and Li-Shiuan Peh and Tulika Mitra},
        year={2025},
        eprint={2508.19090},
        archivePrefix={arXiv},
        primaryClass={cs.AR},
        url={https://arxiv.org/abs/2508.19090}, }


**Or when working with HyCUBE (now PACE), please reference this paper.**

[HotChips] [PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications](https://ieeexplore.ieee.org/abstract/document/10665106)\
Vishnu P. Nambiar, Yi Sheng Chong, Thilini Kaushalya Bandara, Dhananjaya Wijerathne, Zhaoying Li, Rohan Juneja, Li-Shiuan Peh, Tulika Mitra, Anh Tuan Do\
IEEE Hot Chips Symposium (HCS) 2024

        @INPROCEEDINGS{10665106,
        author={Nambiar, Vishnu P. and Chong, Yi Sheng and Bandara, Thilini Kaushalya and Wijerathne, Dhananjaya and Li, Zhaoying and Juneja, Rohan and Peh, Li-Shiuan and Mitra, Tulika and Do, Anh Tuan},
        booktitle={2024 IEEE Hot Chips 36 Symposium (HCS)}, 
        title={PACE: A Scalable and Energy Efficient CGRA in a RISC-V SoC for Edge Computing Applications}, 
        year={2024},
        volume={},
        number={},
        pages={1-1},
        keywords={Program processors;Energy efficiency;Vectors;Hardware;Flow graphs;Arrays;Kernel},
        doi={10.1109/HCS61935.2024.10665106}}


[WOSET] [Morpher: An Open-Source Integrated Compilation and Simulation Framework for CGRA](https://www.comp.nus.edu.sg/~tulika/WOSET_MORPHER_2022.pdf)\
Dhananjaya Wijerathne, Zhaoying Li, Manupa Karunaratne, Li-Shiuan Peh, Tulika Mitra\
Workshop on Open-Source EDA Technology, International Conference on Computer-Aided Design 2022

[DAC] [PANORAMA: Divide-and-Conquer Approach for Mapping Complex Loop Kernels on CGRA](https://www.comp.nus.edu.sg/~tulika/DAC22.pdf)\
Dhananjaya Wijerathne, Zhaoying Li, Thilini Kaushalya Bandara, Tulika Mitra\
59th ACM/IEEE Design Automation Conference, 2022 __Publicity Paper__\
[Artifact Link](https://github.com/ecolab-nus/panorama)

[HPCA] [LISA: Graph Neural Network based Portable Mapping on Spatial Accelerators](https://www.comp.nus.edu.sg/~tulika/HPCA_LISA_2022.pdf)\
Zhaoying Li, Dan Wu, Dhananjaya Wijerathne, Tulika Mitra\
28th IEEE International Symposium on High-Performance Computer Architecture, 2022\
[Artifact Link](https://github.com/ecolab-nus/lisa) __Distinguished Artifact Award__

[ASPLOS] [REVAMP: A Systematic Framework for Heterogeneous CGRA Realization](https://www.comp.nus.edu.sg/~tulika/asplos22.pdf)\
Thilini Kaushalya Bandara, Dhananjaya Wijerathne, Tulika Mitra, Li-Shiuan Peh\
27th ACM International Conference on Architectural Support for Programming Languages and Operating Systems, 2022\
[Artifact Link](https://zenodo.org/record/5848404#.YgyrPTFByUk)

[TCAD] [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap-TCAD.pdf)\
Dhananjaya Wijerathne, Zhaoying Li, Anuj Pathania, Tulika Mitra, Lothar Thiele\
IEEE Transactions on Computer-Aided Design of Integrated Circuits and Systems, 41(10) 2022

[TCAD] [ChordMap: Automated Mapping of Streaming Applications onto CGRA](https://ieeexplore.ieee.org/document/9351547)\
Zhaoying Li, Dhananjaya Wijerathne, Xianzhang Chen, Anuj Pathania, Tulika Mitra\
IEEE Transactions on Computer-Aided Design of Integrated Circuits and Systems, 41(2) 2022

[Book Chapter] [Coarse-Grained Reconfigurable Array (CGRA)](https://www.comp.nus.edu.sg/~tulika/CGRA-Survey.pdf)\
Zhaoying Li, Dhananjaya Wĳerathne, Tulika Mitra\
Book chapter in “Handbook of Computer Architecture”, Springer (Invited)

[DATE] [HiMap: Fast and Scalable High-Quality Mapping on CGRA via Hierarchical Abstraction](https://www.comp.nus.edu.sg/~tulika/HiMap_DATE_2021.pdf)\
Dhananjaya Wijerathne, Zhaoying Li, Anuj Pathania, Tulika Mitra, Lothar Thiele\
Design Automation and Test in Europe 2021

[TECS] [CASCADE: High Throughput Data Streaming via Decoupled Access/Execute CGRA](https://www.comp.nus.edu.sg/~tulika/TECS-CASCADE19.pdf)\
Dhananjaya Wijerathne, Zhaoying Li, Manupa Karunaratne, Anuj Pathania, Tulika Mitra\
ACM Transactions on Embedded Computing Systems\
Special Issue on ACM/IEEE International Conference on Compilers, Architecture, and Synthesis for Embedded Systems 2019

[ICCAD] [4D-CGRA : Introducing the branch dimension to spatio-temporal application mapping of CGRAs](https://www.comp.nus.edu.sg/~tulika/4D-CGRA-ICCAD19.pdf)\
Manupa Karunaratne, Dhananjaya Wijerathne, Tulika Mitra, Li-Shiuan Peh\
38th ACM/IEEE International Conference on Computer Aided Design, November 2019

[A-SSCC] [HyCUBE: a 0.9V 26.4 MOPS/mW, 290 pJ/cycle, Power Efficient Accelerator for IoT Applications](https://www.comp.nus.edu.sg/~tulika/Hycube_for_ASSCC_2019.pdf)\
Bo Wang, Manupa Karunarathne, Aditi Kulkarni, Tulika Mitra, Li-Shiuan Peh\
IEEE Asian Solid-State Circuits Conference, November 2019

[DAC] [DNestMap : Mapping Deeply-Nested Loops on Ultra-Low Power CGRAs](https://www.comp.nus.edu.sg/~tulika/DAC18-CGRA.pdf)\
Manupa Karunaratne, Cheng Tan, Aditi Kulkarni, Tulika Mitra, Li-Shiuan Peh\
55th ACM/IEEE Design Automation Conference, June 2018

[DAC] [HyCUBE : A CGRA with Reconfigurable Single-cycle Multi-hop Interconnect](https://www.comp.nus.edu.sg/~tulika/DAC17.pdf)\
Manupa Karunaratne, Aditi Kulkarni, Tulika Mitra, Li-Shiuan Peh\
54th ACM/IEEE Design Automation Conference, June 2017

[ACM-TRETS] [Graph Minor Approach for Application Mapping on CGRAs](https://www.comp.nus.edu.sg/~tulika/TRETS14.pdf)\
Liang Chen, Tulika Mitra\
ACM Transactions on Reconfigurable Technology and Systems 2014

[FPT] Graph Minor Approach for Application Mapping on CGRAs [Much expanded journal version](https://www.comp.nus.edu.sg/~tulika/TRETS14.pdf)\
Liang Chen, Tulika Mitra\
International Conference on Field Programmable Technology, December 2012\
__Best Paper Award__


**Please see the beginning of the Publications section for the works to reference when using this project.**
