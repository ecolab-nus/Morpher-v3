/* MachSuite: stencil/stencil2d — 2-D 3x3 stencil convolution.
   CGRA kernel: innermost k2 loop — one multiply-accumulate per filter column. */

#include <stdio.h>
#include <stdint.h>

#define col_size 128
#define row_size  32
#define f_size    9

int32_t orig[row_size * col_size];
int32_t sol[row_size * col_size];
int32_t filter[f_size];

__attribute__((noinline))
void stencil2d() {
    int r, c;
    int32_t f0 = filter[0];
    int32_t f1 = filter[1];
    int32_t f2 = filter[2];
    int32_t f3 = filter[3];
    int32_t f4 = filter[4];
    int32_t f5 = filter[5];
    int32_t f6 = filter[6];
    int32_t f7 = filter[7];
    int32_t f8 = filter[8];

    for (r = 0; r < row_size - 2; r++) {
        for (c = 0; c < col_size - 2; c++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            int base0 = r * col_size + c;
            int base1 = base0 + col_size;
            int base2 = base1 + col_size;
            int32_t temp =
                f0 * orig[base0 + 0] +
                f1 * orig[base0 + 1] +
                f2 * orig[base0 + 2] +
                f3 * orig[base1 + 0] +
                f4 * orig[base1 + 1] +
                f5 * orig[base1 + 2] +
                f6 * orig[base2 + 0] +
                f7 * orig[base2 + 1] +
                f8 * orig[base2 + 2];
            sol[base0] = temp;
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
