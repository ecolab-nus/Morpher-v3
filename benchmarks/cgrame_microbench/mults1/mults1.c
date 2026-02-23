#include <stdio.h>

volatile int * N;
//int *a;
int a[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
int *b;
int *m;

int mults1() {
    int i, j;
    int n = 20;//*N;

    int sum = 0;

    for (i = 0; i < n-3; i++) {
        //DFGLoop: loop
#ifdef CGRA_COMPILER
please_map_me();
#endif 
        sum += a[i] * 10 + a[i + 1] * 20 + a[i+ 2] * 39 + a[i + 3] * 15;
    }

    return sum;
}

int main() {
    mults1();
    return 0;
}
