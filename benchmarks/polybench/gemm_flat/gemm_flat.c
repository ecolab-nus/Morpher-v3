#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
volatile int **A;
volatile int *B;   /* flat int* instead of int** */
volatile int **C;


int gemm_flat() {
    int i, j, k;
    int n = *N;
    int alpha = 20;
    int tmp;

    int sum = 0;
    for (k = 1; k < n-1; k++) {
                #ifdef CGRA_COMPILER
           please_map_me();
           #endif
    C[i][j] += A[i][k] * B[k];   /* B[k] instead of B[k][j] */
    }

    return sum;
}

int main() {
    return gemm_flat();
}
