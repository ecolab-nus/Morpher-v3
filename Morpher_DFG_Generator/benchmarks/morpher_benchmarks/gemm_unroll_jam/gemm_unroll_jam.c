// Feasibility kernel for output-stationary GEMM lowering through the normal
// Morpher pipeline. The i/j loops are unrolled-and-jammed by 4x4 while the k
// reduction stays rolled and is the only loop marked for CGRA mapping.

#define N 64
#define TILE 4

static int A[N][N];
static int B[N][N];
static int C[N][N];

__attribute__((noinline))
void gemm_unroll_jam(int alpha, int beta) {
  int ii, jj, k;
  for (ii = 0; ii < N; ii += TILE) {
    for (jj = 0; jj < N; jj += TILE) {
      int c00 = 0, c01 = 0, c02 = 0, c03 = 0;
      int c10 = 0, c11 = 0, c12 = 0, c13 = 0;
      int c20 = 0, c21 = 0, c22 = 0, c23 = 0;
      int c30 = 0, c31 = 0, c32 = 0, c33 = 0;

      for (k = 0; k < N; ++k) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        const int a0 = A[ii + 0][k];
        const int a1 = A[ii + 1][k];
        const int a2 = A[ii + 2][k];
        const int a3 = A[ii + 3][k];
        const int b0 = B[k][jj + 0];
        const int b1 = B[k][jj + 1];
        const int b2 = B[k][jj + 2];
        const int b3 = B[k][jj + 3];

        c00 += a0 * b0; c01 += a0 * b1; c02 += a0 * b2; c03 += a0 * b3;
        c10 += a1 * b0; c11 += a1 * b1; c12 += a1 * b2; c13 += a1 * b3;
        c20 += a2 * b0; c21 += a2 * b1; c22 += a2 * b2; c23 += a2 * b3;
        c30 += a3 * b0; c31 += a3 * b1; c32 += a3 * b2; c33 += a3 * b3;
      }

      C[ii + 0][jj + 0] = alpha * c00 + beta * C[ii + 0][jj + 0];
      C[ii + 0][jj + 1] = alpha * c01 + beta * C[ii + 0][jj + 1];
      C[ii + 0][jj + 2] = alpha * c02 + beta * C[ii + 0][jj + 2];
      C[ii + 0][jj + 3] = alpha * c03 + beta * C[ii + 0][jj + 3];
      C[ii + 1][jj + 0] = alpha * c10 + beta * C[ii + 1][jj + 0];
      C[ii + 1][jj + 1] = alpha * c11 + beta * C[ii + 1][jj + 1];
      C[ii + 1][jj + 2] = alpha * c12 + beta * C[ii + 1][jj + 2];
      C[ii + 1][jj + 3] = alpha * c13 + beta * C[ii + 1][jj + 3];
      C[ii + 2][jj + 0] = alpha * c20 + beta * C[ii + 2][jj + 0];
      C[ii + 2][jj + 1] = alpha * c21 + beta * C[ii + 2][jj + 1];
      C[ii + 2][jj + 2] = alpha * c22 + beta * C[ii + 2][jj + 2];
      C[ii + 2][jj + 3] = alpha * c23 + beta * C[ii + 2][jj + 3];
      C[ii + 3][jj + 0] = alpha * c30 + beta * C[ii + 3][jj + 0];
      C[ii + 3][jj + 1] = alpha * c31 + beta * C[ii + 3][jj + 1];
      C[ii + 3][jj + 2] = alpha * c32 + beta * C[ii + 3][jj + 2];
      C[ii + 3][jj + 3] = alpha * c33 + beta * C[ii + 3][jj + 3];
    }
  }
}

int main(void) {
  int i, j;
  for (i = 0; i < N; ++i) {
    for (j = 0; j < N; ++j) {
      A[i][j] = i + j + 1;
      B[i][j] = i - j + 3;
      C[i][j] = i * j + 1;
    }
  }
  gemm_unroll_jam(3, 5);
  return C[0][0];
}
