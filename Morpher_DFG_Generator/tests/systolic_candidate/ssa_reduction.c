#define N 16

// Per-row dot product: rowsum[i] = sum_k A[i,k] * B[i,k].
// Pure SSA accumulator (output stored once per i, after the k loop).
void ssa_reduction(float * __restrict__ rowsum,
                   const float * __restrict__ A,
                   const float * __restrict__ B) {
    for (int i = 0; i < N; i++) {
        float acc = 0.0f;
        for (int k = 0; k < N; k++)
            acc += A[i*N + k] * B[i*N + k];
        rowsum[i] = acc;
    }
}
