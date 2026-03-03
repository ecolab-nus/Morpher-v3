# Simulator Logic (`hycube_simulator`)

This document explains the cycle-accurate simulator stage and how results are validated.

## Goal

Execute mapped CGRA configuration and compare produced memory values against expected values from traces.

## Key Files

- `src/hycube_sim.cpp`: simulator executable entrypoint
- `src/CGRA.cpp`: global simulator model and memory parsing/comparison logic
- `src/CGRATile.cpp`: per-tile instruction execution semantics
- `src/data_structures.h`: opcodes/data definitions

## Stage Inputs

- `-c <cmem_bin>`: mapped configuration binary file
- `-d <trace_file>`: trace/data input
- `-a <mem_alloc_file>`: memory base-address map
- `-m <total_mem_size>`: memory size setting
- `-x/-y`: CGRA dimensions

## High-Level Runtime Flow

1. Parse CLI arguments.
2. Build tile grid and neighbor connectivity.
3. Parse CMEM into per-tile config memories.
4. Parse DMEM + mem-alloc and initialize memory contents.
5. Execute cycle by cycle:
   - each tile executes current instruction
   - then registers/PC are updated
6. Stop based on memory sentinel condition + epilogue cycles.
7. Compare final memory values against expected post values.
8. Write result summary:
   - stdout line with matches/mismatches
   - `sim_result.txt` containing `matches,mismatches`

## Per-Tile Execution (`CGRATile.cpp`)

Each cycle a tile:

- reads predicate and operands from incoming channels/registers
- executes opcode (`ADD`, `MUL`, `SELECT`, `LOAD`, `STORE`, etc.)
- handles predicate-sensitive behavior for control/store ops
- writes outputs/routed values/register updates

This is the core behavioral model for instruction correctness.

## Memory Comparison Logic (`CGRA.cpp`)

- `parseDMEM(...)` loads expected data model.
- `printInterestedAddrOutcome()` compares observed vs expected values.
- mismatch addresses are logged.
- summary counts are emitted for automation scripts.

## Integration with Python Runner

Python wrapper (`morpher_runner/simulator.py`) runs simulator multiple times over sampled trace files and aggregates mismatch totals.

If simulator is enabled but traces are missing, runner now fails explicitly so false-pass runs are avoided.
