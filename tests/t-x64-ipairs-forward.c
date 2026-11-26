#include "lib/tab_resize_pause.h"

int main(void)
{
  exercise_resize_reads(
    "return function(t,want)\n"
    "  local n=0\n"
    "  for i,v in ipairs(t) do n=n+1; assert(i==n and v==want[i]) end\n"
    "  assert(n==64)\n"
    "end", BC_ITERC);
  puts("t-x64-ipairs-forward OK");
  return 0;
}
