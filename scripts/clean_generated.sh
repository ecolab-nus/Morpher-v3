#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY_RUN=0

if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
fi

run_rm() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[dry-run] rm %s\n' "$*"
  else
    rm "$@"
  fi
}

run_rm_rf() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[dry-run] rm -rf %s\n' "$*"
  else
    rm -rf "$@"
  fi
}

clean_dfg_benchmarks() {
  local dfg_dir="$ROOT_DIR/Morpher_DFG_Generator/benchmarks"
  if [[ ! -d "$dfg_dir" ]]; then
    return
  fi

  while IFS= read -r -d '' file; do
    run_rm -f "$file"
  done < <(find "$dfg_dir" -type f ! -name '*.c' -print0)
}

clean_generated_benchmarks() {
  local bench_dir="$1"
  if [[ ! -d "$bench_dir" ]]; then
    return
  fi

  while IFS= read -r -d '' path; do
    run_rm_rf "$path"
  done < <(find "$bench_dir" -mindepth 1 ! -name '.gitkeep' -print0)
}

ensure_placeholder() {
  local placeholder="$1"
  local parent
  parent="$(dirname "$placeholder")"
  mkdir -p "$parent"
  if [[ ! -f "$placeholder" ]]; then
    : > "$placeholder"
  fi
}

clean_dfg_benchmarks
clean_generated_benchmarks "$ROOT_DIR/Morpher_CGRA_Mapper/benchmarks"
clean_generated_benchmarks "$ROOT_DIR/hycube_simulator/benchmarks"

ensure_placeholder "$ROOT_DIR/Morpher_CGRA_Mapper/benchmarks/.gitkeep"
ensure_placeholder "$ROOT_DIR/hycube_simulator/benchmarks/.gitkeep"

echo "Cleanup complete."
