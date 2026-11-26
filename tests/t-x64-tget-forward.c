#include "lib/tab_resize_pause.h"

int main(void)
{
  exercise_resize_reads(
    "return function(t,want)\n"
    "  local function getv(a,k) return a[k] end\n"
    "  assert(t[64]==want[64] and t[3]==33)\n"
    "  for k=1,64 do assert(t[k]==want[k] and getv(t,k)==want[k]) end\n"
    "  assert(t[65]==nil)\n"
    "end", BC_TGETB);
  puts("t-x64-tget-forward OK");
  return 0;
}
