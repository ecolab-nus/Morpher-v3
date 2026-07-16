#define NI 16
#define NJ 4
#define NK 4

extern void please_map_me(void);

void gemm_ws_marker(int *__restrict__ C,
                    const int *__restrict__ A,
                    const int *__restrict__ B) {
  for (int i = 0; i < NI; ++i)
    for (int j = 0; j < NJ; ++j)
      for (int k = 0; k < NK; ++k) {
        please_map_me();
        C[i * NJ + j] += A[i * NK + k] * B[k * NJ + j];
      }
}
