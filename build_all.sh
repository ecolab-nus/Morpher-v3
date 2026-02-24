#!/bin/bash
set -euo pipefail

build_cmake_target() {
  local src_dir="$1"
  local build_dir="$2"

  mkdir -p "$build_dir"

  # Guard against stale CMake cache files checked in from another machine/path.
  cmake -E rm -f "$build_dir/CMakeCache.txt"
  cmake -E rm -rf "$build_dir/CMakeFiles"

  cmake -S "$src_dir" -B "$build_dir"
  cmake --build "$build_dir" --parallel "$(nproc)"
}

build_cmake_target "Morpher_DFG_Generator" "Morpher_DFG_Generator/build"
build_cmake_target "Morpher_CGRA_Mapper" "Morpher_CGRA_Mapper/build"
build_cmake_target "hycube_simulator/src" "hycube_simulator/src/build"

echo "build success!!!"
