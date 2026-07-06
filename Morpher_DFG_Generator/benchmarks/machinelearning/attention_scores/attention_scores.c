#define T 64
#define D 64

static int Q[T][D];
static int K[T][D];
static int Scores[T][T];

static void init_array(void) {
    int t, d;
    for (t = 0; t < T; ++t) {
        for (d = 0; d < D; ++d) {
            Q[t][d] = ((t + d) % 7) - 3;
            K[t][d] = ((2 * t - d) % 11) - 5;
        }
    }
}

void attention_scores(void) {
    int tq, tk, d;
    for (tq = 0; tq < T; ++tq) {
        for (tk = 0; tk < T; ++tk) {
            int score = 0;
            for (d = 0; d < D; ++d) {
#ifdef CGRA_COMPILER
                please_map_me();
#endif
                score += Q[tq][d] * K[tk][d];
            }
            Scores[tq][tk] = score;
        }
    }
}

int main(void) {
    init_array();
    attention_scores();
    return Scores[0][0];
}
