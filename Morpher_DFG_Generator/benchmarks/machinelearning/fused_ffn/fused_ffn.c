#define T 64
#define DIN 64
#define DH 64
#define DOUT 64

static int X[T][DIN];
static int W1[DIN][DH];
static int W2[DH][DOUT];
static int O[T][DOUT];

static void init_array(void) {
    int t, d, h, j;
    for (t = 0; t < T; ++t) {
        for (d = 0; d < DIN; ++d) {
            X[t][d] = ((t - d) % 13) - 6;
        }
        for (j = 0; j < DOUT; ++j) {
            O[t][j] = 0;
        }
    }
    for (d = 0; d < DIN; ++d) {
        for (h = 0; h < DH; ++h) {
            W1[d][h] = ((d + h) % 9) - 4;
        }
    }
    for (h = 0; h < DH; ++h) {
        for (j = 0; j < DOUT; ++j) {
            W2[h][j] = ((2 * h + j) % 7) - 3;
        }
    }
}

void fused_ffn(void) {
    int t, h, d, j;
    for (t = 0; t < T; ++t) {
        for (h = 0; h < DH; ++h) {
            int hidden = 0;
            for (d = 0; d < DIN; ++d) {
                hidden += X[t][d] * W1[d][h];
            }
            hidden = (hidden > 0) ? hidden : 0;
            for (j = 0; j < DOUT; ++j) {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                O[t][j] += hidden * W2[h][j];
            }
        }
    }
}

int main(void) {
    init_array();
    fused_ffn();
    return O[0][0];
}
