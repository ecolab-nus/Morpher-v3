extern int please_map_me(void);

int a[16];
int b[16];
int out[16];
volatile int result;

__attribute__((noinline))
void zero_seeded_mac(void) {
  int acc = 0;
  for (int i = 0; i < 16; ++i) {
    please_map_me();
    acc += a[i] * b[i];
  }
  result = acc;
}

__attribute__((noinline))
void nonzero_seeded_mac(void) {
  int acc = 7;
  for (int i = 0; i < 16; ++i) {
    please_map_me();
    acc += a[i] * b[i];
  }
  result = acc;
}

__attribute__((noinline))
void nonconstant_seeded_mac(int seed) {
  int acc = seed;
  for (int i = 0; i < 16; ++i) {
    please_map_me();
    acc += a[i] * b[i];
  }
  result = acc;
}

__attribute__((noinline))
void predicated_select(void) {
  for (int i = 0; i < 16; ++i) {
    please_map_me();
    if (a[i] > b[i])
      out[i] = a[i] - b[i];
    else
      out[i] = a[i] + b[i];
  }
}
