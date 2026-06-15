#define N 16

// Polybench-shape bicg: shared inner loop with TWO distinct output stores.
// s[j] += A[i,j] * r[i]   (parallel: j; reduction: i)
// q[i] += A[i,j] * p[j]   (parallel: i; reduction: j)
// Together the inner loop is multi-store; outputDeps = {i, j}; both i and j
// are parallel in the union sense. The detector should report verdict=maybe
// (no clean reduction loop in the union).
void bicg(float * __restrict__ s,
          float * __restrict__ q,
          const float * __restrict__ A,
          const float * __restrict__ r,
          const float * __restrict__ p) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            s[j] += A[i*N + j] * r[i];
            q[i] += A[i*N + j] * p[j];
        }
}
