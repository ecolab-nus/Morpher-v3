# Scripts and Config Logic

This document explains automation scripts and configuration behavior.

## `config/`

## `default.yaml`

Primary runtime config for integrated runs.

Important groups:

- memory model:
  - `memory_mode`
  - `arch_template_json`
  - `numberofbanks`, `banksize` are optional when the arch JSON declares SPM banks and bank size
  - if omitted, bank count is inferred from `SPM_B*_WRAPPER` instances and bank size from `ARCH.SPM.MEM.SIZE`
  - for non-SPM architectures, keep them explicit in config
  - if DFG bank allocation still overflows, the runner can auto-grow the effective `banksize` during the run when doing so will not make the architecture model inconsistent
- mapper behavior:
  - `mapper_preset`: one of `trusted_pf`, `fast`, `balanced`, `quality`, `exhaust`
  - `init_II`, `multihop`
  - presets expand to the low-level mapper search knobs internally
  - `trusted_pf`: deterministic PathFinder-only baseline; the default in `config/default.yaml`
  - `fast`: quicker answer, less search
  - `balanced`: moderate search with SA/hybrid fallback
  - `quality`: tries harder for better mappings / lower II
  - `exhaust`: deepest search, highest runtime cost; disables proximity bias and starting-II headroom by default to broaden exploration
  - `node_pin`: optional advanced override, passed through to the mapper as `--node_pin`
  - `node_pin` syntax: `node_idx:X:Y` (example: `17:0:0`)
- architecture dimensions:
  - `xdim`, `ydim` are optional
  - when omitted, they are inferred from `arch_template_json`
  - if provided, they must match the JSON dimensions
- simulation behavior:
  - `enable_simulation`
  - `max_test_samples`

## `xml_arch.yaml`

Convenience config for XML-only mapping with static architecture JSON (no mem-alloc rewrite required). `config/README.md` lists the remaining provided configs.

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

## `scripts/clean_workspace.sh`

Purpose:

- remove generated artifacts in benchmark trees and `workspace/morpher_runs` while preserving expected placeholders

Supports:

- normal mode
- `--dry-run` preview mode

## Other Runner Scripts

- `scripts/regression_gate.py`: one-command build + end-to-end guard over a few fast kernels; fails on II regression, validation mismatch, or mapper-time blowup. Run after any mapper/DFG/simulator change.
- `scripts/run_regression_benchmarks.py`: curated suites with per-benchmark `max_ii` bounds (`scripts/regression_benchmarks.yaml`).
- `scripts/run_arch_sweeps.py`: expands an architecture/config manifest (`scripts/arch_sweeps.yaml`) into experiments and emits combined CSVs.

See the README for full flag documentation; each script also takes `--help`.

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
