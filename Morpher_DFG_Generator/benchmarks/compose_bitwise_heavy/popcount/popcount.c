#define SIZE 20

unsigned char DATA[SIZE];
unsigned char OUT[SIZE];

__attribute__((noinline))
void popcount() {
    for (int i = 0; i < SIZE; i++) {
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
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        DATA[i] = i * 3; // Example non-uniform values
    }

    popcount();

    return 0;
}
