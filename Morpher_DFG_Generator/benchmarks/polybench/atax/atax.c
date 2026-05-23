#include <stdio.h>

#define NX 64
#define NY 64

static int A[NX][NY];
static int x[NY];
static int y[NY];
static int tmp[NX];

static void init_array(void) {
  int i, j;
  for (i = 0; i < NX; i++) {
    tmp[i] = 0;
    for (j = 0; j < NY; j++)
      A[i][j] = i + j + 1;
  }
  for (j = 0; j < NY; j++) {
    x[j] = j + 1;
    y[j] = 0;
  }
}

static int atax(void) {
  int i, j;

#pragma scop
  for (i = 0; i < NY; i++)
    y[i] = 0;
  for (i = 0; i < NX; i++) {
    tmp[i] = 0;
    for (j = 0; j < NY; j++)
      tmp[i] += A[i][j] * x[j];
    for (j = 0; j < NY; j++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      y[j] += A[i][j] * tmp[i];
    }
  }
#pragma endscop

  return y[0];
}

int main(void) {
  init_array();
  return atax();
}
