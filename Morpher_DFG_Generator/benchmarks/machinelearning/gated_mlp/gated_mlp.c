#define T 64
#define DIN 64
#define DH 64
#define DOUT 64

static int X[T][DIN];
static int WG[DIN][DH];
static int WV[DIN][DH];
static int WO[DH][DOUT];
static int O[T][DOUT];

static void init_array(void) {
    int t, d, h, j;
    for (t = 0; t < T; ++t) {
        for (d = 0; d < DIN; ++d) {
            X[t][d] = ((3 * t + d) % 17) - 8;
        }
        for (j = 0; j < DOUT; ++j) {
            O[t][j] = 0;
        }
    }
    for (d = 0; d < DIN; ++d) {
        for (h = 0; h < DH; ++h) {
            WG[d][h] = ((d + 2 * h) % 11) - 5;
            WV[d][h] = ((2 * d - h) % 13) - 6;
        }
    }
    for (h = 0; h < DH; ++h) {
        for (j = 0; j < DOUT; ++j) {
            WO[h][j] = ((h + j) % 7) - 3;
        }
    }
}

void gated_mlp(void) {
    int t, h, d;
    for (t = 0; t < T; ++t) {
        for (h = 0; h < DH; ++h) {
            int gate = 0;
            int value = 0;
            for (d = 0; d < DIN; d++) {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                gate += X[t][d] * WG[d][h];
                value += X[t][d] * WV[d][h];
            }
            /* gate = (gate > 0) ? gate : 0; */
            /* for (j = 0; j < DOUT; ++j) { */
/* #ifdef CGRA_COMPILER */
            /*     please_map_me(); */
/* #endif */
            /*     O[t][j] += gate * value * WO[h][j]; */
            /* } */
            O[t][h] = gate + value;
        }
    }
}

int main(void) {
    int t, j;
    int checksum = 0;
    init_array();
    gated_mlp();
    for (t = 0; t < T; ++t) {
        for (j = 0; j < DOUT; ++j) {
            checksum += O[t][j];
        }
    }
    return checksum;
}
