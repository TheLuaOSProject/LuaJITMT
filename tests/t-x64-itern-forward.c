#include "lib/tab_resize_pause.h"

int main(void)
{
  exercise_resize_reads(
    "return function(t,want)\n"
    "  local seen,n={},0\n"
    "  for k,v in pairs(t) do\n"
    "    assert(not seen[k] and v==want[k]); seen[k]=true; n=n+1\n"
    "  end\n"
    "  assert(n==67)\n"
    "end", BC_ITERN);
  puts("t-x64-itern-forward OK");
  return 0;
}
