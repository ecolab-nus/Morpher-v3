#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **A;
int **C;
int syrk_unroll_8() {
    int i, j ,k, alpha=10;
    int n = *N;

    int sum = 0;
    for (k = 1; k+7 < n-1; k+=8) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        C[i][j] += alpha * A[i][k] * A[j][k];
        C[i][j] += alpha * A[i][k+1] * A[j][k+1];
        C[i][j] += alpha * A[i][k+2] * A[j][k+2];
        C[i][j] += alpha * A[i][k+3] * A[j][k+3];
        C[i][j] += alpha * A[i][k+4] * A[j][k+4];
        C[i][j] += alpha * A[i][k+5] * A[j][k+5];
        C[i][j] += alpha * A[i][k+6] * A[j][k+6];
        C[i][j] += alpha * A[i][k+7] * A[j][k+7];

    }

    return sum;
}

int main() {
    syrk_unroll_8();
    return 0;
}
