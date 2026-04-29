#include <stdio.h>
#include <string.h>

#define SIZE 20
#define SRC_VAL(i) ((i * 13) % 256) // Example values

int src[SIZE], dest[SIZE];

__attribute__((noinline))
void dither4() {
    int err = 0;
    
    for (int i = 0; i < SIZE; i += 4) {
        #ifdef CGRA_COMPILER
        please_map_me();
        #endif
        
        int out0 = src[i] + err;
        int pixel0 = (out0 > 127) ? 0xFF : 0;
        err = out0 - pixel0;
        dest[i] = pixel0;
        
        if (i + 1 < SIZE) {
            int out1 = src[i + 1] + err;
            int pixel1 = (out1 > 127) ? 0xFF : 0;
            err = out1 - pixel1;
            dest[i + 1] = pixel1;
        }
        
        if (i + 2 < SIZE) {
            int out2 = src[i + 2] + err;
            int pixel2 = (out2 > 127) ? 0xFF : 0;
            err = out2 - pixel2;
            dest[i + 2] = pixel2;
        }
        
        if (i + 3 < SIZE) {
            int out3 = src[i + 3] + err;
            int pixel3 = (out3 > 127) ? 0xFF : 0;
            err = out3 - pixel3;
            dest[i + 3] = pixel3;
        }
    }
}

int main() {
    for (int i = 0; i < SIZE; i++) {
        src[i] = SRC_VAL(i);
        dest[i] = 0;
    }
    
    dither4();
    
    for (int i = 0; i < SIZE; i++) {
        printf("%d\n", dest[i]);
    }
    
    return 0;
}
