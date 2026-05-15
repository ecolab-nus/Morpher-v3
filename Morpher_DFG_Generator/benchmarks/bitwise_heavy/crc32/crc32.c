#include <stdio.h>

#define SIZE 64
unsigned char msg[SIZE];
unsigned int out[SIZE];

// snippet from: https://stackoverflow.com/questions/21001659/crc32-algorithm-implementation-in-c-without-a-look-up-table-and-with-a-public-li
__attribute__((noinline))
void crc32() {
    int i = 0;
    unsigned int byte, crc, mask;
    crc = 0xFFFFFFFF;

    while (i < SIZE) {
        #ifdef CGRA_COMPILER
            please_map_me();
        #endif
        byte = msg[i];
        crc = crc ^ byte;

        // unrolled crc loop 
        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 1

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 2

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 3

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 4

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 5

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 6

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 7

        mask = -(crc & 1);
        crc = (crc >> 1) ^ (0xEDB88320 & mask); // 8

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

    crc32();

    return 0;
}
