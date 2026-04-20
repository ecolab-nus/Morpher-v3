/* MachSuite: sort/merge — merge sort.
   CGRA kernel: inner k loop of the merge step, which selects from two
   sorted halves using a compare-and-pick. */

#include <stdio.h>
#include <stdint.h>

#define SIZE 16

int32_t a[SIZE];
int32_t temp_buf[SIZE];

__attribute__((noinline))
void sort_merge(int start, int m, int stop) {
    int i, j, k;

    for (i = start; i <= m; i++)
        temp_buf[i] = a[i];
    for (j = m+1; j <= stop; j++)
        temp_buf[m+1+stop-j] = a[j];

    i = start;
    j = stop;
    for (k = start; k <= stop; k++) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        int32_t tmp_j = temp_buf[j];
        int32_t tmp_i = temp_buf[i];
        if (tmp_j < tmp_i) {
            a[k] = tmp_j;
            j--;
        } else {
            a[k] = tmp_i;
            i++;
        }
    }
}

__attribute__((noinline))
void ms_mergesort() {
    int m, i, from, mid, to;

    for (m = 1; m < SIZE; m += m) {
        for (i = 0; i < SIZE; i += m+m) {
            from = i;
            mid  = i + m - 1;
            to   = i + m + m - 1;
            if (to < SIZE)
                sort_merge(from, mid, to);
            else
                sort_merge(from, mid, SIZE - 1);
        }
    }
}

int main() {
    int i;
    for (i = 0; i < SIZE; i++)
        a[i] = SIZE - i;           /* reverse-sorted input */
    ms_mergesort();
    for (i = 0; i < SIZE; i++)
        printf("%d ", a[i]);
    printf("\n");
    return 0;
}
