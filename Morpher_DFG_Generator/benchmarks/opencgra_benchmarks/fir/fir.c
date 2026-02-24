#define NTAPS 32

int input[NTAPS];
int output[NTAPS];
int coefficients[NTAPS] = {025, 150, 375, -225, 050, 075, -300, 125,
025, 150, 375, -225, 050, 075, -300, 125,
025, 150, 375, -225, 050, 075, -300, 125,
025, 150, 375, -225, 050, 075, -300, 125};

void fir();

int main()
{

//  input_dsp (input, NTAPS, 0);

  fir();

//  output_dsp (input, NTAPS, 0);
//  output_dsp (coefficients, NTAPS, 0);
//  output_dsp (output, NTAPS, 0);
  return 0;
}

__attribute__((noinline))
void fir()
{
  int i;
  int j = 0;

 for(j=0; j< NTAPS; ++j) {
    for (i = 0; i < NTAPS; ++i) {
      #ifdef CGRA_COMPILER
      please_map_me();
      #endif
      output[j] += input[i] * coefficients[i];
    }
 }
}

