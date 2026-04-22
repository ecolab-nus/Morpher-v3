#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *y;
volatile int *tmp;
int *c;

int atax() {
    int i = 1;
    int j;
    int n = *N;

    int sum = 0;
    for (j = 1; j < n-1; j++) {
               #ifdef CGRA_COMPILER
           please_map_me();
           #endif
        y[j] = y[j] + A[i][j] * tmp[i];
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
    int n_val = 8;
    volatile int N_storage = n_val;
    static int A_flat[64];
    static volatile int *A_rows[8];
    static volatile int y_data[8];
    static volatile int tmp_data[8];
    N = &N_storage;

    for (int ii = 0; ii < n_val; ii++) {
        A_rows[ii] = (volatile int *)(A_flat + ii * n_val);
        tmp_data[ii] = ii + 1;
        y_data[ii] = ii;
        for (int jj = 0; jj < n_val; jj++) {
            A_rows[ii][jj] = ii + jj + 1;
        }
    }
    A = A_rows;
    y = y_data;
    tmp = tmp_data;

    atax();
    return 0;
}
