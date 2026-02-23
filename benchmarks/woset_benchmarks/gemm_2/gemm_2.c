#include <string.h>
#include <stdio.h>


#define SIZE  8
int n = SIZE;
int A[SIZE][SIZE], B[SIZE][SIZE], C[SIZE][SIZE];
int i,j;

__attribute__((noinline))
void gemm_2(){



   int i,j,k;

   for (i=0;i<SIZE; i++)
      for (j=0; j<SIZE; j++)
        for (k=0;k<SIZE; k=k+4){
          #ifdef CGRA_COMPILER
           please_map_me();
           #endif
		       A[i][j] = A[i][j] + B[i][k]* C[k][j] + B[i][k+1]* C[k+1][j]
			       + B[i][k+2]* C[k+2][j] + B[i][k+3]* C[k+3][j];
            }


}

int main(){

int i,j;
for (i=0;i<n; i++)
   for (j=0; j<n; j++) {
      A[i][j]=0;
      B[i][j] = j+i;
      C[i][j] = j*i;
    }

gemm_2();

for (i=0;i<n; i++)
   for (j=0; j<n; j++) {
      printf("%d\n", A[i][j]);
    }

return 0;
}
