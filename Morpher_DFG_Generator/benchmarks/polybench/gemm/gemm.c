#include <stdio.h>

#define NI 64
#define NJ 64
#define NK 64
#define DATA_TYPE int
#define POLYBENCH_2D(var, dim1, dim2, n1, n2) var[dim1][dim2]
#define _PB_NI ni
#define _PB_NJ nj
#define _PB_NK nk

static DATA_TYPE A[NI][NK];
static DATA_TYPE B[NK][NJ];
static DATA_TYPE C[NI][NJ];
static DATA_TYPE alpha_storage = 3;
static DATA_TYPE beta_storage = 5;

static void init_array(void) {
  int i, j;

  for (i = 0; i < NI; i++)
    for (j = 0; j < NK; j++)
      A[i][j] = i + j + 1;

  for (i = 0; i < NK; i++)
    for (j = 0; j < NJ; j++)
      B[i][j] = i - j + 3;

  for (i = 0; i < NI; i++)
    for (j = 0; j < NJ; j++)
      C[i][j] = i * j + 1;
}

static void gemm(int ni, int nj, int nk, DATA_TYPE alpha, DATA_TYPE beta,
                 DATA_TYPE POLYBENCH_2D(C, NI, NJ, ni, nj),
                 DATA_TYPE POLYBENCH_2D(A, NI, NK, ni, nk),
                 DATA_TYPE POLYBENCH_2D(B, NK, NJ, nk, nj)) {
  int i, j, k;

  // BLAS parameters:
  // TRANSA = 'N'
  // TRANSB = 'N'
  // => Form C := alpha * A * B + beta * C.
#pragma scop
  for (i = 0; i < _PB_NI; i++) {
    for (j = 0; j < _PB_NJ; j++)
      C[i][j] *= beta;
    for (k = 0; k < _PB_NK; k++) {
      DATA_TYPE aik = alpha * A[i][k];
      for (j = 0; j < _PB_NJ; j++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        C[i][j] += aik * B[k][j];
      }
    }
  }
#pragma endscop
}

int main(void) {
  init_array();
  gemm(NI, NJ, NK, alpha_storage, beta_storage, C, A, B);
  return C[0][0];
}
