#include <stdio.h>

#define N 64

static int A[N][N];
static int C[N][N];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++) {
      A[i][j] = i + j + 1;
      C[i][j] = (i == j) ? 2 : 1;
    }
}

static int syrk(void) {
  int i, j, k;

#pragma scop
  for (i = 0; i < N; i++)
    for (j = 0; j <= i; j++) {
      C[i][j] *= beta;
      for (k = 0; k < N; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        C[i][j] += alpha * A[i][k] * A[j][k];
      }
    }
#pragma endscop

  return C[0][0];
}

int main(void) {
  init_array();
  return syrk();
}
