#include <stddef.h>
#include <stdio.h>
#include "lj_dispatch.h"
#define SIZE(T) printf("sizeof.%s=%zu\n", #T, sizeof(T))
#define OFF(T, F) printf("offset.%s.%s=%zu\n", #T, #F, offsetof(T, F))
int main(void) {
  SIZE(void *); SIZE(GC2State); SIZE(global_State); SIZE(GG_State); SIZE(lua_State);
  OFF(GC2State, grey_capacity); OFF(GC2State, grey_top);
#ifdef FAIR_VARIANT
  OFF(GC2State, sweep_owner_next_tid);
#endif
  OFF(GC2State, grey_bottom); OFF(GC2State, worker_active);
  OFF(GC2State, deferred_epoch); OFF(GC2State, tg_list);
  OFF(global_State, gc); OFF(global_State, vmstate); OFF(global_State, cur_L);
  OFF(global_State, jit_base); OFF(global_State, gc2);
  OFF(global_State, gc2.phase); OFF(global_State, gc2.cycle);
  OFF(global_State, gc2.jit_phase_gate); OFF(global_State, gc2.n_workers);
  OFF(global_State, gc2.grey_top); OFF(global_State, main_tg);
  OFF(global_State, mt_active); OFF(global_State, mt_entering);
  OFF(global_State, tab_resize); OFF(GG_State, g); OFF(GG_State, J);
  OFF(GG_State, hotcount); OFF(GG_State, dispatch); OFF(GG_State, main_tg);
  printf("GG_G2J=%d\nGG_G2DISP=%d\n", GG_G2J, GG_G2DISP);
  return 0;
}
