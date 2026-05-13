#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int *x2;
int **A;
int *y_2;

int mvt() {
    int i = 1;
    int j;
    int n = *N;

    int sum = 0;
    for (j = 1; j < n-1; j++) {
               #ifdef CGRA_COMPILER
           please_map_me();
           #endif
        x2[i] = x2[i] + A[j][i] * y_2[j];
    }

    return sum;
}

int main() {
    int n_val = 66;
    volatile int N_storage = n_val;
    static int A_flat[66 * 66];
    static int *A_rows[66];
    static int x2_data[66];
    static int y2_data[66];
    N = &N_storage;

    for (int ii = 0; ii < n_val; ii++) {
        A_rows[ii] = A_flat + ii * n_val;
        x2_data[ii] = ii;
        y2_data[ii] = ii + 1;
        for (int jj = 0; jj < n_val; jj++) {
            A_rows[ii][jj] = ii + jj + 1;
        }
    }
    A = A_rows;
    x2 = x2_data;
    y_2 = y2_data;

    mvt();
    return 0;
}
