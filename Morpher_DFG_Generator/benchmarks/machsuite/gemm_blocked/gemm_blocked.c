/* MachSuite: gemm/blocked — cache-blocked matrix multiplication.
   CGRA kernel: innermost j loop (block_size iterations) within the tiled loops. */

#include <stdio.h>
#include <stdint.h>

#define row_size   16
#define col_size   16
#define N          (row_size * col_size)
#define block_size 16

int m1[N], m2[N], prod[N];

__attribute__((noinline))
void gemm_blocked() {
    int i, k, j, jj, kk;
    int i_row, k_row;
    int temp_x, mul;

    for (jj = 0; jj < row_size; jj += block_size) {
        for (kk = 0; kk < row_size; kk += block_size) {
            for (i = 0; i < row_size; i++) {
                for (k = 0; k < block_size; k++) {
                    i_row = i * row_size;
                    k_row = (k + kk) * row_size;
                    temp_x = m1[i_row + k + kk];
                    for (j = 0; j < block_size; j++) {
                        #ifdef CGRA_COMPILER
                        please_map_me();
                        #endif
                        mul = temp_x * m2[k_row + j + jj];
                        prod[i_row + j + jj] += mul;
                    }
                }
            }
        }
    }
}

int main() {
    int i;
    for (i = 0; i < N; i++) {
        m1[i] = i % row_size + 1;
        m2[i] = i / row_size + 1;
        prod[i] = 0;
    }
    gemm_blocked();
    printf("prod[0]=%d\n", prod[0]);
    return 0;
}
