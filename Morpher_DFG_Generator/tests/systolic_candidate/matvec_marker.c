#define N 16

extern void please_map_me(void);

void matvec_marker(int *__restrict__ y,
                   const int *__restrict__ A,
                   const int *__restrict__ x) {
  for (int i = 0; i < N; ++i)
    for (int k = 0; k < N; ++k) {
      please_map_me();
      y[i] += A[i * N + k] * x[k];
    }
}
