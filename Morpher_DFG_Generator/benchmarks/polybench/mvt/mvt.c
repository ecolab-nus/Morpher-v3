#include <stdio.h>

#define N 64

static int A[N][N];
static int x1[N], x2[N], y1[N], y2[N];

static void init_array(void) {
  int i, j;
  for (i = 0; i < N; i++) {
    x1[i] = i;
    x2[i] = i + 1;
    y1[i] = i + 2;
    y2[i] = i + 3;
    for (j = 0; j < N; j++)
      A[i][j] = i + j + 1;
  }
}

static int mvt(void) {
  int i, j;

#pragma scop
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      x1[i] += A[i][j] * y1[j];
    }
  for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
      x2[i] += A[j][i] * y2[j];
#pragma endscop

  return x1[0] + x2[0];
}

int main(void) {
  init_array();
  return mvt();
}
