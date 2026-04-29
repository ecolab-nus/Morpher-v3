#include <stdio.h>

#define ARRAY_LEN 300
#define N_VAL 258

int a[ARRAY_LEN];
int b[ARRAY_LEN];

int conv2_large() {
    int i;
    int n = N_VAL;

    for (i = 1; i < n - 1; i++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        b[i] = a[i] * 10 + a[i + 1] * 20;
    }

    return 0;
}

int main() {
    int i;
    for (i = 0; i < ARRAY_LEN; i++) {
        a[i] = i + 1;
        b[i] = 0;
    }
    conv2_large();
    printf("b[1]=%d b[2]=%d b[128]=%d\n", b[1], b[2], b[128]);
    return 0;
}
