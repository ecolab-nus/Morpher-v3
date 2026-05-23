#include <stdio.h>

#define M 64
#define N 64

static int A[M][M];
static int B[M][N];
static int C[M][N];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < M; i++) {
    for (j = 0; j < M; j++)
      A[i][j] = i + j + 1;
    for (j = 0; j < N; j++) {
      B[i][j] = i - j + 3;
      C[i][j] = (i == j) ? 2 : 1;
    }
  }
}

static int symm(void) {
  int i, j, k;

#pragma scop
  for (i = 0; i < M; i++)
    for (j = 0; j < N; j++) {
      int temp2 = 0;
      for (k = 0; k < i; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        C[k][j] += alpha * B[i][j] * A[i][k];
        temp2 += B[k][j] * A[i][k];
      }
      C[i][j] = beta * C[i][j] + alpha * B[i][j] * A[i][i] + alpha * temp2;
    }
#pragma endscop

  return C[0][0];
}

int main(void) {
  init_array();
  return symm();
}
