#!/bin/bash
set -euo pipefail

build_cmake_target() {
  local src_dir="$1"
  local build_dir="$2"

  mkdir -p "$build_dir"

  # Guard against stale CMake cache files checked in from another machine/path.
  cmake -E remove -f "$build_dir/CMakeCache.txt"
  cmake -E remove_directory "$build_dir/CMakeFiles"

  local -a cmake_args
  if [[ -n "${MORPHER_LLVM_DIR:-}" ]]; then
    cmake_args+=("-DLLVM_DIR=${MORPHER_LLVM_DIR}")
  fi
  if [[ -n "${MORPHER_CMAKE_ARGS:-}" ]]; then
    # Intentionally split on spaces so callers can pass ordinary CMake flags.
    read -r -a extra_args <<< "${MORPHER_CMAKE_ARGS}"
    cmake_args+=("${extra_args[@]}")
  fi

  cmake -S "$src_dir" -B "$build_dir" "${cmake_args[@]}"
  cmake --build "$build_dir" --parallel "$(nproc)"
}

build_cmake_target "Morpher_DFG_Generator" "Morpher_DFG_Generator/build"
build_cmake_target "Morpher_CGRA_Mapper" "Morpher_CGRA_Mapper/build"
build_cmake_target "simulator/artifact_simulator" "simulator/artifact_simulator/build"
build_cmake_target "simulator/hycube_simulator/src" "simulator/hycube_simulator/src/build"

echo "build success!!!"
