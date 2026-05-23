#include <stdio.h>

#define N 64

static int A[N][N];
static int B[N][N];
static int x[N];
static int tmp[N];
static int y[N];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++) {
    x[i] = i + 1;
    tmp[i] = 0;
    y[i] = 0;
    for (j = 0; j < N; j++) {
      A[i][j] = i + j + 1;
      B[i][j] = i - j + 3;
    }
  }
}

static int gesummv(void) {
  int i, j;

#pragma scop
  for (i = 0; i < N; i++) {
    tmp[i] = 0;
    y[i] = 0;
    for (j = 0; j < N; j++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      tmp[i] += A[i][j] * x[j];
      y[i] += B[i][j] * x[j];
    }
    y[i] = alpha * tmp[i] + beta * y[i];
  }
#pragma endscop

  return y[0];
}

int main(void) {
  init_array();
  return gesummv();
}
