#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **A;
int **C;
int syrk_unroll() {
    int i, j ,k, alpha=10;
    int n = *N;

    int sum = 0;
    for (k = 1; k+1 < n-1; k+=2) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        C[i][j] += alpha * A[i][k] * A[j][k];
        C[i][j] += alpha * A[i][k+1] * A[j][k+1];
    }

    return sum;
}

int main() {
    syrk_unroll();
    return 0;
}
