#include "lib/tab_resize_pause.h"

int main(void)
{
  exercise_resize_reads(
    "return function(t,want)\n"
    "  assert(t.field==want.field and t.missing==nil)\n"
    "end", BC_TGETS);
  puts("t-x64-tgets-forward OK");
  return 0;
}
