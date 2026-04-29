#include <stdio.h>

#define N_VAL 258
#define FLAT_SIZE (N_VAL * N_VAL)

volatile int *N;
volatile int **A;
volatile int **B;
volatile int **C;

__attribute__((noinline))
int gemm_large() {
    int i = 1;
    int j = 1;
    int k;
    int n = *N;
    int acc = C[i][j];
    for (k = 1; k < n - 1; k++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        acc += A[i][k] * B[k][j];
    }
    C[i][j] = acc;
    return 0;
}

int main() {
    int n_val = N_VAL;
    volatile int N_storage = n_val;
    static int A_flat[FLAT_SIZE];
    static int B_flat[FLAT_SIZE];
    static int C_flat[FLAT_SIZE];
    static int C_ref[FLAT_SIZE];
    static volatile int *A_rows[N_VAL];
    static volatile int *B_rows[N_VAL];
    static volatile int *C_rows[N_VAL];
    N = &N_storage;

    for (int ii = 0; ii < n_val; ii++) {
        A_rows[ii] = (volatile int *)(A_flat + ii * n_val);
        B_rows[ii] = (volatile int *)(B_flat + ii * n_val);
        C_rows[ii] = (volatile int *)(C_flat + ii * n_val);
    }
    A = A_rows;
    B = B_rows;
    C = C_rows;

    for (int ii = 0; ii < n_val; ii++) {
        for (int kk = 0; kk < n_val; kk++) {
            A_rows[ii][kk] = ii + kk + 1;
            B_rows[ii][kk] = ii + kk + 1;
            C_rows[ii][kk] = 0;
            C_ref[ii * n_val + kk] = 0;
        }
    }

    for (int ii = 0; ii < n_val; ii++) {
        for (int jj = 0; jj < n_val; jj++) {
            for (int kk = 1; kk < n_val - 1; kk++) {
                C_ref[ii * n_val + jj] += (ii + kk + 1) * (kk + jj + 1);
            }
        }
    }

    gemm_large();

    printf("C[1][1]=%d ref=%d\n", (int)C_rows[1][1], C_ref[n_val + 1]);
    return 0;
}
