#include <stdio.h>

#define M 256
#define N 256
#define DATA_TYPE int
#define POLYBENCH_2D(var, dim1, dim2, n1, n2) var[dim1][dim2]
#define _PB_M m
#define _PB_N n

static DATA_TYPE A[M][M];
static DATA_TYPE B[M][N];
static DATA_TYPE alpha_storage = 5;

static void init_array(void) {
  int i, j;

  for (i = 0; i < M; i++) {
    for (j = 0; j < M; j++)
      A[i][j] = i + j + 1;
  }

  for (i = 0; i < M; i++) {
    for (j = 0; j < N; j++)
      B[i][j] = (i == j) ? 2 : 1;
  }
}

static void trmm(int m, int n, DATA_TYPE alpha,
                 DATA_TYPE POLYBENCH_2D(A, M, M, m, m),
                 DATA_TYPE POLYBENCH_2D(B, M, N, m, n)) {
  int i, j, k;

  // BLAS parameters:
  // SIDE   = 'L'
  // UPLO   = 'L'
  // TRANSA = 'T'
  // DIAG   = 'U'
  // => Form B := alpha * A**T * B.
#pragma scop
  for (i = 0; i < _PB_M; i++) {
    for (j = 0; j < _PB_N; j++)
      B[i][j] = alpha * B[i][j];
    for (k = i + 1; k < _PB_M; k++) {
      DATA_TYPE aik = alpha * A[k][i];
      for (j = 0; j < _PB_N; j++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        B[i][j] += aik * B[k][j];
      }
    }
  }
#pragma endscop
}

int main(void) {
  init_array();
  trmm(M, N, alpha_storage, A, B);
  return B[0][0];
}
