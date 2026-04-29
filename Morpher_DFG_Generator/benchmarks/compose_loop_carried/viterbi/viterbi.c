#include <stdint.h>
#include <limits.h>

#define N_STATES   4
#define N_OBS      8

// Flattened input observation sequence
unsigned char obs[N_OBS] = {0, 1, 0, 2, 1, 0, 1, 2};

// Flattened transition probabilities: index = from_state * N_STATES + to_state
int trans_prob[N_STATES * N_STATES] = {
    0,  -1, -2, -2,
   -1,   0, -2, -2,
   -2,  -1,  0, -1,
   -2,  -2, -1,  0
};

// Flattened emission probabilities: index = state * N_EMIT + symbol
#define N_EMIT 3
int emit_prob[N_STATES * N_EMIT] = {
    0,  -1, -2,
   -1,   0, -2,
   -2,  -1,  0,
   -1,  -2,  0
};

// Flattened DP arrays
int dp[N_OBS * N_STATES];
unsigned char backptr[N_OBS * N_STATES];

// Macros
#define DP(t, s)         dp[(t) * N_STATES + (s)]
#define BP(t, s)         backptr[(t) * N_STATES + (s)]
#define TRANS(sp, s)     trans_prob[(sp) * N_STATES + (s)]
#define EMIT(s, sym)     emit_prob[(s) * N_EMIT + (sym)]
#define MAX_UPDATE(val, idx, best_val, best_idx) \
    if ((val) > (best_val)) { best_val = (val); best_idx = (idx); }

__attribute__((noinline))
void viterbi() {

    // DP iterations
    for (int t = 1; t < N_OBS; t++) {
        for (int s = 0; s < N_STATES; s++) {
            int best_val = INT16_MIN;
            unsigned char best_state = 0;

            for (int sp = 0; sp < N_STATES; sp++) {
                #ifdef CGRA_COMPILER
                    please_map_me();
                #endif
                int val = DP(t-1, sp) + TRANS(sp, s);
                MAX_UPDATE(val, sp, best_val, best_state);
            }

            DP(t, s) = best_val + EMIT(s, obs[t]);
            BP(t, s) = best_state;
        }
    }
}

int main() {
    // Initialization
    for (int s = 0; s < N_STATES; s++) {
        DP(0, s) = EMIT(s, obs[0]);
        BP(0, s) = 0;
    }

    viterbi();
    return 0;
}
