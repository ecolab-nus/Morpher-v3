/* MachSuite: nw — Needleman-Wunsch global sequence alignment.
   CGRA kernel: inner a_idx loop fills one row of the DP scoring matrix,
   computing up-left/up/left scores and recording the traceback pointer. */

#include <stdio.h>
#include <stdint.h>

#define ALEN 8
#define BLEN 8

#define MATCH_SCORE    1
#define MISMATCH_SCORE (-1)
#define GAP_SCORE      (-1)

#define ALIGN '\\'
#define SKIPA '^'
#define SKIPB '<'

#define MAX(A,B) (((A)>(B))?(A):(B))

char seqA[ALEN];
char seqB[BLEN];
char alignedA[ALEN + BLEN];
char alignedB[ALEN + BLEN];
int  M[(ALEN+1)*(BLEN+1)];
char ptr[(ALEN+1)*(BLEN+1)];

__attribute__((noinline))
void nw() {
    int score, up_left, up, left, max;
    int row, row_up;
    int a_idx, b_idx;
    int a_str_idx, b_str_idx;

    /* Initialise first row and column */
    for (a_idx = 0; a_idx < ALEN+1; a_idx++)
        M[a_idx] = a_idx * GAP_SCORE;
    for (b_idx = 0; b_idx < BLEN+1; b_idx++)
        M[b_idx*(ALEN+1)] = b_idx * GAP_SCORE;

    /* Fill DP matrix */
    for (b_idx = 1; b_idx < BLEN+1; b_idx++) {
        row_up = (b_idx-1) * (ALEN+1);
        row    = b_idx     * (ALEN+1);
        for (a_idx = 1; a_idx < ALEN+1; a_idx++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            score   = (seqA[a_idx-1] == seqB[b_idx-1]) ? MATCH_SCORE : MISMATCH_SCORE;
            up_left = M[row_up + (a_idx-1)] + score;
            up      = M[row_up +  a_idx   ] + GAP_SCORE;
            left    = M[row    + (a_idx-1)] + GAP_SCORE;
            max     = MAX(up_left, MAX(up, left));
            M[row + a_idx] = max;
            if (max == left)
                ptr[row + a_idx] = SKIPB;
            else if (max == up)
                ptr[row + a_idx] = SKIPA;
            else
                ptr[row + a_idx] = ALIGN;
        }
    }

    /* Traceback */
    a_idx = ALEN; b_idx = BLEN;
    a_str_idx = 0; b_str_idx = 0;
    while (a_idx > 0 || b_idx > 0) {
        int r = b_idx * (ALEN+1);
        if (ptr[r + a_idx] == ALIGN) {
            alignedA[a_str_idx++] = seqA[a_idx-1];
            alignedB[b_str_idx++] = seqB[b_idx-1];
            a_idx--; b_idx--;
        } else if (ptr[r + a_idx] == SKIPB) {
            alignedA[a_str_idx++] = seqA[a_idx-1];
            alignedB[b_str_idx++] = '-';
            a_idx--;
        } else {
            alignedA[a_str_idx++] = '-';
            alignedB[b_str_idx++] = seqB[b_idx-1];
            b_idx--;
        }
    }
    for (; a_str_idx < ALEN+BLEN; a_str_idx++) alignedA[a_str_idx] = '_';
    for (; b_str_idx < ALEN+BLEN; b_str_idx++) alignedB[b_str_idx] = '_';
}

int main() {
    int i;
    const char *s1 = "ATCGATCG";
    const char *s2 = "TACGTACG";
    for (i = 0; i < ALEN; i++) seqA[i] = s1[i];
    for (i = 0; i < BLEN; i++) seqB[i] = s2[i];
    nw();
    printf("A: %.*s\n", ALEN+BLEN, alignedA);
    printf("B: %.*s\n", ALEN+BLEN, alignedB);
    return 0;
}
