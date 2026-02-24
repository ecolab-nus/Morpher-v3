#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int *x2;
int **A;
int *y_2;

int mvt_unroll() {
    int i = 1, j;
    int n = *N;

    int sum = 0;
    for (j = 1; j + 1 < n-1; j += 2) {
               #ifdef CGRA_COMPILER
           please_map_me();
           #endif
        x2[i] = x2[i] + A[j][i] * y_2[j];
        x2[i] = x2[i] + A[j+1][i] * y_2[j+1];
    }

    for (; j < n - 1; j++) {
        x2[i] = x2[i] + A[j][i] * y_2[j];
    }

    return sum;
}

int main() {
    return mvt_unroll();
}
