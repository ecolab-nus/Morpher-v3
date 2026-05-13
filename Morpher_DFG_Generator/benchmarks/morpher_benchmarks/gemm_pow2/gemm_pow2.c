// Triple-nested GEMM with N=8 (power-of-2 dimensions) on 1D flattened arrays.
// N=8 means the 2D index `i*N + k` lowers to a left-shift in LLVM IR, not a
// MUL, which is the form the layout solver's AGI parser can recover an affine
// stride from. Use this to exercise the partition/replicate paths on a
// real-shaped GEMM kernel.

#include <stdio.h>

#define N 128

int A[N * N];
int B[N * N];
int C[N * N];

__attribute__((noinline))
void gemm_pow2() {
  int i, j, k;
  for (i = 0; i < N; i++) {
    for (j = 0; j < N; j++) {
      for (k = 0; k < N; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        C[i * N + j] += A[i * N + k] * B[k * N + j];
      }
    }
  }
}

int main() {
  int i;
  for (i = 0; i < N * N; i++) {
    A[i] = i + 1;
    B[i] = (i * 3) + 2;
    C[i] = i;
  }
  gemm_pow2();
  for (i = 0; i < N * N; i++) {
    printf("%d\n", C[i]);
  }
  return 0;
}
