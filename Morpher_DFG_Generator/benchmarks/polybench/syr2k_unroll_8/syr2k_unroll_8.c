#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **A;
int **B;
int **C;
int syr2k_unroll_8() {
    int i, j, k, alpha = 2;
    int n = *N;

    int sum = 0;
    for (k = 1; k+7 < n-1; k+=8) {
        #ifdef CGRA_COMPILER
            please_map_me();
        #endif
        C[i][j] += alpha * A[i][k] * B[j][k];
        C[i][j] += alpha * B[i][k] * A[j][k];

        C[i][j] += alpha * A[i][k+1] * B[j][k+1];
        C[i][j] += alpha * B[i][k+1] * A[j][k+1];

        C[i][j] += alpha * A[i][k+2] * B[j][k+2];
        C[i][j] += alpha * B[i][k+2] * A[j][k+2];

        C[i][j] += alpha * A[i][k+3] * B[j][k+3];
        C[i][j] += alpha * B[i][k+3] * A[j][k+3];

        C[i][j] += alpha * A[i][k+4] * B[j][k+4];
        C[i][j] += alpha * B[i][k+4] * A[j][k+4];

        C[i][j] += alpha * A[i][k+5] * B[j][k+5];
        C[i][j] += alpha * B[i][k+5] * A[j][k+5];

        C[i][j] += alpha * A[i][k+6] * B[j][k+6];
        C[i][j] += alpha * B[i][k+6] * A[j][k+6];

        C[i][j] += alpha * A[i][k+7] * B[j][k+7];
        C[i][j] += alpha * B[i][k+7] * A[j][k+7];
    }

    return sum;
}

int main() {
    syr2k_unroll_8();
    return 0;
}
