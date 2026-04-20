/* MachSuite: viterbi — Viterbi HMM decoder.
   Integer version: avoids loop-carried float phi nodes.
   CGRA kernel: inner prev-state loop.
   emit_curr is precomputed before the prev loop. */

#include <stdio.h>
#include <stdint.h>

#define N_STATES  8
#define N_OBS    16
#define N_TOKENS  4

int   obs[N_OBS];
int32_t init_prob[N_STATES];
int32_t transition[N_STATES * N_STATES];
int32_t emission[N_STATES * N_TOKENS];
int   path[N_OBS];
int32_t llike[N_OBS * N_STATES];

__attribute__((noinline))
int viterbi() {
    int t, prev, curr, s, min_s;
    int32_t min_p, p;

    for (s = 0; s < N_STATES; s++)
        llike[s] = init_prob[s] + emission[s*N_TOKENS + obs[0]];

    for (t = 1; t < N_OBS; t++) {
        int obs_t = obs[t];
        for (curr = 0; curr < N_STATES; curr++) {
            int base_prev = (t-1) * N_STATES;
            /* Precompute the loop-invariant emission term as a scalar
               so it never appears as a memory load inside the prev loop. */
            int32_t emit_curr = emission[curr*N_TOKENS + obs_t];

            min_p = llike[base_prev] + transition[curr] + emit_curr;
            for (prev = 1; prev < N_STATES; prev++) {
                #ifdef CGRA_COMPILER
                please_map_me();
                #endif
                p = llike[base_prev + prev] +
                    transition[prev*N_STATES + curr] +
                    emit_curr;
                if (p < min_p)
                    min_p = p;
            }
            llike[t*N_STATES + curr] = min_p;
        }
    }

    min_s = 0;
    min_p = llike[(N_OBS-1)*N_STATES];
    for (s = 1; s < N_STATES; s++) {
        p = llike[(N_OBS-1)*N_STATES + s];
        if (p < min_p) { min_p = p; min_s = s; }
    }
    path[N_OBS-1] = min_s;

    for (t = N_OBS-2; t >= 0; t--) {
        min_s = 0;
        min_p = llike[t*N_STATES] + transition[path[t+1]];
        for (s = 1; s < N_STATES; s++) {
            p = llike[t*N_STATES + s] + transition[s*N_STATES + path[t+1]];
            if (p < min_p) { min_p = p; min_s = s; }
        }
        path[t] = min_s;
    }
    return 0;
}

int main() {
    int i, j;
    for (i = 0; i < N_OBS; i++)
        obs[i] = i % N_TOKENS;
    for (i = 0; i < N_STATES; i++) {
        init_prob[i] = i + 1;
        for (j = 0; j < N_STATES; j++)
            transition[i*N_STATES + j] = i + j + 1;
        for (j = 0; j < N_TOKENS; j++)
            emission[i*N_TOKENS + j] = i*N_TOKENS + j + 1;
    }
    viterbi();
    for (i = 0; i < 4; i++)
        printf("path[%d]=%d\n", i, path[i]);
    return 0;
}
