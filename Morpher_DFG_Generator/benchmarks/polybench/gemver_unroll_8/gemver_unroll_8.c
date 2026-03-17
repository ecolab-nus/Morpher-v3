#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
volatile int **A;
volatile int *u1;
volatile int *v1;
volatile int *u2;
volatile int *v2;

int *c;

int gemver_unroll_8() {
    int i,j;
    int n = *N;

    int sum = 0;
    for (j = 1; j+7 < n-1; j+=8) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        A[i][j] = A[i][j] + u1[i] * v1[j] + u2[i] * v2[j];

        A[i][j+1] = A[i][j+1] + u1[i] * v1[j+1] + u2[i] * v2[j+1];

        A[i][j+2] = A[i][j+2] + u1[i] * v1[j+2] + u2[i] * v2[j+2];

        A[i][j+3] = A[i][j+3] + u1[i] * v1[j+3] + u2[i] * v2[j+3];

        A[i][j+4] = A[i][j+4] + u1[i] * v1[j+4] + u2[i] * v2[j+4];

        A[i][j+5] = A[i][j+5] + u1[i] * v1[j+5] + u2[i] * v2[j+5];

        A[i][j+6] = A[i][j+6] + u1[i] * v1[j+6] + u2[i] * v2[j+6];

        A[i][j+7] = A[i][j+7] + u1[i] * v1[j+7] + u2[i] * v2[j+7];

    }

    return sum;
}

int main() {
    gemver_unroll_8();
    return 0;
}
