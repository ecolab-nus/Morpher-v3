#define SIZE 20

unsigned char DATA[SIZE];
unsigned char OUT[SIZE];

__attribute__((noinline))
void popcount4() {
    for (int i = 0; i < SIZE-3; i+=4) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif

        unsigned char x = DATA[i];
        unsigned char count = 0;

        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1;

        OUT[i] = count;

        x = DATA[i+1];
        count = 0;

        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1;

        OUT[i+1] = count;

        x = DATA[i+2];
        count = 0;

        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1;

        OUT[i+2] = count;

        x = DATA[i+3];
        count = 0;

        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1; x >>= 1;
        count += x & 1;

        OUT[i+3] = count;
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        DATA[i] = i * 3; // Example non-uniform values
    }

    popcount4();

    return 0;
}
