#define LA_T 64
#define LA_D 64
#define LA_DV 64

static int Q_storage[LA_T * LA_D];
static int K_storage[LA_T * LA_D];
static int V_storage[LA_T * LA_DV];
static int O_storage[LA_T * LA_DV];
static int S_storage[LA_D][LA_DV];
static int Z_storage[LA_D];
static int y_storage[LA_DV];
static int eps_storage = 1;

static void init_array(void) {
    int t, d, j;

    for (t = 0; t < LA_T; ++t) {
        for (d = 0; d < LA_D; ++d) {
            Q_storage[t * LA_D + d] = ((t + d) % 7) - 3;
            K_storage[t * LA_D + d] = ((2 * t + d) % 9) - 4;
        }
        for (j = 0; j < LA_DV; ++j) {
            V_storage[t * LA_DV + j] = ((t - j) % 11);
            O_storage[t * LA_DV + j] = 0;
        }
    }
}

/*
    Causal linear attention with one fused per-token kernel.

    Shapes:
      Q: [T][D]
      K: [T][D]
      V: [T][DV]
      O: [T][DV]

    Memory layout is row-major contiguous:
      Q[t*D + d], K[t*D + d], V[t*DV + j], O[t*DV + j]

    Recurrence:
      qd = relu(Q[t,d])
      kd = relu(K[t,d])

      Z[d] += kd
      S[d,j] += kd * V[t,j]

      denom += qd * Z[d]
      y[j]   += qd * S[d,j]

      O[t,j] = y[j] / (denom + eps)
*/
void linear_attention(
    const int *Q,
    const int *K,
    const int *V,
    int *O,
    int T,
    int D,
    int DV,
    int eps
) {
    int t, d, j;

    int (*S)[LA_DV] = S_storage;
    int *Z = Z_storage;
    int *y = y_storage;

    if (T > LA_T || D > LA_D || DV > LA_DV || T < 0 || D < 0 || DV < 0) {
        return;
    }

    for (d = 0; d < LA_D; ++d) {
        Z[d] = 0;
        for (j = 0; j < LA_DV; ++j) {
            S[d][j] = 0;
        }
    }

    for (t = 0; t < T; ++t) {
        const int *qt = Q + t * D;
        const int *kt = K + t * D;
        const int *vt = V + t * DV;
        int *ot = O + t * DV;

        for (j = 0; j < DV; ++j) {
            y[j] = 0;
        }

        int denom = eps;

        for (d = 0; d < D; ++d) {
            int qd = (qt[d] > 0) ? qt[d] : 0;
            int kd = (kt[d] > 0) ? kt[d] : 0;

            Z[d] += kd;
            denom += qd * Z[d];

            for (j = 0; j < DV; ++j) {
#ifdef CGRA_COMPILER
      please_map_me();
#endif
                S[d][j] += kd * vt[j];
                y[j]  += qd * S[d][j];
            }
        }

        for (j = 0; j < DV; ++j) {
            ot[j] = (denom != 0) ? (y[j] / denom) : 0;
        }
    }

}

int main(void) {
    init_array();
    linear_attention(Q_storage, K_storage, V_storage, O_storage,
                     LA_T, LA_D, LA_DV, eps_storage);
    return O_storage[0];
}
