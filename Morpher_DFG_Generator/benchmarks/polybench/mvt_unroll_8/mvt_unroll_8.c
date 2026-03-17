#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int *x2;
int **A;
int *y_2;

int mvt_unroll_8() {
    int i = 1, j;
    int n = *N;

    int sum = 0;
    for (j = 1; j+7 < n-1; j += 8) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        x2[i] = x2[i] + A[j][i] * y_2[j];
        x2[i] = x2[i] + A[j+1][i] * y_2[j+1];
        x2[i] = x2[i] + A[j+2][i] * y_2[j+2];
        x2[i] = x2[i] + A[j+3][i] * y_2[j+3];
        x2[i] = x2[i] + A[j+4][i] * y_2[j+4];
        x2[i] = x2[i] + A[j+5][i] * y_2[j+5];
        x2[i] = x2[i] + A[j+6][i] * y_2[j+6];
        x2[i] = x2[i] + A[j+7][i] * y_2[j+7];
    }

    return sum;
}

int main() {
    return mvt_unroll_8();
}
