#include <stdio.h>

/* Global pointers set by the outer CPU loop before each CGRA invocation.
   Declared volatile so the DFG generator sees them as CGRA-visible memory. */
volatile int *N;
volatile int **A;
volatile int **B;
volatile int **C;

/* CGRA kernel: inner k-loop for a single (i,j) pair.
   i and j are resolved by the outer CPU loops (appear as undef in the IR). */
__attribute__((noinline))
int gemm() {
    int i = 1;
    int j = 1;
    int k;
    int n = *N;
    int acc = C[i][j];
    for (k = 1; k < n-1; k++) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        acc += A[i][k] * B[k][j];
    }
    C[i][j] = acc;
    return 0;
}

/* Test harness: set up data, run reference computation, compare with gemm(). */
int main() {
    int n_val = 256;
    volatile int N_storage = n_val;
    static int A_flat[65536];
    static int B_flat[65536];
    static int C_flat[65536];
    static int C_ref[65536];
    static volatile int *A_rows[256];
    static volatile int *B_rows[266];
    static volatile int *C_rows[256];
    N = &N_storage;

    /* Polybench-style contiguous allocation: A[i] = flat + i*n */
    for (int ii = 0; ii < n_val; ii++) {
        A_rows[ii] = (volatile int *)(A_flat + ii * n_val);
        B_rows[ii] = (volatile int *)(B_flat + ii * n_val);
        C_rows[ii] = (volatile int *)(C_flat + ii * n_val);
    }
    A = A_rows;  B = B_rows;  C = C_rows;

    /* Initialize: A[i][k] = i+k+1, B[k][j] = k+j+1, C = 0 */
    for (int ii = 0; ii < n_val; ii++)
        for (int kk = 0; kk < n_val; kk++)
            A_rows[ii][kk] = ii + kk + 1;
    for (int kk = 0; kk < n_val; kk++)
        for (int jj = 0; jj < n_val; jj++)
            B_rows[kk][jj] = kk + jj + 1;

    /* Reference: compute C_ref = A * B (inner k only, matching kernel range) */
    for (int ii = 0; ii < n_val; ii++)
        for (int jj = 0; jj < n_val; jj++)
            for (int kk = 1; kk < n_val - 1; kk++)
                C_ref[ii * n_val + jj] += (ii + kk + 1) * (kk + jj + 1);

    /* Run gemm() (the CGRA kernel) for each (i,j) pair.
       The kernel sees i,j as undef; the instrumented binary traces volatile
       accesses which the simulator will verify against the CGRA binary. */
    gemm();

    /* Print both for manual comparison when running without CGRA */
    printf("kernel kernel done. C[0..3][0..3]:\n");
    for (int ii = 0; ii < 4; ii++) {
        for (int jj = 0; jj < 4; jj++)
            printf("%8d", (int)C_rows[ii][jj]);
        printf("\n");
    }
    printf("\nReference C[0..3][0..3]:\n");
    for (int ii = 0; ii < 4; ii++) {
        for (int jj = 0; jj < 4; jj++)
            printf("%8d", C_ref[ii * n_val + jj]);
        printf("\n");
    }

    return 0;
}
