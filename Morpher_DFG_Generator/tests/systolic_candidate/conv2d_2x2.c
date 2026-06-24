int input[18 * 18];
int output[18 * 18];

__attribute__((noinline))
void conv2d_2x2(void) {
  for (int i = 1; i < 17; ++i) {
    for (int j = 1; j < 17; ++j) {
      output[i * 18 + j] =
          2 * input[(i - 1) * 18 + (j - 1)] -
          3 * input[i * 18 + (j - 1)] +
          5 * input[(i - 1) * 18 + j] +
          6 * input[i * 18 + j];
    }
  }
}
