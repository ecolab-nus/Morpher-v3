#define N 10

void gemm_n10(float *__restrict__ C,
              const float *__restrict__ A,
              const float *__restrict__ B) {
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j)
      for (int k = 0; k < N; ++k)
        C[i * N + j] += A[i * N + k] * B[k * N + j];
}
