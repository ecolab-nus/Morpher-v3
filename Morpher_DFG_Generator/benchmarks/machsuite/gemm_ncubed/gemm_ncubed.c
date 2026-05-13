/* MachSuite: gemm/ncubed — naive O(N^3) matrix multiplication.
   CGRA kernel: innermost k loop accumulates one output element. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define N 128

int m1[N*N], m2[N*N], prod[N*N];

__attribute__((noinline))
void gemm_ncubed() {
    int i, j, k;
    int i_col, k_col;
    int mult;

    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            i_col = i * N;
            int sum = 0;
            for (k = 0; k < N; k++) {
                #ifdef CGRA_COMPILER
                please_map_me();
                #endif
                k_col = k * N;
                mult = m1[i_col + k] * m2[k_col + j];
                sum += mult;
            }
            prod[i_col + j] = sum;
        }
    }
}

int main() {
    int i, j;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            m1[i*N+j] = i + j + 1;
            m2[i*N+j] = i - j + 2;
        }
    gemm_ncubed();
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++)
            printf("%6d", prod[i*N+j]);
        printf("\n");
    }
    return 0;
}
