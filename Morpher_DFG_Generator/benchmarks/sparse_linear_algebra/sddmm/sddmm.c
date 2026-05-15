#define M 8
#define N 64
#define NNZ 8

int A[M * N];
int B[N * M];
int C[M * M] = {0};
int mask_row_indices[NNZ] = {0, 1, 2, 3, 4, 5, 6, 7};
int mask_col_indices[NNZ] = {0, 2, 4, 6, 1, 3, 5, 7};
int sddmm_result = 0;

__attribute__((noinline))
void sddmm()
{
  for (int k = 0; k < NNZ; k++) {
    int i = mask_row_indices[k];
    int j = mask_col_indices[k];

    for (int l = 0; l < N; l++) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
      C[i * M + j] += A[i * N + l] * B[l * M + j];
    }
  }
}

int main()
{
  for (int i = 0; i < M * N; i++) {
    A[i] = (i % 17) + 1;
  }
  for (int i = 0; i < N * M; i++) {
    B[i] = (i % 13) + 3;
  }

  sddmm();
  sddmm_result = C[0] + C[10] + C[20] + C[30] + C[33] + C[43] + C[53] + C[63];
  return sddmm_result == -1;
}
