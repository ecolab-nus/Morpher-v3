#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *p;
volatile int *r;
volatile int *s;
volatile int *q;


int *c;

int bicg_unroll() {
    int i;
    int j;
    int m = *N;
    int n = *N;

    int sum = 0;
    for (i = 0; i < m; i++) {
        s[i] = 0;
    }

    for (i = 0; i < n; i++) {
        int qi = 0;
        for (j = 0; j + 1 < m; j += 2) {
            #ifdef CGRA_COMPILER
               please_map_me();
            #endif
            s[j] = s[j] + r[i] * A[i][j];
            qi = qi + A[i][j] * p[j];

            s[j + 1] = s[j + 1] + r[i] * A[i][j + 1];
            qi = qi + A[i][j + 1] * p[j + 1];
        }
        for (; j < m; j++) {
            s[j] = s[j] + r[i] * A[i][j];
            qi = qi + A[i][j] * p[j];
        }
        q[i] = qi;
    }


// #pragma scop
//   for (i = 0; i < _PB_NY; i++)
//     s[i] = 0;
//   for (i = 0; i < _PB_NX; i++)
//     {
//       q[i] = 0;
//       for (j = 0; j < _PB_NY; j++)
// 	{
// 	  s[j] = s[j] + r[i] * A[i][j];
// 	  q[i] = q[i] + A[i][j] * p[j];
// 	}
//     }
// #pragma endscop


    return sum;
}

int main() {
    return bicg_unroll();
}
