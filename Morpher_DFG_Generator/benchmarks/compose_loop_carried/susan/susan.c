#define S 20
#define WIN_SIZE (2 * S + 1)
#define MAX_DIFF 255

int ip[WIN_SIZE];
int dpt[WIN_SIZE];
int cp[2 * MAX_DIFF + 1]; // indexed from -255 to +255
int out = 0;

__attribute__((noinline))
void susan() {
    int area = 0;
    int total = 0;
    int center = 128;

    for (int x = -S; x <= S; x++) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        int idx = x + S; // convert from [-S, S] to [0, 2S]
        int bright = ip[idx];
        int sim = dpt[idx] * cp[MAX_DIFF - (center - bright)];
        area += sim;
        total += bright * sim;
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

    susan();
    return 0;
}
