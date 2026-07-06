#define T 64
#define DIN 64
#define DH 64
#define E 4

static int X[T][DIN];
static int Router[DIN][E];
static int Expert[E * DIN * DH];
static int O[T][DH];

static void init_array(void) {
    int t, d, e, h;
    for (t = 0; t < T; ++t) {
        for (d = 0; d < DIN; ++d) {
            X[t][d] = ((t + 3 * d) % 19) - 9;
        }
    }
    for (d = 0; d < DIN; ++d) {
        for (e = 0; e < E; ++e) {
            Router[d][e] = ((d - e) % 7) - 3;
        }
    }
    for (e = 0; e < E; ++e) {
        for (d = 0; d < DIN; ++d) {
            for (h = 0; h < DH; ++h) {
                Expert[(e * DIN + d) * DH + h] = ((e + d + h) % 11) - 5;
            }
        }
    }
}

static int route_token(int t) {
    int e, d;
    int best = 0;
    int best_score = -1000000;
    for (e = 0; e < E; ++e) {
        int score = 0;
        for (d = 0; d < DIN; ++d) {
            score += X[t][d] * Router[d][e];
        }
        if (score > best_score) {
            best_score = score;
            best = e;
        }
    }
    return best;
}

void moe_top1(void) {
    int t, h, d;
    for (t = 0; t < T; ++t) {
        int e = route_token(t);
        for (h = 0; h < DH; ++h) {
            int out = 0;
            for (d = 0; d < DIN; ++d) {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                out += X[t][d] * Expert[(e * DIN + d) * DH + h];
            }
            O[t][h] = out;
        }
    }
}

int main(void) {
    init_array();
    moe_top1();
    return O[0][0];
}
