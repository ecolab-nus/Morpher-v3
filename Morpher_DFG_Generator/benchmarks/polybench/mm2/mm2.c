#include <stdio.h>

#define NI 64
#define NJ 64
#define NK 64
#define NL 64

static int A[NI][NK];
static int B[NK][NJ];
static int C[NJ][NL];
static int D[NI][NL];
static int tmp[NI][NJ];
static int alpha = 3;
static int beta = 5;

static void init_array(void) {
  int i, j;
  for (i = 0; i < NI; i++)
    for (j = 0; j < NK; j++)
      A[i][j] = i + j + 1;
  for (i = 0; i < NK; i++)
    for (j = 0; j < NJ; j++)
      B[i][j] = i - j + 3;
  for (i = 0; i < NJ; i++)
    for (j = 0; j < NL; j++)
      C[i][j] = i + 2 * j + 1;
  for (i = 0; i < NI; i++)
    for (j = 0; j < NL; j++)
      D[i][j] = (i == j) ? 2 : 1;
}

static int mm2(void) {
  int i, j, k;

#pragma scop
  for (i = 0; i < NI; i++)
    for (j = 0; j < NJ; j++) {
      tmp[i][j] = 0;
      for (k = 0; k < NK; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        tmp[i][j] += alpha * A[i][k] * B[k][j];
      }
    }

  for (i = 0; i < NI; i++)
    for (j = 0; j < NL; j++) {
      D[i][j] *= beta;
      for (k = 0; k < NJ; k++)
        D[i][j] += tmp[i][k] * C[k][j];
    }
#pragma endscop

  return D[0][0];
}

int main(void) {
  init_array();
  return mm2();
}
