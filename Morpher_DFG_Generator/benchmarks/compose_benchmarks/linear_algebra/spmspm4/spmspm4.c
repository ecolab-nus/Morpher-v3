#include <stdio.h>

#define rows 4
#define cols 4
#define valid_points_a 10
#define valid_points_b 6

// Matrix A
int data_a[valid_points_a]      = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
int colindex_a[valid_points_a]  = {0, 0, 1, 2, 3, 1, 2, 0, 2, 3};
int rowptr_a[rows+1]            = {0, 1, 5, 7, 10};

// Matrix B
int data_b[valid_points_b]      = {1, 2, 3, 4, 5, 6};
int colindex_b[valid_points_b]  = {0, 0, 1, 2, 1, 3};
int rowptr_b[rows+1]            = {0, 1, 4, 5, 6};

int out[rows][cols] = {0};

int spmspm4()
{
  for( int i=0; i<rows; i++) {
    for( int k=rowptr_a[i]; k<rowptr_a[i+1]; k++) {
      
      int m = colindex_a[k];
      // printf("k %d m %d\n", k, m);

      // for( int n=rowptr_b[m]; n<rowptr_b[m+1]; n++) {
      //   // printf("k %d m %d n %d\n", k, m, n);
      //   #ifdef CGRA_COMPILER
      //   please_map_me();
      //   #endif
      //   // printf("dataa %d datab %d\n", data_a[k], data_b[n]);
      //   out[i][colindex_b[n]] += data_a[k] * data_b[n];
      // }

      int n = rowptr_b[m];
int end = rowptr_b[m + 1];

for (; n + 3 < end; n += 4) {
    #ifdef CGRA_COMPILER
        please_map_me();
    #endif

    int idx0 = colindex_b[n];
    int idx1 = colindex_b[n + 1];
    int idx2 = colindex_b[n + 2];
    int idx3 = colindex_b[n + 3];

    out[i][idx0] += data_a[k] * data_b[n];
    out[i][idx1] += data_a[k] * data_b[n + 1];
    out[i][idx2] += data_a[k] * data_b[n + 2];
    out[i][idx3] += data_a[k] * data_b[n + 3];
}

// remainder loop
for (; n < end; n++) {
    out[i][colindex_b[n]] += data_a[k] * data_b[n];
}

    }
  }

  printf("Output\n");

  for( int i=0; i<rows; i++) {
    for( int j=0; j<cols; j++)
      printf("%d ", out[i][j]);
    printf("\n");
  }

    return 0;
}
