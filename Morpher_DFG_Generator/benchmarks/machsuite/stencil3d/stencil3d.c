/* MachSuite: stencil/stencil3d — 3-D 7-point stencil.
   Dimensions reduced so each array < 2048 bytes (banksize limit).
   Coefficients are compile-time #defines (avoids non-GEP global scalar loads).
   volatile arrays prevent LLVM loop-strength-reduction from using inttoptr. */

#include <stdio.h>
#include <stdint.h>

#define height_size  4
#define col_size     8
#define row_size     8
#define SIZE         (row_size * col_size * height_size)  /* = 256 * 4 = 1024 B */

#define C0_VAL  2
#define C1_VAL  1

volatile int32_t orig[SIZE];
volatile int32_t sol[SIZE];

__attribute__((noinline))
void stencil3d() {
    int i, j, k;
    int32_t sum0, sum1;

    for (i = 1; i < height_size - 1; i++) {
        for (j = 1; j < col_size - 1; j++) {
            for (k = 1; k < row_size - 1; k++) {
                #ifdef CGRA_COMPILER
                please_map_me();
                #endif
                int idx = k + row_size * (j + col_size * i);
                sum0 = orig[idx];
                sum1 = orig[idx + row_size * col_size]  /* i+1 */
                     + orig[idx - row_size * col_size]  /* i-1 */
                     + orig[idx + row_size]             /* j+1 */
                     + orig[idx - row_size]             /* j-1 */
                     + orig[idx + 1]                    /* k+1 */
                     + orig[idx - 1];                   /* k-1 */
                sol[idx] = C0_VAL * sum0 + C1_VAL * sum1;
            }
        }
    }
}

int main() {
    int i;
    for (i = 0; i < SIZE; i++)
        orig[i] = i % 10 + 1;
    stencil3d();
    printf("sol[%d]=%d\n", 1 + row_size*(1 + col_size*1),
                            (int)sol[1 + row_size*(1 + col_size*1)]);
    return 0;
}
