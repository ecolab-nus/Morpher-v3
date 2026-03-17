#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **A;
int **C;
int syrk_unroll_4() {
    int i, j ,k, alpha=10;
    int n = *N;

    int sum = 0;
    for (k = 1; k+3 < n-1; k+=4) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        C[i][j] += alpha * A[i][k] * A[j][k];
        C[i][j] += alpha * A[i][k+1] * A[j][k+1];
        C[i][j] += alpha * A[i][k+2] * A[j][k+2];
        C[i][j] += alpha * A[i][k+3] * A[j][k+3];

    }

    return sum;
}

int main() {
    syrk_unroll_4();
    return 0;
}
