#define SIZE 20

unsigned int PLAINTEXT[SIZE];
unsigned int KEY[SIZE];
unsigned int CIPHERTEXT[SIZE];

__attribute__((noinline))
void tinydes() {
    for (int i = 0; i < SIZE; i++) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif

        unsigned int val = PLAINTEXT[i];
        unsigned int rotated = (val << 3) | (val >> (32 - 3));
        CIPHERTEXT[i] = rotated ^ KEY[i];
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        PLAINTEXT[i] = i * 12345;
        KEY[i] = 0x0F0F0F0F ^ i;
    }

    tinydes();

    return 0;
}
