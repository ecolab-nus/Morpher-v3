#define N 16

void indirect(float * __restrict__ y,
              const float * __restrict__ A,
              const int * __restrict__ idx) {
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++)
            y[i] += A[idx[k]] * A[idx[k]];
}
