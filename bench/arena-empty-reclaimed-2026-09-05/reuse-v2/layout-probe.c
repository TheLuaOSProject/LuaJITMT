#include <stdio.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lj_dispatch.h"
int main(void) {
  printf("TGAlloc=%zu TGState=%zu GG_State=%zu global_State=%zu\n", sizeof(TGAlloc), sizeof(TGState), sizeof(GG_State), sizeof(global_State));
  return 0;
}
