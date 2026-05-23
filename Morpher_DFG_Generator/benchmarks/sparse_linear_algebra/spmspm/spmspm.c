#define rows 8
#define cols 128
#define valid_points_a 8
#define valid_points_b 128

int data_a[valid_points_a];
int colindex_a[valid_points_a];
int rowptr_a[rows + 1];

int data_b[valid_points_b];
int colindex_b[valid_points_b];
int rowptr_b[rows + 1];

int out[rows][cols] = {0};
int spmspm_result = 0;

__attribute__((noinline))
int spmspm()
{
  for (int i = 0; i < rows; i++) {
    for (int k = rowptr_a[i]; k < rowptr_a[i + 1]; k++) {
      int m = colindex_a[k];

      for (int n = rowptr_b[m]; n < rowptr_b[m + 1]; n++) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        out[i][colindex_b[n]] += data_a[k] * data_b[n];
      }
    }
  }

  return out[0][0] + out[1][1] + out[2][2] + out[3][3] +
         out[4][4] + out[5][5] + out[6][6] + out[7][7];
}

static void init_inputs()
{
  for (int i = 0; i < rows; i++) {
    rowptr_a[i] = i;
    colindex_a[i] = 0;
    data_a[i] = i + 1;
    rowptr_b[i] = valid_points_b;
  }
  rowptr_a[rows] = valid_points_a;
  rowptr_b[0] = 0;
  rowptr_b[rows] = valid_points_b;

  for (int n = 0; n < valid_points_b; n++) {
    colindex_b[n] = n;
    data_b[n] = (n % 11) + 1;
  }
}

int main()
{
  init_inputs();
  spmspm_result = spmspm();
  return spmspm_result == -1;
}
