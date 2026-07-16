// Canonical source-level GEMM candidate for the automatic systolic
// unroll-and-jam path. Unlike gemm_unroll_jam, this source is not manually
// tiled: the compiler pass must create the 4x4 steady-state body.

#define N 64

static int A[N][N];
static int B[N][N];
static int C[N][N];

#ifdef CGRA_COMPILER
extern void please_map_me(void);
#endif

__attribute__((noinline))
void gemm_systolic_candidate(int alpha, int beta) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      int accumulator = 0;
      for (int k = 0; k < N; ++k) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        accumulator += A[i][k] * B[k][j];
      }
      C[i][j] = alpha * accumulator + beta * C[i][j];
    }
  }
}

int main(void) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      A[i][j] = i + j + 1;
      B[i][j] = i - j + 3;
      C[i][j] = i * j + 1;
    }
  }
  gemm_systolic_candidate(3, 5);
  return C[0][0];
}
