#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **B;
int **A;


int trmm_unroll_8() {
    int i, j, k, alpha = 5;
    int n = *N;

    int sum = 0;
    for (i = 1; i+7 < n-1; i+=8) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        B[i][j] += alpha * A[i][k] * B[j][k];
        B[i+1][j] += alpha * A[i+1][k] * B[j][k];
        B[i+2][j] += alpha * A[i+2][k] * B[j][k];
        B[i+3][j] += alpha * A[i+3][k] * B[j][k];
        B[i+4][j] += alpha * A[i+4][k] * B[j][k];
        B[i+5][j] += alpha * A[i+5][k] * B[j][k];
        B[i+6][j] += alpha * A[i+6][k] * B[j][k];
        B[i+7][j] += alpha * A[i+7][k] * B[j][k];
    }

    return sum;
}

int main() {
    trmm_unroll_8();
    return 0;
}
