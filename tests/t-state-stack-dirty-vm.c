/*
** Focused test for the assembly-callable VM stack-dirty helper.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *main_tg;
  TGState owner_tg;
  lua_State *co;
  uint64_t main_epoch, owner_epoch;

  assert(L != NULL);
  g = G(L);
  main_tg = G2TG(g);
  assert(main_tg != NULL && L2TG(L) == main_tg);

  main_epoch = lj_tg_stack_dirty_epoch_acq(main_tg);
  lj_state_stack_dirty_vm(L);
  assert(lj_tg_stack_dirty_epoch_acq(main_tg) == main_epoch + 1u);

  co = lua_newthread(L);
  assert(co != NULL);
  lj_tg_init_thread(g, &owner_tg, co, 0);
  assert(L2TG(co) == &owner_tg);

  main_epoch = lj_tg_stack_dirty_epoch_acq(main_tg);
  owner_epoch = lj_tg_stack_dirty_epoch_acq(&owner_tg);
  lj_state_stack_dirty_vm(co);
  assert(lj_tg_stack_dirty_epoch_acq(&owner_tg) == owner_epoch + 1u);
  assert(lj_tg_stack_dirty_epoch_acq(main_tg) == main_epoch);

  /* owner_tg was never attached or registered. Remove its sole external path
  ** before destroying this stack-resident TG; fini does not clear tg_hint. */
  assert(co->tg_hint == &owner_tg);
  co->tg_hint = NULL;
  assert(lj_tg_fini_thread(g, &owner_tg));
  lua_pop(L, 1);
  lua_close(L);
  puts("VM stack dirty helper tests passed");
  return 0;
}
