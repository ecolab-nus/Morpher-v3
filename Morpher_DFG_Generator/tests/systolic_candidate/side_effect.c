#include <stdio.h>
#define N 16

void side_effect(float * __restrict__ C,
                 const float * __restrict__ A,
                 const float * __restrict__ B) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            for (int k = 0; k < N; k++) {
                C[i*N + j] += A[i*N + k] * B[k*N + j];
                printf("step\n");
            }
}
