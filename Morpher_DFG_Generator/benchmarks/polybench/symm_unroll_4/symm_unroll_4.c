#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int **A;
int **B;
int **C;

int symm_unroll_4() {
    int i, j, k, alpha = 1, acc = 100;
    int n = *N;

    int sum = 0;
    for (k = 1; k+3 < n-1; k+=4) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        C[k][j] += alpha * A[k][i] * B[i][j];
	    acc += B[k][j] * A[k][i];

        C[k+1][j] += alpha * A[k+1][i] * B[i][j];
	    acc += B[k+1][j] * A[k+1][i];

        C[k+2][j] += alpha * A[k+2][i] * B[i][j];
	    acc += B[k+2][j] * A[k+2][i];

        C[k+3][j] += alpha * A[k+3][i] * B[i][j];
	    acc += B[k+3][j] * A[k+3][i];
    }

    return sum;
}

int main() {
    symm_unroll_4();
    return 0;
}
