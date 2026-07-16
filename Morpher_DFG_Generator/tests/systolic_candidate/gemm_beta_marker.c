#define N 16

extern void please_map_me(void);

void gemm_beta_marker(float *__restrict__ C,
                      const float *__restrict__ A,
                      const float *__restrict__ B,
                      float beta) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j)
      C[i * N + j] *= beta;
    for (int k = 0; k < N; ++k)
      for (int j = 0; j < N; ++j) {
        please_map_me();
        C[i * N + j] += A[i * N + k] * B[k * N + j];
      }
  }
}
