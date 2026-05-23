#include <stdio.h>

#define NR 10
#define NQ 8
#define NP 128
#define IDX_A(r, q, p) (((r) * NQ + (q)) * NP + (p))
#define IDX_C(s, p) ((s) * NP + (p))

static int A[NR * NQ * NP];
static int C4[NP * NP];
static int sum[NP];

static void init_array(void) {
  int r, q, p, s;

  for (r = 0; r < NR; r++)
    for (q = 0; q < NQ; q++)
      for (p = 0; p < NP; p++)
        A[IDX_A(r, q, p)] = r + q + p + 1;

  for (s = 0; s < NP; s++)
    for (p = 0; p < NP; p++)
      C4[IDX_C(s, p)] = s + p + 1;
}

static int doitgen(void) {
  int r, q, p, s;

#pragma scop
  for (r = 0; r < NR; r++)
    for (q = 0; q < NQ; q++) {
      for (p = 0; p < NP; p++) {
        sum[p] = 0;
        for (s = 0; s < NP; s++) {
#ifdef CGRA_COMPILER
          please_map_me();
#endif
          sum[p] += A[IDX_A(r, q, s)] * C4[IDX_C(s, p)];
        }
      }
      for (p = 0; p < NP; p++)
        A[IDX_A(r, q, p)] = sum[p];
    }
#pragma endscop

  return A[0];
}

int main(void) {
  init_array();
  return doitgen();
}
