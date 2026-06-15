#define N 16

void please_map_me(void);

void gemm_with_marker(float * __restrict__ C,
                      const float * __restrict__ A,
                      const float * __restrict__ B) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            for (int k = 0; k < N; k++) {
                please_map_me();
                C[i*N + j] += A[i*N + k] * B[k*N + j];
            }
}
