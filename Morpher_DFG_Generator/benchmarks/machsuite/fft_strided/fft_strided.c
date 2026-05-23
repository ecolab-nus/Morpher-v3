/* MachSuite: fft/strided — Cooley-Tukey FFT with strided butterfly.
   CGRA kernel: inner "odd" loop processes all butterfly pairs at the current
   span, performing complex multiply-add with pre-computed twiddle factors. */

#include <stdio.h>

#define FFT_SIZE 256

float real_in[FFT_SIZE];
float img_in[FFT_SIZE];
float real_twid[FFT_SIZE/2];
float img_twid[FFT_SIZE/2];

__attribute__((noinline))
void fft_strided() {
    int even, odd, block, offset, span, log, rootindex;
    float temp, prod0, prod1;

    log = 0;
    for (span = FFT_SIZE >> 1; span; span >>= 1, log++) {
        for (block = 0; block < FFT_SIZE; block += (span << 1)) {
            for (offset = 0; offset < span; offset++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            even = block + offset;
            odd = even + span;

            temp         = real_in[even] + real_in[odd];
            real_in[odd] = real_in[even] - real_in[odd];
            real_in[even]= temp;

            temp        = img_in[even] + img_in[odd];
            img_in[odd] = img_in[even] - img_in[odd];
            img_in[even]= temp;

            rootindex = (even << log) & (FFT_SIZE - 1);
            if (rootindex) {
                prod0        = real_twid[rootindex] * real_in[odd];
                prod1        = img_twid[rootindex]  * img_in[odd];
                temp         = prod0 - prod1;
                prod0        = real_twid[rootindex] * img_in[odd];
                prod1        = img_twid[rootindex]  * real_in[odd];
                img_in[odd]  = prod0 + prod1;
                real_in[odd] = temp;
            }
            }
        }
    }
}

int main() {
    int i;
    for (i = 0; i < FFT_SIZE; i++) {
        real_in[i] = (i < FFT_SIZE/2) ? 1.0f : 0.0f;
        img_in[i]  = 0.0f;
    }
    for (i = 0; i < FFT_SIZE/2; i++) {
        real_twid[i] = (i & 1) ? 0.0f : 1.0f;
        img_twid[i]  = (i & 1) ? 1.0f : 0.0f;
    }
    fft_strided();
    for (i = 0; i < 4; i++)
        printf("real[%d]=%.4f img[%d]=%.4f\n", i, real_in[i], i, img_in[i]);
    return 0;
}
