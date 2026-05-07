# Mapper Logic (`Morpher_CGRA_Mapper`)

This document explains how the CGRA mapper works, what algorithms are used, and what files are emitted.

## Goal

Map a DFG to the target architecture (space + time + routing), producing legal mapping outputs and configuration binaries.

## Key Directories and Files

- `src/CGRA_xml_compiler.cpp`: mapper executable entrypoint
- `src/mapper/PathFinderMapper.cpp`: pathfinder placement/routing core
- `src/mapper/HeuristicMapper.cpp`: II/resource heuristics and mapping logs
- `src/mapper/SimulatedAnnealingMapper.cpp`: SA-based mapping engine
- `src/arch/`, `src/dfg/`: architecture and DFG runtime model classes
- `update_mem_alloc.py`: runtime architecture generation for auto memory mode

## Entry Flow (`CGRA_xml_compiler.cpp`)

1. Parse mapper arguments.
2. Load DFG XML.
3. Load architecture:
   - from JSON template/runtime JSON
4. Select SPM behavior:
   - `on`, `off`, or `auto`
5. Select mapping method:
   - `0`: PathFinder
   - `1`: Simulated Annealing
   - `2`: Hybrid (PF + SA phases)
6. Estimate II bounds unless disabled.
7. Run mapping attempts/restarts and optional II escalations.
8. Emit mapping logs and binary output for simulator.

## Method-Specific Logic

## PathFinder (`PathFinderMapper.cpp`)

Core ideas:

- incremental placement and routing
- cost-aware path search (A*-style heuristics)
- congestion/conflict accounting across ports/resources
- backtracking when placement/routing dead-ends occur

Behavior knobs:

- `max_iter`
- `backtrack_limit`
- `max_hops`
- mutex/conflict path settings

Runner config exposes `max_hops` as YAML key `multihop`, which is forwarded to the mapper's `--hops` flag. The current default is `4`.
Runner config can also omit `xdim` and `ydim`; the runner infers them from the selected architecture JSON.
Runner config now prefers a high-level `mapper_preset` (`fast`, `balanced`, `quality`, `exhaust`) instead of exposing most low-level SA / search knobs in every YAML.

## Heuristic Layer (`HeuristicMapper.cpp`)

Core ideas:

- estimate minimum feasible II from operation demand vs available datapaths/FUs
- produce mapping logs for debug and analysis

## Simulated Annealing (`SimulatedAnnealingMapper.cpp`)

Core ideas:

- randomized search from an initial mapping
- iterative perturb + accept/reject based on temperature and cost
- tracks and restores best state if final state is not legal

Behavior knobs:

- seed / restart offsets
- moves per temperature
- max/min temperature
- candidate sampling size (`topk`)

## Auto Memory Layout (`update_mem_alloc.py`)

Used when runner config sets `memory_mode: auto`.

Flow:

1. Load architecture template JSON.
2. Load mem-alloc CSV (`var,base_addr`).
3. Split variables by bank index.
4. Validate expected SPM wrapper modules exist.
5. Rewrite wrapper `DATA_LAYOUT` sections and SPM size.
6. Emit runtime architecture JSON for mapper input.

## Main Output Artifacts

Per benchmark mapping folder:

- `<kernel>_arch_runtime.json` (auto mode)
- `*.mapping.csv`
- `*.routeInfo.log`
- `*.mappingwithlatency.txt`
- `*.congestion.info`
- `*.bin` (used by simulator)
- `result.txt` (final summary line)

## Common Debug Checks

- DFG loaded correctly (`--print_dfg 1`)
- II search bounds (`init_II`, `max_II`, `ii_restarts`)
- architecture JSON includes expected SPM wrappers when `spm_mode=on` or `memory_mode=auto`
