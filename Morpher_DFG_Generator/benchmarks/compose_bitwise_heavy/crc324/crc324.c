#include <stdio.h>

#define SIZE 256
unsigned char msg[SIZE];
unsigned int out[SIZE];

// snippet from: https://stackoverflow.com/questions/21001659/crc32-algorithm-implementation-in-c-without-a-look-up-table-and-with-a-public-li
__attribute__((noinline))
void crc324() {
    int i = 0;
    unsigned int byte, crc, mask;
    crc = 0xFFFFFFFF;

    // while (i < SIZE) {
    //     #ifdef CGRA_COMPILER
    //         please_map_me();
    //     #endif
    //     byte = msg[i];
    //     crc = crc ^ byte;

    //     // unrolled crc loop 
    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 1

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 2

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 3

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 4

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 5

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 6

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 7

    //     mask = -(crc & 1);
    //     crc = (crc >> 1) ^ (0xEDB88320 & mask); // 8

    //     out[i] = ~crc;

    //     i++;
    // }

    while (i + 3 < SIZE) {
    #ifdef CGRA_COMPILER
        please_map_me();
    #endif

    // ---- iteration 0 ----
    byte = msg[i];
    crc ^= byte;

    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);

    out[i] = ~crc;

    // ---- iteration 1 ----
    byte = msg[i + 1];
    crc ^= byte;

    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);

    out[i + 1] = ~crc;

    // ---- iteration 2 ----
    byte = msg[i + 2];
    crc ^= byte;

    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);

    out[i + 2] = ~crc;

    // ---- iteration 3 ----
    byte = msg[i + 3];
    crc ^= byte;

    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);
    mask = -(crc & 1);
    crc = (crc >> 1) ^ (0xEDB88320 & mask);

    out[i + 3] = ~crc;

    i += 4;
}

// handle remainder
while (i < SIZE) {
    byte = msg[i];
    crc ^= byte;

    for (int k = 0; k < 8; k++) {
        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }

    out[i] = ~crc;
    i++;
}

    // crc = ~crc;
    // printf("%d\n", crc);
}

int main() {
    int i;

    // for (i = 0; i < SIZE; i++) { // dummy values
    //     msg[i] = (char)i + 2;
    // }

    crc324();

    return 0;
}
