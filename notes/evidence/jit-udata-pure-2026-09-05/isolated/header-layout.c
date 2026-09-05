#include <stdio.h>
#include "lj_obj.h"
#include "lj_jit.h"
int main(void) {
  printf("global-size=%zu mt-active=%zu mt-entering=%zu jit-size=%zu loop-flag=%zu\n",
         sizeof(global_State), offsetof(global_State, mt_active),
         offsetof(global_State, mt_entering), sizeof(jit_State),
         offsetof(jit_State, loop_cdata_fload));
  return 0;
}
