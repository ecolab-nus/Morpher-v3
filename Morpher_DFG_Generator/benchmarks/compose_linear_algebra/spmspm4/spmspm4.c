#define rows 8
#define cols 8
#define valid_points_a 8
#define valid_points_b 256

int data_a[valid_points_a];
int colindex_a[valid_points_a];
int rowptr_a[rows + 1];

int data_b[valid_points_b];
int colindex_b[valid_points_b];
int rowptr_b[rows + 1];

int out[rows][cols] = {0};
int spmspm4_result = 0;

__attribute__((noinline))
int spmspm4()
{
  for (int i = 0; i < rows; i++) {
    for (int k = rowptr_a[i]; k < rowptr_a[i + 1]; k++) {
      int m = colindex_a[k];
      int n = rowptr_b[m];
      int end = rowptr_b[m + 1];

      for (; n + 3 < end; n += 4) {
#ifdef CGRA_COMPILER
        please_map_me();
#endif
        int idx0 = colindex_b[n];
        int idx1 = colindex_b[n + 1];
        int idx2 = colindex_b[n + 2];
        int idx3 = colindex_b[n + 3];

        out[i][idx0] += data_a[k] * data_b[n];
        out[i][idx1] += data_a[k] * data_b[n + 1];
        out[i][idx2] += data_a[k] * data_b[n + 2];
        out[i][idx3] += data_a[k] * data_b[n + 3];
      }

      for (; n < end; n++) {
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
    colindex_b[n] = n & (cols - 1);
    data_b[n] = (n % 11) + 1;
  }
}

int main()
{
  init_inputs();
  spmspm4_result = spmspm4();
  return spmspm4_result == -1;
}
