#include <stdio.h>

#define N 64

static int A[N][N];
static int u1[N], v1[N], u2[N], v2[N];
static int w[N], x[N], y[N], z[N];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++) {
    u1[i] = i + 1;
    u2[i] = i + 2;
    v1[i] = i + 3;
    v2[i] = i + 4;
    y[i] = i + 5;
    z[i] = i + 6;
    x[i] = 0;
    w[i] = 0;
    for (j = 0; j < N; j++)
      A[i][j] = i + j + 1;
  }
}

static int gemver(void) {
  int i, j;

#pragma scop
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      A[i][j] += u1[i] * v1[j] + u2[i] * v2[j];
    }

  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      x[i] += beta * A[j][i] * y[j];

  for (i = 0; i < N; i++)
    x[i] += z[i];

  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      w[i] += alpha * A[i][j] * x[j];
#pragma endscop

  return w[0];
}

int main(void) {
  init_array();
  return gemver();
}
