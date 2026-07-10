#include <stdio.h>

#define N 18

void please_map_me(void);

static int input_a[N];
static int input_b[N];

__attribute__((noinline))
int vector_psum_remainder(void) {
  int acc = 7;

  for (int i = 0; i < N; ++i) {
    please_map_me();
    acc += input_a[i] * input_b[i];
  }
  return acc;
}

int main(void) {
  for (int i = 0; i < N; ++i) {
    input_a[i] = i - 4;
    input_b[i] = (i * 3) + 1;
  }

  const int result = vector_psum_remainder();
  printf("%d\n", result);
  return result & 0xff;
}
