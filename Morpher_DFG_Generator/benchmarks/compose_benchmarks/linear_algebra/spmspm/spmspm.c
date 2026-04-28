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

int spmspm()
{
  for( int i=0; i<rows; i++) {
    for( int k=rowptr_a[i]; k<rowptr_a[i+1]; k++) {
      
      int m = colindex_a[k];
      // printf("k %d m %d\n", k, m);

      for( int n=rowptr_b[m]; n<rowptr_b[m+1]; n++) {
        // printf("k %d m %d n %d\n", k, m, n);
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        // printf("dataa %d datab %d\n", data_a[k], data_b[n]);
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
