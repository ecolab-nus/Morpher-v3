#define N 16

// 5-point Jacobi stencil over a single time step. No reduction loop, but the
// body is stencil-shape (each output cell = sum of 5 input cells). Detector
// should report verdict=maybe with stencil_pairs > 0.
void jacobi_2d(float * __restrict__ out, const float * __restrict__ in) {
    for (int i = 1; i < N - 1; i++)
        for (int j = 1; j < N - 1; j++)
            out[i*N + j] = 0.2f * (in[i*N + j]
                                  + in[(i-1)*N + j]
                                  + in[(i+1)*N + j]
                                  + in[i*N + (j-1)]
                                  + in[i*N + (j+1)]);
}
