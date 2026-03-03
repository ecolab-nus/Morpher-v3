# Scripts and Config Logic

This document explains automation scripts and configuration behavior.

## `config/`

## `default_config.yaml`

Primary runtime config for integrated runs.

Important groups:

- memory model:
  - `memory_mode`
  - `arch_template_json`
  - `numberofbanks`, `banksize`
- mapper behavior:
  - `mapping_method`
  - `init_II`, `max_II`, `ii_restarts`
  - `max_mapper_iter`, `backtrack_limit`
  - SA parameters (`sa_*`)
- simulation behavior:
  - `enable_simulation`
  - `max_test_samples`

## `xml_mapper_static.yaml`

Convenience config for XML-only mapping with static architecture JSON (no mem-alloc rewrite required).

## `scripts/run_all_benchmarks.py`

Purpose:

- run many benchmarks in parallel and produce summary CSV

Discovery rule:

- benchmark sources must follow:
  - `Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c`

Main behavior:

1. discover + validate benchmark layout
2. optional regex filtering and max-count limiting
3. launch parallel worker runs (`run_morpher.py`)
4. capture per-benchmark log files
5. extract key summary fields:
   - status
   - achieved II
   - match/mismatch counts
6. write `benchmark_run_results.csv`

## `scripts/clean_generated.sh`

Purpose:

- remove generated artifacts in benchmark trees while preserving expected placeholders

Supports:

- normal mode
- `--dry-run` preview mode

## `scripts/check_no_generated_tracked.sh`

Purpose:

- enforce repository policy that generated files are not tracked in git

Checks:

- DFG benchmark trees only track source `.c` files
- mapper/simulator benchmark trees only track `.gitkeep`
- build output trees are not tracked

## Legacy Helper Scripts

- `scripts/bin_to_trace.py`
- `scripts/hycube_config.py`

These are utility workflows for trace/memory conversion and architecture-specific helper constants.
