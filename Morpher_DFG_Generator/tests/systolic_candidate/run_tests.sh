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
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

if [[ ! -f "${PLUGIN}" ]]; then
  echo "FAIL: plugin not found at ${PLUGIN}" >&2
  echo "Build first: cd Morpher_DFG_Generator && cmake --build build" >&2
  exit 2
fi

declare -A EXPECTED_VERDICT=(
  [gemm_ijk]=yes
  [gemm_ijk_marker]=yes
  [gemm_int_memory_acc_marker]=yes
  [gemm_dynamic]=yes
  [gemm_n10]=yes
  [gemm_two_nests]=yes
  [gemm_ikj]=yes
  [matvec]=maybe
  [stencil]=maybe
  [indirect]=no
  [side_effect]=no
  [aliasing]=no
  [ssa_reduction]=maybe
  [bicg]=maybe
  [jacobi_2d]=maybe
  [conv2d_2x2]=maybe
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
  ll="${TMP_DIR}/${fixture}.ll"
  report="${TMP_DIR}/${fixture}.report.json"

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

  if [[ "${fixture}" == "conv2d_2x2" ]]; then
    for needle in \
      '"weighted_terms":4' \
      '"coefficients_known":true' \
      '"coefficient":-3' \
      '"coefficient":2' \
      '"coefficient":5' \
      '"coefficient":6'; do
      if ! grep -q "${needle}" "${report}"; then
        echo "FAIL ${fixture}: expected source expression fact ${needle}, got:"
        cat "${report}" | head -5
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue 2
      fi
    done
  fi

  if ! python3 "${SCRIPT_DIR}/assert_recipe.py" "${fixture}" "${report}"; then
    echo "FAIL ${fixture}: transform recipe assertion failed"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    continue
  fi

  echo "PASS ${fixture}: verdict=${expected_verdict}"
  PASS_COUNT=$((PASS_COUNT + 1))
done

marker_src="${SCRIPT_DIR}/gemm_ijk_marker.c"
marker_ll="${TMP_DIR}/gemm_ijk_marker.transform.ll"
marker_out="${TMP_DIR}/gemm_ijk_marker.transformed.ll"
marker_err="${TMP_DIR}/gemm_ijk_marker.transform.err"

clang -O1 -fno-unroll-loops -emit-llvm -S "${marker_src}" -o "${marker_ll}" 2>/dev/null
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_ijk_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    -morpher-systolic-fp-nondeterminism=false \
    "${marker_ll}" -S -o "${marker_out}" 2>"${marker_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_ijk_marker transform: opt failed"
  head -20 "${marker_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'call.*@llvm.fmuladd' "${marker_out}") -ne 16 ]]; then
  echo "FAIL gemm_ijk_marker transform: expected 16 MAC calls"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'load float' "${marker_out}") -ne 24 ]]; then
  echo "FAIL gemm_ijk_marker transform: expected 16 output seeds + 8 shared input loads"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'morpher.systolic.acc.* = phi float' "${marker_out}") -ne 0 ]]; then
  echo "FAIL gemm_ijk_marker transform: fp_nondeterminism=false must preserve C-seeded recurrence"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'load float.*!morpher.systolic.mem_stream' "${marker_out}") -ne 8 ]]; then
  echo "FAIL gemm_ijk_marker transform: expected 8 tagged systolic input loads"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif ! grep -q '!{!"row"}' "${marker_out}" || ! grep -q '!{!"column"}' "${marker_out}"; then
  echo "FAIL gemm_ijk_marker transform: expected row and column stream metadata"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'call void @please_map_me' "${marker_out}") -ne 1 ]]; then
  echo "FAIL gemm_ijk_marker transform: expected one restored map marker"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!"morpher.systolic.rolled_reduction.v1"' "${marker_out}") -ne 1 ]]; then
  echo "FAIL gemm_ijk_marker transform: expected one rolled reduction loop"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_ijk_marker transform: 4x4 jammed body with shared loads"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

fp_nondeterminism_out="${TMP_DIR}/gemm_ijk_marker.fp_nondeterminism.transformed.ll"
fp_nondeterminism_err="${TMP_DIR}/gemm_ijk_marker.fp_nondeterminism.transform.err"
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_ijk_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    "${marker_ll}" -S -o "${fp_nondeterminism_out}" 2>"${fp_nondeterminism_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_ijk_marker fp_nondeterminism transform: opt failed"
  head -20 "${fp_nondeterminism_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'morpher.systolic.acc.* = phi float' "${fp_nondeterminism_out}") -ne 16 ]]; then
  echo "FAIL gemm_ijk_marker fp_nondeterminism transform: expected 16 temporary accumulators"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mac_coord' "${fp_nondeterminism_out}") -ne 16 ]]; then
  echo "FAIL gemm_ijk_marker fp_nondeterminism transform: expected 16 MAC coordinates"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif ! grep -q 'fp_nondeterminism enabled' "${fp_nondeterminism_err}"; then
  echo "FAIL gemm_ijk_marker fp_nondeterminism transform: default policy was not reported"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_ijk_marker fp_nondeterminism transform: 16 temporary accumulators"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

int_src="${SCRIPT_DIR}/gemm_int_memory_acc_marker.c"
int_ll="${TMP_DIR}/gemm_int_memory_acc_marker.transform.ll"
int_out="${TMP_DIR}/gemm_int_memory_acc_marker.transformed.ll"
int_err="${TMP_DIR}/gemm_int_memory_acc_marker.transform.err"
clang -O1 -fno-unroll-loops -emit-llvm -S "${int_src}" -o "${int_ll}" 2>/dev/null
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_int_memory_acc_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    "${int_ll}" -S -o "${int_out}" 2>"${int_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_int_memory_acc_marker transform: opt failed"
  head -20 "${int_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'morpher.systolic.acc.* = phi i32' "${int_out}") -ne 16 ]]; then
  echo "FAIL gemm_int_memory_acc_marker transform: expected 16 scalar accumulators"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mac_coord' "${int_out}") -ne 16 ]]; then
  echo "FAIL gemm_int_memory_acc_marker transform: expected 16 MAC coordinates"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mem_stream_index' "${int_out}") -ne 8 ]]; then
  echo "FAIL gemm_int_memory_acc_marker transform: expected 8 stream indices"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.dataflow' "${int_out}") -ne 24 ]]; then
  echo "FAIL gemm_int_memory_acc_marker transform: expected selected dataflow on 8 loads and 16 MACs"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif ! grep -q 'promoted 1 output recurrence' "${int_err}"; then
  echo "FAIL gemm_int_memory_acc_marker transform: output recurrence was not promoted"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_int_memory_acc_marker transform: stateful 4x4 MAC tile metadata"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

matvec_src="${SCRIPT_DIR}/matvec_marker.c"
matvec_ll="${TMP_DIR}/matvec_marker.transform.ll"
matvec_out="${TMP_DIR}/matvec_marker.transformed.ll"
matvec_err="${TMP_DIR}/matvec_marker.transform.err"
clang -O1 -fno-unroll-loops -emit-llvm -S "${matvec_src}" -o "${matvec_ll}" 2>/dev/null
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=matvec_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=1 \
    "${matvec_ll}" -S -o "${matvec_out}" 2>"${matvec_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL matvec_marker transform: opt failed"
  head -20 "${matvec_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'morpher.systolic.acc.* = phi i32' "${matvec_out}") -ne 4 ]]; then
  echo "FAIL matvec_marker transform: expected 4 scalar accumulators"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mac_coord' "${matvec_out}") -ne 4 ]]; then
  echo "FAIL matvec_marker transform: expected 4 MAC coordinates"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mem_stream_index' "${matvec_out}") -ne 5 ]]; then
  echo "FAIL matvec_marker transform: expected 4 row loads plus one shared column load"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.dataflow' "${matvec_out}") -ne 9 ]]; then
  echo "FAIL matvec_marker transform: expected selected dataflow on 5 loads and 4 MACs"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'call void @please_map_me' "${matvec_out}") -ne 1 ]]; then
  echo "FAIL matvec_marker transform: expected one restored map marker"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS matvec_marker transform: 4x1 jammed dot body with shared operand"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

beta_src="${SCRIPT_DIR}/gemm_beta_marker.c"
beta_ll="${TMP_DIR}/gemm_beta_marker.transform.ll"
beta_out="${TMP_DIR}/gemm_beta_marker.transformed.ll"
beta_err="${TMP_DIR}/gemm_beta_marker.transform.err"
clang -O1 -fno-unroll-loops -emit-llvm -S "${beta_src}" -o "${beta_ll}" 2>/dev/null
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-schedule-prep,morpher-systolic-polly-distribute,simplifycfg,adce,simplifycfg,morpher-systolic-unroll-jam' \
    -polly-only-func=gemm_beta_marker \
    -morpher-systolic-fn=gemm_beta_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    "${beta_ll}" -S -o "${beta_out}" 2>"${beta_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_beta_marker transform: Polly distribution or jam failed"
  head -20 "${beta_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mac_coord' "${beta_out}") -ne 16 ]]; then
  echo "FAIL gemm_beta_marker transform: expected 16 MAC coordinates"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'call void @please_map_me' "${beta_out}") -ne 1 ]]; then
  echo "FAIL gemm_beta_marker transform: expected one restored map marker"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif ! grep -q 'Morpher/Polly distributed beta' "${beta_err}"; then
  echo "FAIL gemm_beta_marker transform: targeted distribution did not run"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_beta_marker transform: Polly-distributed 4x4 jammed body"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

ws_src="${SCRIPT_DIR}/gemm_ws_marker.c"
ws_ll="${TMP_DIR}/gemm_ws_marker.transform.ll"
ws_out="${TMP_DIR}/gemm_ws_marker.transformed.ll"
ws_err="${TMP_DIR}/gemm_ws_marker.transform.err"
clang -m32 -O1 -fno-unroll-loops -emit-llvm -S "${ws_src}" -o "${ws_ll}" 2>/dev/null
opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_ws_marker \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    -morpher-systolic-dataflow=weight_stationary \
    "${ws_ll}" -S -o "${ws_out}" 2>"${ws_err}"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_ws_marker transform: opt failed"
  head -20 "${ws_err}"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.mac_coord' "${ws_out}") -ne 16 ]]; then
  echo "FAIL gemm_ws_marker transform: expected 16 spatial multiplies"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c '!morpher.systolic.stationary_weight' "${ws_out}") -ne 16 ]]; then
  echo "FAIL gemm_ws_marker transform: expected 16 stationary weights"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'call void @please_map_me' "${ws_out}") -ne 1 ]]; then
  echo "FAIL gemm_ws_marker transform: expected one restored map marker"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_ws_marker transform: exact 4x4 integer weight-stationary tile"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

ws_dfg_dir="${TMP_DIR}/gemm_ws_dfg"
mkdir -p "${ws_dfg_dir}"
(cd "${ws_dfg_dir}" && opt -load-pass-plugin "${PLUGIN}" \
    -passes=skeleton --fn gemm_ws_marker --nobanks 4 --banksize 65536 \
    --externalize-access=agi_iter --suppress-dfg-dot -S "${ws_out}" \
    -o instrumented.ll) >/dev/null 2>"${ws_dfg_dir}/dfg.err"
if [[ $? -ne 0 ]]; then
  echo "FAIL gemm_ws_marker DFG: generation failed"
  head -20 "${ws_dfg_dir}/dfg.err"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif [[ $(grep -c 'stationary_weight_operand' \
    "${ws_dfg_dir}/gemm_ws_marker_PartPredDFG.xml") -ne 16 ]]; then
  echo "FAIL gemm_ws_marker DFG: expected 16 register-supplied operands"
  FAIL_COUNT=$((FAIL_COUNT + 1))
elif ! python3 -c \
    'import json,sys; m=json.load(open(sys.argv[1])); assert len(m["stationary_preloads"]) == 16; assert len([a for a in m["launch"]["actions"] if a["operation"] == "preload"]) == 16' \
    "${ws_dfg_dir}/gemm_ws_marker_INNERMOST_LN1_BOUNDARY_manifest.json"; then
  echo "FAIL gemm_ws_marker DFG: invalid launch preload contract"
  FAIL_COUNT=$((FAIL_COUNT + 1))
else
  echo "PASS gemm_ws_marker DFG: 16 loads externalized into launch preloads"
  PASS_COUNT=$((PASS_COUNT + 1))
fi

if python3 -c \
    'import subprocess, sys; result = subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); raise SystemExit(0 if result.returncode != 0 else 1)' \
    opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_ijk \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=3 \
    -morpher-systolic-factor1=4 \
    "${TMP_DIR}/gemm_ijk.ll" -disable-output; then
  echo "PASS gemm_ijk invalid factor: rejected"
  PASS_COUNT=$((PASS_COUNT + 1))
else
  echo "FAIL gemm_ijk invalid factor: transform unexpectedly succeeded"
  FAIL_COUNT=$((FAIL_COUNT + 1))
fi

if python3 -c \
    'import subprocess, sys; result = subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); raise SystemExit(0 if result.returncode != 0 else 1)' \
    opt -load-pass-plugin "${PLUGIN}" \
    -passes='morpher-systolic-unroll-jam' \
    -morpher-systolic-fn=gemm_ijk \
    -morpher-systolic-candidate-index=0 \
    -morpher-systolic-factor0=4 \
    -morpher-systolic-factor1=4 \
    "${TMP_DIR}/gemm_ijk.ll" -disable-output; then
  echo "PASS gemm_ijk missing marker: rejected"
  PASS_COUNT=$((PASS_COUNT + 1))
else
  echo "FAIL gemm_ijk missing marker: transform unexpectedly succeeded"
  FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo "----------------------------------------"
echo "Summary: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
exit ${FAIL_COUNT}
