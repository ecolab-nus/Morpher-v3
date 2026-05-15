#define SIZE 64
#define TARGET 137

int node_data[SIZE];
int node_next[SIZE];
int head_idx = 0;
int llist_result = 0;

__attribute__((noinline))
int llist(int target)
{
  int idx = head_idx;
  int found = 0;

  for (int step = 0; step < SIZE; step++) {
#ifdef CGRA_COMPILER
    please_map_me();
#endif
    int active = idx >= 0;
    int safe_idx = active ? idx : 0;
    int value = node_data[safe_idx];
    int next = node_next[safe_idx];
    found |= active && (value == target);
    idx = active ? next : -1;
  }

  return found;
}

static void init_list()
{
  for (int i = 0; i < SIZE; i++) {
    node_data[i] = (i * 13) & 255;
    node_next[i] = i + 1;
  }
  node_data[37] = TARGET;
  node_next[SIZE - 1] = -1;
}

int main()
{
  init_list();
  llist_result = llist(TARGET);
  return llist_result != 1;
}
