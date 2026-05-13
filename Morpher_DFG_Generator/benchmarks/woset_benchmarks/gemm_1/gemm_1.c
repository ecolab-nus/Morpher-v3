#include <string.h>
#include <stdio.h>


#define SIZE  64
int A[SIZE][SIZE], B[SIZE][SIZE], C[SIZE][SIZE];

__attribute__((noinline))
void gemm_1(){
   int i,j,k;

   for (i=0;i<SIZE; i++)
      for (j=0;j<SIZE; j++)
        for (k=0;k<SIZE; k++){
           #ifdef CGRA_COMPILER
           please_map_me();
           #endif
           A[i][j] = A[i][j] + B[i][k]* C[k][j];
        }

}

int main(){

int i,j;
for (i=0;i<SIZE; i++)
   for (j=0;j<SIZE; j++) {
      A[i][j]=0;
      B[i][j] = j+i;
      C[i][j] = j*i;
    }

gemm_1();

for (i=0;i<SIZE; i++)
   for (j=0;j<SIZE; j++) {
      printf("%d\n", A[i][j]);
    }

return 0;
}

