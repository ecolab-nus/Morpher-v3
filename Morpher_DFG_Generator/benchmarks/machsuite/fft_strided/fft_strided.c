/* MachSuite: fft/strided — Cooley-Tukey FFT with strided butterfly.
   CGRA kernel: inner "odd" loop processes all butterfly pairs at the current
   span, performing complex multiply-add with pre-computed twiddle factors. */

#include <stdio.h>
#include <math.h>

#define FFT_SIZE 32
#define twoPI    6.28318530717959

double real_in[FFT_SIZE];
double img_in[FFT_SIZE];
double real_twid[FFT_SIZE/2];
double img_twid[FFT_SIZE/2];

__attribute__((noinline))
void fft_strided() {
    int even, odd, span, log, rootindex;
    double temp;

    log = 0;
    for (span = FFT_SIZE >> 1; span; span >>= 1, log++) {
        for (odd = span; odd < FFT_SIZE; odd++) {
            #ifdef CGRA_COMPILER
            please_map_me();
            #endif
            odd |= span;
            even = odd ^ span;

            temp         = real_in[even] + real_in[odd];
            real_in[odd] = real_in[even] - real_in[odd];
            real_in[even]= temp;

            temp        = img_in[even] + img_in[odd];
            img_in[odd] = img_in[even] - img_in[odd];
            img_in[even]= temp;

            rootindex = (even << log) & (FFT_SIZE - 1);
            if (rootindex) {
                temp         = real_twid[rootindex] * real_in[odd]
                             - img_twid[rootindex]  * img_in[odd];
                img_in[odd]  = real_twid[rootindex] * img_in[odd]
                             + img_twid[rootindex]  * real_in[odd];
                real_in[odd] = temp;
            }
        }
    }
}

int main() {
    int i;
    for (i = 0; i < FFT_SIZE; i++) {
        real_in[i] = (i < FFT_SIZE/2) ? 1.0 : 0.0;
        img_in[i]  = 0.0;
    }
    for (i = 0; i < FFT_SIZE/2; i++) {
        real_twid[i] = cos(twoPI * i / FFT_SIZE);
        img_twid[i]  = sin(twoPI * i / FFT_SIZE);
    }
    fft_strided();
    for (i = 0; i < 4; i++)
        printf("real[%d]=%.4f img[%d]=%.4f\n", i, real_in[i], i, img_in[i]);
    return 0;
}
