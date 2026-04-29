#define S 20
#define WIN_SIZE (2 * S + 1)
#define MAX_DIFF 255

int ip[WIN_SIZE];
int dpt[WIN_SIZE];
int cp[2 * MAX_DIFF + 1]; // indexed from -255 to +255
int out = 0;

__attribute__((noinline))
void susan4() {
    int area = 0;
    int total = 0;
    int center = 128;

    for (int x = -S; x <= S - 3; x+=4) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif


        int idx0 = x + S; // convert from [-S, S] to [0, 2S]
        int bright0 = ip[idx0];
        int sim0 = dpt[idx0] * cp[MAX_DIFF - (center - bright0)];
        area += sim0;
        total += bright0 * sim0;

        int idx1 = x + S + 1; // convert from [-S, S] to [0, 2S]
        int bright1 = ip[idx1];
        int sim1 = dpt[idx1] * cp[MAX_DIFF - (center - bright1)];
        area += sim1;
        total += bright1 * sim1;

        int idx2 = x + S + 2; // convert from [-S, S] to [0, 2S]
        int bright2 = ip[idx2];
        int sim2 = dpt[idx2] * cp[MAX_DIFF - (center - bright2)];
        area += sim2;
        total += bright2 * sim2;

        int idx3 = x + S + 3; // convert from [-S, S] to [0, 2S]
        int bright3 = ip[idx3];
        int sim3 = dpt[idx3] * cp[MAX_DIFF - (center - bright3)];
        area += sim3;
        total += bright3 * sim3;
    }

    if (area > 0)
        out = total / area;
    else
        out = center;
}

int main() {
    for (int i = 0; i < WIN_SIZE; i++) {
        ip[i] = (i * 7 + 35) % 256;
    }

    for (int i = 0; i < WIN_SIZE; i++) {
        int dist = i - S;
        int mask = dist >> 31;
        dist = (dist ^ mask) - mask;
        dpt[i] = 256 - dist * 12;
        if (dpt[i] < 0) dpt[i] = 0;
    }

    for (int i = -MAX_DIFF; i <= MAX_DIFF; i++) {
        int val = 256 - (i * i) / 4;
        if (val < 0) val = 0;
        cp[i + MAX_DIFF] = val;
    }

    susan4();
    return 0;
}
