# Morpher DFG Generator

This directory builds Morpher's LLVM 18 DFG-generation plugin, `libdfggenPass.so`.
The runner loads it through `opt -load-pass-plugin` and uses two pipeline entries:

- `-passes=skeleton` for DFG extraction/instrumentation
- `-passes=morpher-loop-unroll` for Morpher's annotated-loop unroll pass

The pass expects the target loop to be marked with `please_map_me()` in the benchmark source.

## Requirements

- LLVM/Clang 18 development packages
- CMake and a C++17-capable compiler
- The parent repository's LLVM environment variables when LLVM 18 is not the default toolchain:
  - `MORPHER_LLVM_BIN_DIR`
  - `MORPHER_LLVM_LIB_DIR`
  - `MORPHER_LLVM_DIR`

`polly` is not required for the current build.

## Build

From the repository root, prefer:

```bash
bash build_all.sh
```

To build only this component:

```bash
cmake -S Morpher_DFG_Generator -B Morpher_DFG_Generator/build \
  ${MORPHER_LLVM_DIR:+-DLLVM_DIR="$MORPHER_LLVM_DIR"}
cmake --build Morpher_DFG_Generator/build --parallel "$(nproc)"
```

## Notes

- This repository now targets LLVM 18 directly rather than maintaining an older multi-version compatibility branch.
- If the host has multiple LLVM installs, set both `MORPHER_LLVM_BIN_DIR` and `MORPHER_LLVM_LIB_DIR` so `clang`, `opt`, and the plugin resolve against the same runtime.
- Usage examples and the end-to-end flow are documented in the repository-level [`README.md`](../README.md).
