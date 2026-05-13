/* MachSuite: spmv/ellpack — sparse matrix-vector multiply in ELLPACK format.
   Integer version: avoids loop-carried float phi nodes.
   CGRA kernel: inner j loop; vec values pre-gathered to avoid integer-
   from-memory loads inside the kernel. */

#include <stdio.h>
#include <stdint.h>

#define N   64
#define L   64

int32_t nzval[N*L];
int32_t cols[N*L];
int32_t vec[N];
int32_t out[N];

/* Pre-gathered vec: vec_gathered[j+i*L] = vec[cols[j+i*L]] */
int32_t vec_gathered[N*L];

__attribute__((noinline))
void spmv_ellpack() {
    int i, j;
    int32_t Si;

    for (i = 0; i < N; i++) {
        int32_t sum = out[i];
        for (j = 0; j < L; j++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            int k = j + i*L;
            Si   = nzval[k] * vec_gathered[k];
            sum += Si;
        }
        out[i] = sum;
    }
}

int main() {
    int i, j;
    for (i = 0; i < N; i++) {
        out[i] = 0;
        for (j = 0; j < L; j++) {
            cols[j + i*L]  = (i + j) % N;
            nzval[j + i*L] = j + 1;
        }
    }
    for (i = 0; i < N; i++)
        vec[i] = i + 1;
    /* Pre-gather */
    for (i = 0; i < N; i++)
        for (j = 0; j < L; j++)
            vec_gathered[j + i*L] = vec[cols[j + i*L]];

    spmv_ellpack();

    for (i = 0; i < 4; i++)
        printf("out[%d]=%d\n", i, out[i]);
    return 0;
}
