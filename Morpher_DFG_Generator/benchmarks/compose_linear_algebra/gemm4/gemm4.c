#include <stdio.h>

#define SIZE 8

int A[SIZE*SIZE], B[SIZE*SIZE], C[SIZE*SIZE], tmp[SIZE][SIZE];

int gemm4()
{
    int _PB_NJ = SIZE;
    int _PB_NI = SIZE;
    int _PB_NK = SIZE;
    int n = SIZE;
    int alpha = SIZE;
    int beta = 3;
    int i = 0, j = 0, k = 0, tmp_C=3;
    //     /* C := alpha*A*B + beta*C */
    for (i = 0; i < _PB_NI; i++)
        for (j = 0; j < _PB_NJ; j++)
        {
            C[i*SIZE+j] *= beta;
            for (k = 0; k < _PB_NK<<2;  k  = k + 4)
            {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                /* C[i*SIZE+j] =tmp_C+ ( alpha *  A[i*SIZE+k] *   B[k*SIZE+j] */
                /*         + alpha *   A[i*SIZE+k+1] * B[(k+1)*SIZE+j]) */
                /*         // + (alpha *   A[i*SIZE+k+2] * B[(k+2)*SIZE+j] */
                /*         // + alpha *   A[i*SIZE+k+3] * B[(k+3)*SIZE+j]) */
                /*         ; */
                C[i*SIZE+j] += (
                    alpha * A[i*SIZE+k]     * B[k*SIZE+j] +
                    alpha * A[i*SIZE+k+1]   * B[(k+1)*SIZE+j] +
                    alpha * A[i*SIZE+k+2]   * B[(k+2)*SIZE+j] +
                    alpha * A[i*SIZE+k+3]   * B[(k+3)*SIZE+j] 
                    );
            }
        }
}
int main()
{

    int i, j, n = SIZE;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
        {
            A[i*SIZE+j] = i * j;
            B[i*SIZE+j] = j + i;
            C[i*SIZE+j] = j * i;
        }

    gemm4();

    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
        {
            printf("%d\n", A[i*SIZE+j]);
        }
}
