#!/usr/bin/env bash
# Test harness for SystolicCandidatePass.
#
# Compiles each fixture.c with `clang -O1 -fno-unroll-loops`, runs the
# `morpher-systolic-candidate` pass, captures JSON output, and asserts the
# emitted verdict (and any expected_reject_reason substring) for each fixture.
#
# We do NOT compare full JSON exactly because field ordering, loop header
# names, and IV names depend on clang internals. We instead grep for the
# verdict + (optional) reject_reason substring per fixture, which is robust.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="${SCRIPT_DIR}/../../build/src/libdfggenPass.so"

if [[ ! -f "${PLUGIN}" ]]; then
  echo "FAIL: plugin not found at ${PLUGIN}" >&2
  echo "Build first: cd Morpher_DFG_Generator && cmake --build build" >&2
  exit 2
fi

declare -A EXPECTED_VERDICT=(
  [gemm_ijk]=yes
  [gemm_ikj]=yes
  [matvec]=maybe
  [stencil]=maybe
  [indirect]=no
  [side_effect]=no
  [aliasing]=no
  [ssa_reduction]=maybe
  [bicg]=maybe
  [jacobi_2d]=maybe
)

declare -A EXPECTED_REJECT=(
  [indirect]="non-affine memory access"
  [side_effect]="impure call"
  [aliasing]="unsafe aliasing"
)

PASS_COUNT=0
FAIL_COUNT=0

for fixture in "${!EXPECTED_VERDICT[@]}"; do
  src="${SCRIPT_DIR}/${fixture}.c"
  ll="${SCRIPT_DIR}/${fixture}.ll"
  report="${SCRIPT_DIR}/${fixture}.report.json"

  if [[ ! -f "${src}" ]]; then
    echo "FAIL ${fixture}: source file missing"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    continue
  fi

  rm -f "${report}"

  clang -O1 -fno-unroll-loops -emit-llvm -S "${src}" -o "${ll}" 2>/dev/null
  if [[ $? -ne 0 ]]; then
    echo "FAIL ${fixture}: clang compilation failed"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    continue
  fi

  opt -load-pass-plugin "${PLUGIN}" \
      -passes='morpher-systolic-candidate' \
      "-morpher-systolic-report=${report}" \
      "${ll}" -disable-output 2>/dev/null

  if [[ ! -f "${report}" ]]; then
    echo "FAIL ${fixture}: no report produced"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    continue
  fi

  expected_verdict="${EXPECTED_VERDICT[${fixture}]}"
  if ! grep -q "\"verdict\":\"${expected_verdict}\"" "${report}"; then
    echo "FAIL ${fixture}: expected verdict=${expected_verdict}, got:"
    cat "${report}" | head -5
    FAIL_COUNT=$((FAIL_COUNT + 1))
    continue
  fi

  reject_needle="${EXPECTED_REJECT[${fixture}]:-}"
  if [[ -n "${reject_needle}" ]]; then
    if ! grep -q "${reject_needle}" "${report}"; then
      echo "FAIL ${fixture}: expected reject_reason to contain '${reject_needle}', got:"
      cat "${report}" | head -5
      FAIL_COUNT=$((FAIL_COUNT + 1))
      continue
    fi
  fi

  echo "PASS ${fixture}: verdict=${expected_verdict}"
  PASS_COUNT=$((PASS_COUNT + 1))
done

echo "----------------------------------------"
echo "Summary: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
exit ${FAIL_COUNT}
