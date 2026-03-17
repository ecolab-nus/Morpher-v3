#include <stdio.h>

volatile int * N;
volatile int *a;
volatile int *b;
int *c;
int **A;
int *tmp;
int *x;
int *y;
int **B;
int gesummv_unroll_4() {
    int i = 1, j;
    int n = *N;

    int sum = 0;
    for (j = 1; j+3 < n-1; j += 4) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        tmp[i] = A[i][j] * x[j] + tmp[i];
	    y[i] = B[i][j] * x[j] + y[i];

        tmp[i] = A[i][j+1] * x[j+1] + tmp[i];
	    y[i] = B[i][j+1] * x[j+1] + y[i];

        tmp[i] = A[i][j+2] * x[j+2] + tmp[i];
	    y[i] = B[i][j+2] * x[j+2] + y[i];

        tmp[i] = A[i][j+3] * x[j+3] + tmp[i];
	    y[i] = B[i][j+3] * x[j+3] + y[i];
    }

    return sum;
}

int main() {
    return gesummv_unroll_4();
}
