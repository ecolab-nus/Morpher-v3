#define T 64
#define DIN 64
#define DOUT 64

static int X[T][DIN];
static int WQ[DIN][DOUT];
static int WK[DIN][DOUT];
static int WV[DIN][DOUT];
static int Q[T][DOUT];
static int K[T][DOUT];
static int V[T][DOUT];

static void init_array(void) {
    int t, d, j;
    for (t = 0; t < T; ++t) {
        for (d = 0; d < DIN; ++d) {
            X[t][d] = ((t + 2 * d) % 13) - 6;
        }
    }
    for (d = 0; d < DIN; ++d) {
        for (j = 0; j < DOUT; ++j) {
            WQ[d][j] = ((d + j) % 7) - 3;
            WK[d][j] = ((2 * d - j) % 9) - 4;
            WV[d][j] = ((d + 3 * j) % 11) - 5;
        }
    }
}

void qkv_projection(void) {
    int t, j, d;
    for (t = 0; t < T; ++t) {
        for (j = 0; j < DOUT; ++j) {
            int q = 0;
            int k = 0;
            int v = 0;
            for (d = 0; d < DIN; ++d) {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                q += X[t][d] * WQ[d][j];
                k += X[t][d] * WK[d][j];
                v += X[t][d] * WV[d][j];
            }
            Q[t][j] = q;
            K[t][j] = k;
            V[t][j] = v;
        }
    }
}

int main(void) {
    init_array();
    qkv_projection();
    return Q[0][0] + K[0][0] + V[0][0];
}
