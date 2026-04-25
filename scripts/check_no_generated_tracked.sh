#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

print_violation() {
  local title="$1"
  local lines="$2"
  if [[ -n "$lines" ]]; then
    echo "[FAIL] $title"
    echo "$lines"
    echo
    FAIL=1
  fi
}

tracked_matching() {
  local repo_dir="$1"
  local glob="$2"
  git -C "$repo_dir" ls-files "$glob" 2>/dev/null || true
}

tracked_with_prefix() {
  local repo_dir="$1"
  local prefix="$2"
  git -C "$repo_dir" ls-files | grep -E "^${prefix}" || true
}

dfg_repo="$ROOT_DIR/Morpher_DFG_Generator"
mapper_repo="$ROOT_DIR/Morpher_CGRA_Mapper"

root_generated="$(git -C "$ROOT_DIR" ls-files 'workspace/*' 2>/dev/null || true)"
print_violation "Root repo tracks run artifacts under workspace/." "$root_generated"

if [[ -d "$dfg_repo/.git" ]]; then
  dfg_non_c="$(tracked_with_prefix "$dfg_repo" 'benchmarks/' | grep -Ev '\.c$' || true)"
  dfg_build="$(tracked_with_prefix "$dfg_repo" 'build/' || true)"
  print_violation "DFG generator tracks non-.c benchmark files." "$dfg_non_c"
  print_violation "DFG generator tracks build outputs under build/." "$dfg_build"
fi

if [[ -d "$mapper_repo/.git" ]]; then
  mapper_bench="$(tracked_with_prefix "$mapper_repo" 'benchmarks/' | grep -Ev '^benchmarks/\.gitkeep$' || true)"
  mapper_build="$(
    {
      tracked_with_prefix "$mapper_repo" 'build/'
      tracked_with_prefix "$mapper_repo" 'src/build/'
    } | sed '/^$/d' || true
  )"
  print_violation "Mapper tracks benchmark artifacts (only benchmarks/.gitkeep allowed)." "$mapper_bench"
  print_violation "Mapper tracks build outputs under build/ or src/build/." "$mapper_build"
fi

if [[ "$FAIL" -ne 0 ]]; then
  echo "Generated-artifact tracking policy violated."
  exit 1
fi

echo "Generated-artifact tracking policy OK."
