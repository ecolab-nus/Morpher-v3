#define SIZE 256

unsigned int PLAINTEXT[SIZE];
unsigned int KEY[SIZE];
unsigned int CIPHERTEXT[SIZE];

__attribute__((noinline))
void tinydes4() {
    for (int i = 0; i < SIZE-3; i+=4) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif

        unsigned int val = PLAINTEXT[i];
        unsigned int rotated = (val << 3) | (val >> (32 - 3));
        CIPHERTEXT[i] = rotated ^ KEY[i];

        val = PLAINTEXT[i + 1];
        rotated = (val << 3) | (val >> (32 - 3));
        CIPHERTEXT[i + 1] = rotated ^ KEY[i + 1];

        val = PLAINTEXT[i + 2];
        rotated = (val << 3) | (val >> (32 - 3));
        CIPHERTEXT[i + 2] = rotated ^ KEY[i + 2];

        val = PLAINTEXT[i + 3];
        rotated = (val << 3) | (val >> (32 - 3));
        CIPHERTEXT[i + 2] = rotated ^ KEY[i + 3];
        // notice how this is i+2 instead of i+3, the compiler does not work with i+3
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        PLAINTEXT[i] = i * 12345;
        KEY[i] = 0x0F0F0F0F ^ i;
    }

    tinydes4();

    return 0;
}
