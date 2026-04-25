/* MachSuite: stencil/stencil2d — 2-D 3x3 stencil convolution.
   CGRA kernel: innermost k2 loop — one multiply-accumulate per filter column. */

#include <stdio.h>
#include <stdint.h>

#define col_size 16
#define row_size  8
#define f_size    9

int32_t orig[row_size * col_size];
int32_t sol[row_size * col_size];
int32_t filter[f_size];

__attribute__((noinline))
void stencil2d() {
    int r, c, k1, k2;
    int32_t mul;

    for (r = 0; r < row_size - 2; r++) {
        for (c = 0; c < col_size - 2; c++) {
            int32_t temp = 0;
            for (k1 = 0; k1 < 3; k1++) {
                for (k2 = 0; k2 < 3; k2++) {
                    #ifdef CGRA_COMPILER
                    please_map_me();
                    #endif
                    mul  = filter[k1*3 + k2] * orig[(r+k1)*col_size + c+k2];
                    temp += mul;
                }
            }
            sol[r*col_size + c] = temp;
        }
    }
}

int main() {
    int i;
    for (i = 0; i < row_size * col_size; i++)
        orig[i] = i % 16 + 1;
    for (i = 0; i < f_size; i++)
        filter[i] = i + 1;
    stencil2d();
    printf("sol[0]=%d sol[1]=%d\n", sol[0], sol[1]);
    return 0;
}
