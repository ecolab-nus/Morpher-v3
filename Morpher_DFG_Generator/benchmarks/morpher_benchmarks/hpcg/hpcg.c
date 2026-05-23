#include <stdio.h>
#define SIZE 128

int n = SIZE;

int A[SIZE*SIZE], x[SIZE];

__attribute__((noinline))
void hpcg(){
    int i, j, sum;


    for(i=0; i<SIZE; i++) {
        sum = 0;
        for(j=0; j<SIZE; j++) {
            
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            sum += A[i*SIZE+j] * x[j];
        }
        x[i] = sum;
    }
}

int main() {
    int k, l, checksum;

    for (k=0; k<n; k++) {
        for(l=0; l<n; l++)
            A[k*SIZE+l] = k*2 + l*3 + 5;
        x[k] = k*3;
    }

    hpcg();

    checksum = 0;
    for(k=0; k<n; k++)
        checksum += x[k];

    return checksum == -1;
}
