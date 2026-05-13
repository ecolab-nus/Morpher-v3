#include <stdio.h>

#define N_VAL 64
volatile int a[N_VAL] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};

volatile int * n;
// Simple accumulation
int sum() {

    int N = N_VAL;//*n;
    int sum = 0;
    int i;
    for (i = 0; i < N; i++) {
#ifdef CGRA_COMPILER
please_map_me();
#endif 
        sum += a[i];
    }
    printf("sum = %d\n", sum);

    return sum;
}

int main() {
    sum();
    return 0;
}
