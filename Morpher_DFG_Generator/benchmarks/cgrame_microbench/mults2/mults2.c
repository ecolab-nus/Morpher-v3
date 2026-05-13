#include <stdio.h>

volatile int * N;
//int *a;
//int *b;
int *m;

#define N_VAL 66
#define ARRAY_LEN (N_VAL + 3)
int a[ARRAY_LEN] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
int b[ARRAY_LEN] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};

int mults2() {
    int i, j;
    int n = N_VAL;// *N;

    int sum = 0;

    for (i = 1; i < n-1; i++) {
        //DFGLoop: loop
#ifdef CGRA_COMPILER
please_map_me();
#endif 
        sum += (a[i] * 2 + a[i + 1] * 2) * (b[i] * 2 + b[i + 3] * 2) * (b[i + 3] * 3) * b[i];
    }

    return sum;
}

int main() {
    mults2();
    return 0;
}
