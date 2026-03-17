#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *y;
volatile int *tmp;
int *c;

int atax_unroll_8() {
    int i;
    int j;
    int n = *N;

    int sum = 0;
    for (j = 1; j+7 < n-1; j+=8) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        y[j] = y[j] + A[i][j] * tmp[i];
        y[j+1] = y[j+1] + A[i][j+1] * tmp[i];
        y[j+2] = y[j+2] + A[i][j+2] * tmp[i];
        y[j+3] = y[j+3] + A[i][j+3] * tmp[i];
        y[j+4] = y[j+4] + A[i][j+4] * tmp[i];
        y[j+5] = y[j+5] + A[i][j+5] * tmp[i];
        y[j+6] = y[j+6] + A[i][j+6] * tmp[i];
        y[j+7] = y[j+7] + A[i][j+7] * tmp[i];
    }

//     #pragma scop
//   for (i = 0; i < _PB_NY; i++)
//     y[i] = 0;
//   for (i = 0; i < _PB_NX; i++)
//     {
//       tmp[i] = 0;
//       for (j = 0; j < _PB_NY; j++)
// 	tmp[i] = tmp[i] + A[i][j] * x[j];
//       for (j = 0; j < _PB_NY; j++)
// 	y[j] = y[j] + A[i][j] * tmp[i];
//     }
// #pragma endscop

    return sum;
}

int main() {
    atax_unroll_8();
    return 0;
}
