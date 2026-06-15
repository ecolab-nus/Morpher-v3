#define N 16

void stencil(float * __restrict__ out, const float * __restrict__ in) {
    for (int i = 1; i < N - 1; i++)
        for (int j = 1; j < N - 1; j++)
            out[i*N + j] = in[(i-1)*N + j]
                         + in[(i+1)*N + j]
                         + in[i*N + (j-1)]
                         + in[i*N + (j+1)];
}
