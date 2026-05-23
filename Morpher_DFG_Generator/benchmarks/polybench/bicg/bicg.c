#include <stdio.h>

#define NX 64
#define NY 64

static int A[NX][NY];
static int p[NY];
static int r[NX];
static int s[NY];
static int q[NX];

static void init_array(void) {
  int i, j;
  for (i = 0; i < NX; i++) {
    r[i] = i + 2;
    q[i] = 0;
    for (j = 0; j < NY; j++)
      A[i][j] = i + j + 1;
  }
  for (j = 0; j < NY; j++) {
    p[j] = j + 1;
    s[j] = 0;
  }
}

static int bicg(void) {
  int i, j;

#pragma scop
  for (i = 0; i < NY; i++)
    s[i] = 0;
  for (i = 0; i < NX; i++) {
    q[i] = 0;
    for (j = 0; j < NY; j++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      s[j] += r[i] * A[i][j];
      q[i] += A[i][j] * p[j];
    }
  }
#pragma endscop

  return q[0] + s[0];
}

int main(void) {
  init_array();
  return bicg();
}
