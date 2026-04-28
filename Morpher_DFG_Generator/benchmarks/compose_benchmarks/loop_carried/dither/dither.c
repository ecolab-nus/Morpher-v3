#include <stdio.h>
#include <string.h>

#define SIZE 20
#define SRC_VAL(i) ((i * 13) % 256) // Example values

int src[SIZE], dest[SIZE];

__attribute__((noinline))
void dither() {
    int err = 0;
    
    for (int i = 0; i < SIZE; i++) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        
        int out = src[i] + err;
        int pixel;
        
        if (out > 127) {
            pixel = 0xFF;
            err = out - pixel;
        } else {
            pixel = 0;
            err = out;
        }
        
        dest[i] = pixel;
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        src[i] = SRC_VAL(i);
        dest[i] = 0;
    }
    
    dither();
    
    // for (int i = 0; i < SIZE; i++) {
    //     printf("%d\n", dest[i]);
    // }
    
    return 0;
}
