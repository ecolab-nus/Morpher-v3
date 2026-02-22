/* Verification test for HoistInvariantRowPointers + FlattenDoublePointers.
 *
 * Implements the ORIGINAL double-pointer gemm computation and the TRANSFORMED
 * version side-by-side on the same input, then compares results.
 *
 * The transforms:
 *   1. HoistInvariantRowPointers: hoist A[i] row ptr to preheader
 *   2. FlattenDoublePointers:     B[k][j] → B_elem PHI advancing by N
 *   3. HoistInvariantRowPointers: hoist C[i][j] elem ptr to preheader
 *
 * All three preserve the same volatile load/store addresses, so both versions
 * must produce identical C matrices for any valid (A, B) input.
 *
 * Compile and run:
 *   gcc -O0 -o gemm_verify gemm_verify.c && ./gemm_verify
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 8

/* -------------------------------------------------------------------------
 * ORIGINAL: straight double-pointer access as in gemm.c before any pass.
 * For fixed i=I, j=J, k=1..N-2:
 *   C[i][j] += A[i][k] * B[k][j]
 * This is what the unmodified IR computes.
 * -------------------------------------------------------------------------*/
void gemm_original(int **A, int **B, int **C, int n, int I, int J) {
    for (int k = 1; k < n - 1; k++) {
        /* Original pattern:
         *   load @B          -> B base ptr
         *   gep(B, k)        -> &B[k]          (row_gep,  IV index)
         *   load B[k]        -> row ptr         (row_load, eliminated by FlattenDP)
         *   gep(B[k], j)     -> &B[k][j]        (elem_gep, eliminated by FlattenDP)
         *   load volatile    -> B[k][j]
         *   load @A          -> A base ptr
         *   gep(A, i)        -> &A[i]           (row_gep,  undef index)
         *   load A[i]        -> row ptr         (row_load, eliminated by HoistIRP)
         *   gep(A[i], k)     -> &A[i][k]        (elem_gep, IV in col)
         *   load volatile    -> A[i][k]
         *   load volatile C[i][j], add, store volatile C[i][j]
         */
        C[I][J] += A[I][k] * B[k][J];
    }
}

/* -------------------------------------------------------------------------
 * TRANSFORMED: what the two passes produce in the IR.
 *
 * HoistInvariantRowPointers:
 *   - A[I] row ptr hoisted to preheader  → body uses gep(A_row, k)
 *   - C[I][J] elem ptr hoisted           → body uses C_elem directly
 *
 * FlattenDoublePointers:
 *   - B[k][J] replaced by B_elem PHI
 *     B_elem_init = B[k_start=1][J]
 *     B_elem_next = gep(B_elem, N)     <- advance by one full row (N ints)
 * -------------------------------------------------------------------------*/
void gemm_transformed(int **A, int **B, int **C, int n, int I, int J) {
    /* --- Preheader (HoistInvariantRowPointers) --- */
    int *A_row  = A[I];          /* hoisted: A[I] row ptr, same every k iter */
    int *C_elem = &C[I][J];      /* hoisted: C[I][J] elem ptr, invariant     */

    /* --- Preheader (FlattenDoublePointers) --- */
    int k_start = 1;
    int *B_elem = B[k_start] + J; /* B_elem_init = B[k_start][J]            */

    /* --- Loop body --- */
    for (int k = k_start; k < n - 1; k++) {
        /* A[I][k]: gep(hoisted A_row, k) -- stride-1 along the row */
        int a_val = A_row[k];

        /* B[k][J]: load B_elem directly -- no row-ptr load, no elem GEP */
        int b_val = *B_elem;

        /* C[I][J]: load and store via hoisted C_elem -- no GEP in body */
        *C_elem += a_val * b_val;

        /* --- Latch (FlattenDoublePointers) --- */
        B_elem += n;   /* gep(B_elem, N): advance to B[k+1][J] */
                       /* correct because B is contiguous row-major:
                          B[k+1][J] = B[k][J] + N elements       */
    }
}

int main(void) {
    /* ---- Allocate contiguous row-major storage (polybench convention) ---- */
    int *A_flat = calloc(N * N, sizeof(int));
    int *B_flat = calloc(N * N, sizeof(int));
    int *C_orig = calloc(N * N, sizeof(int));
    int *C_tran = calloc(N * N, sizeof(int));

    int *A_rows[N], *B_rows[N], *C_orig_rows[N], *C_tran_rows[N];
    for (int i = 0; i < N; i++) {
        A_rows[i]      = A_flat + i * N;
        B_rows[i]      = B_flat + i * N;
        C_orig_rows[i] = C_orig + i * N;
        C_tran_rows[i] = C_tran + i * N;
    }

    /* ---- Initialize A, B with known values; C = 0 ---- */
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++)
            A_rows[i][k] = i * N + k + 1;
    for (int k = 0; k < N; k++)
        for (int j = 0; j < N; j++)
            B_rows[k][j] = k + j + 1;

    /* ---- Run both versions for every (i,j) pair ---- */
    int mismatches = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            gemm_original  (A_rows, B_rows, (int **)C_orig_rows, N, i, j);
            gemm_transformed(A_rows, B_rows, (int **)C_tran_rows, N, i, j);
        }
    }

    /* ---- Compare ---- */
    printf("C_original vs C_transformed:\n");
    printf("%-6s %-6s %-12s %-12s %-6s\n", "i", "j", "original", "transformed", "match");
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            int orig = C_orig_rows[i][j];
            int tran = C_tran_rows[i][j];
            int ok   = (orig == tran);
            if (!ok) mismatches++;
            if (i < 4 && j < 4)   /* print top-left 4x4 for readability */
                printf("%-6d %-6d %-12d %-12d %s\n",
                       i, j, orig, tran, ok ? "OK" : "MISMATCH");
        }
    }

    printf("\nTotal mismatches: %d / %d\n", mismatches, N * N);
    if (mismatches == 0)
        printf("PASS: transforms are semantically correct.\n");
    else
        printf("FAIL: transforms produce wrong results!\n");

    free(A_flat); free(B_flat); free(C_orig); free(C_tran);
    return mismatches != 0;
}
