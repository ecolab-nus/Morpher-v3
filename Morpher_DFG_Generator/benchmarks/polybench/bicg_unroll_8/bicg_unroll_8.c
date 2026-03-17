#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *p;
volatile int *r;
volatile int *s;
volatile int *q;


int *c;

int bicg_unroll_8() {
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
        for (j = 0; j + 7 < m; j += 8) {
            #ifdef CGRA_COMPILER
               please_map_me();
            #endif
            s[j] = s[j] + r[i] * A[i][j];
            qi = qi + A[i][j] * p[j];

            s[j + 1] = s[j + 1] + r[i] * A[i][j + 1];
	        qi = qi + A[i][j + 1] * p[j + 1];

            s[j + 2] = s[j + 2] + r[i] * A[i][j + 2];
	        qi = qi + A[i][j + 2] * p[j + 2];

            s[j + 3] = s[j + 3] + r[i] * A[i][j + 3];
	        qi = qi + A[i][j + 3] * p[j + 3];

            s[j + 4] = s[j + 4] + r[i] * A[i][j + 4];
	        qi = qi + A[i][j + 4] * p[j + 4];

            s[j + 5] = s[j + 5] + r[i] * A[i][j + 5];
	        qi = qi + A[i][j + 5] * p[j + 5];

            s[j + 6] = s[j + 6] + r[i] * A[i][j + 6];
	        qi = qi + A[i][j + 6] * p[j + 6];

            s[j + 7] = s[j + 7] + r[i] * A[i][j + 7];
	        qi = qi + A[i][j + 7] * p[j + 7];
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
    return bicg_unroll_8();
}
