#include <stdio.h>

void sddmm4(int M, int N, int A[], int B[], int C[], int mask_row_indices[], int mask_col_indices[], int nnz) {
   
    // Iterate through the non-zero elements of the mask
    for (int k = 0; k < nnz; k++) {
        int i = mask_row_indices[k]; // Row index of the mask
        int j = mask_col_indices[k]; // Column index of the mask
        
        // Iterate through the columns of the dense matrices for these positions
        // for (int l = 0; l < N; l++) {
        //     #ifdef CGRA_COMPILER
        // please_map_me();
        // #endif
        //     C[i * M + j] += A[i * N + l] * B[l * M + j];
        // }

        // Iterate through the columns of the dense matrices for these positions
int l = 0;
for (; l + 3 < N; l += 4) {
    #ifdef CGRA_COMPILER
        please_map_me();
    #endif

    C[i * M + j] += A[i * N + l]     * B[l * M + j];
    C[i * M + j] += A[i * N + l + 1] * B[(l + 1) * M + j];
    C[i * M + j] += A[i * N + l + 2] * B[(l + 2) * M + j];
    C[i * M + j] += A[i * N + l + 3] * B[(l + 3) * M + j];
}

// remainder loop
for (; l < N; l++) {
    C[i * M + j] += A[i * N + l] * B[l * M + j];
}
    }
}

//test
int main() {

    // Define the dimensions of the matrices
    int M = 4;
    int N = 3;

    // Define the input dense matrices A and B
    
    int A[] = {
                1, 2, 3, 
                4, 5, 6, 
                7, 8, 9, 
                10, 11, 12
              };
    
    int B[] = {
                13, 14, 15, 16, 
                17, 18, 19, 20, 
                21, 22, 23, 24
              };

    int C[16] = {0};

    // Define the mask in COO format
    int mask_row_indices[] = {0, 0, 2, 2, 2, 3};
    int mask_col_indices[] = {0, 1, 0, 1, 2, 3};
    int nnz = sizeof(mask_col_indices)/sizeof(int);
    
    sddmm4(M, N, A, B, C, mask_row_indices, mask_col_indices, nnz);

    printf("Result matrix C:\n");
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < M; j++) {
            printf("%d ", C[i * M + j]);
        }
        printf("\n");
    }

    return 0;
}

