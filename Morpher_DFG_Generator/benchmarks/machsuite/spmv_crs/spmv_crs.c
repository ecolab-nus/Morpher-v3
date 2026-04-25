/* MachSuite: spmv/crs — sparse matrix-vector multiply in CRS format.
   Integer version: avoids loop-carried float phi nodes.
   Uses fixed row width (padded) so kernel has a static loop bound. */

#include <stdio.h>
#include <stdint.h>

#define N            16
#define NNZ_PER_ROW   4   /* fixed / padded row width */
#define NNZ          (N * NNZ_PER_ROW)

int32_t val[NNZ];
int32_t cols[NNZ];
int32_t rowDelimiters[N+1];
int32_t vec[N];
int32_t out[N];

/* Pre-gathered vec values, one slot per (row, nonzero) pair. */
int32_t vec_gathered[NNZ];

__attribute__((noinline))
void spmv_crs() {
    int i, j;
    int32_t sum, Si;

    for (i = 0; i < N; i++) {
        sum = 0;
        for (j = 0; j < NNZ_PER_ROW; j++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            int k = i*NNZ_PER_ROW + j;
            Si  = val[k] * vec_gathered[k];
            sum = sum + Si;
        }
        out[i] = sum;
    }
}

int main() {
    int i, j, idx;
    idx = 0;
    for (i = 0; i < N; i++) {
        rowDelimiters[i] = idx;
        for (j = 0; j < NNZ_PER_ROW; j++) {
            cols[idx] = (i + j) % N;
            val[idx]  = i + j + 1;
            idx++;
        }
    }
    rowDelimiters[N] = idx;
    for (i = 0; i < N; i++)
        vec[i] = i + 1;
    /* Gather */
    for (i = 0; i < N; i++)
        for (j = 0; j < NNZ_PER_ROW; j++)
            vec_gathered[i*NNZ_PER_ROW + j] = vec[cols[i*NNZ_PER_ROW + j]];

    spmv_crs();

    for (i = 0; i < 4; i++)
        printf("out[%d]=%d\n", i, out[i]);
    return 0;
}
