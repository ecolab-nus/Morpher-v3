#include <stdio.h>

#define N 64

static int A[N][N];

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      A[i][j] = (i == j) ? (N + i + 1) : ((i + j) % 7 + 1);
}

static int cholesky(void) {
  int i, j, k;

#pragma scop
  for (i = 0; i < N; i++) {
    for (j = 0; j < i; j++) {
      for (k = 0; k < j; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        A[i][j] -= A[i][k] * A[j][k];
      }
      A[i][j] /= A[j][j];
    }
    for (k = 0; k < i; k++)
      A[i][i] -= A[i][k] * A[i][k];
  }
#pragma endscop

  return A[N - 1][N - 1];
}

int main(void) {
  init_array();
  return cholesky();
}
