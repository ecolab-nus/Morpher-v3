#include <stdio.h>

volatile int * N;
volatile int **A;
volatile int *p;
volatile int *r;
volatile int *s;
volatile int *q;


int *c;

int bicg() {
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
        for (j = 0; j < m; j++) {
                   #ifdef CGRA_COMPILER
               please_map_me();
               #endif
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
    int n_val = 64;
    volatile int N_storage = n_val;
    static int A_flat[64 * 64];
    static volatile int *A_rows[64];
    static volatile int p_data[64];
    static volatile int r_data[64];
    static volatile int s_data[64];
    static volatile int q_data[64];
    N = &N_storage;

    for (int ii = 0; ii < n_val; ii++) {
        A_rows[ii] = (volatile int *)(A_flat + ii * n_val);
        p_data[ii] = ii + 1;
        r_data[ii] = ii + 2;
        for (int jj = 0; jj < n_val; jj++) {
            A_rows[ii][jj] = ii + jj + 1;
        }
    }
    A = A_rows;
    p = p_data;
    r = r_data;
    s = s_data;
    q = q_data;

    int result = bicg();
    return result;
}
