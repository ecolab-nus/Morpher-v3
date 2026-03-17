#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *y;
volatile int *tmp;
int *c;

int atax_unroll_4() {
    int i;
    int j;
    int n = *N;

    int sum = 0;
    for (j = 1; j+3 < n-1; j+=4) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        y[j] = y[j] + A[i][j] * tmp[i];
        y[j+1] = y[j+1] + A[i][j+1] * tmp[i];
        y[j+2] = y[j+2] + A[i][j+2] * tmp[i];
        y[j+3] = y[j+3] + A[i][j+3] * tmp[i];
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
    atax_unroll_4();
    return 0;
}
