#include <stdio.h>

#define N 64
#define M 64

static int A[N][M];
static int B[N][M];
static int C[N][N];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++) {
    for (j = 0; j < M; j++) {
      A[i][j] = i + j + 1;
      B[i][j] = i - j + 3;
    }
    for (j = 0; j < N; j++)
      C[i][j] = (i == j) ? 2 : 1;
  }
}

static int syr2k(void) {
  int i, j, k;

#pragma scop
  for (i = 0; i < N; i++)
    for (j = 0; j <= i; j++) {
      C[i][j] *= beta;
      for (k = 0; k < M; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        C[i][j] += alpha * A[i][k] * B[j][k];
        C[i][j] += alpha * B[i][k] * A[j][k];
      }
    }
#pragma endscop

  return C[0][0];
}

int main(void) {
  init_array();
  return syr2k();
}
