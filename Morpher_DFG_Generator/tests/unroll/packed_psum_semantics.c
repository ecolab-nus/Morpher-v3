#include <stdio.h>

#define N 16

void please_map_me(void);

static int input_a[N];
static int input_b[N];

__attribute__((noinline))
int packed_psum(void) {
  int acc = 3;

  for (int i = 0; i < N; ++i) {
    please_map_me();
    acc += input_a[i] * input_b[i];
  }
  return acc;
}

int main(void) {
  for (int i = 0; i < N; ++i) {
    input_a[i] = i + 1;
    input_b[i] = (i * 5) - 7;
  }

  const int result = packed_psum();
  printf("%d\n", result);
  return result & 0xff;
}
