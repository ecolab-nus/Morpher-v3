#define N 16

void matvec(float * __restrict__ y,
            const float * __restrict__ A,
            const float * __restrict__ x) {
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++)
            y[i] += A[i*N + k] * x[k];
}
