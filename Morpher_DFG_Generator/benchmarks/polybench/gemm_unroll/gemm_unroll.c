#include <stdio.h>

#define N_VAL 130

volatile int *N;
volatile int *a;
volatile int *b;
int *c;
volatile int **A;
volatile int **B;
volatile int **C;

int gemm_unroll() {
    int i = 0, j = 0, k;
    int n = *N;
    int alpha = 20;
    int tmp;

    int sum = 0;
    for (k = 1; k+1 < n-1; k+=2) {
        #ifdef CGRA_COMPILER
           please_map_me();
        #endif
        tmp = alpha * A[i][k];
        tmp = tmp * B[k][j];
        C[i][j] += tmp ;

        tmp = alpha * A[i][k+1];
        tmp = tmp * B[k+1][j];
        C[i][j] += tmp ;
    }

    return sum;
}

static int n_val = N_VAL;
static int A_data[N_VAL][N_VAL];
static int B_data[N_VAL][N_VAL];
static int C_data[N_VAL][N_VAL];
static volatile int *A_rows[N_VAL];
static volatile int *B_rows[N_VAL];
static volatile int *C_rows[N_VAL];

int main() {
    int r, s;
    N = &n_val;

    for (r = 0; r < N_VAL; r++) {
        for (s = 0; s < N_VAL; s++) {
            A_data[r][s] = r * N_VAL + s + 1;
            B_data[r][s] = r * N_VAL + s + 1;
            C_data[r][s] = 0;
        }
        A_rows[r] = A_data[r];
        B_rows[r] = B_data[r];
        C_rows[r] = C_data[r];
    }

    A = A_rows;
    B = B_rows;
    C = (volatile int **)C_rows;

    gemm_unroll();
    return 0;
}
