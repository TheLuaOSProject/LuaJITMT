/*
** Focused regression test for lockless threading.thread live-root validation.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_thr.h"
#include "lj_udata.h"

static GCudata *new_thread_ud(lua_State *L)
{
  GCudata *ud = lj_udata_new(L, sizeof(LJThread), NULL);
  LJThread *th = (LJThread *)uddata(ud);
  memset(th, 0, sizeof(*th));
  lj_thread_udata_rel(th, ud);
  lj_udata_udtype_rel(ud, UDTYPE_THREAD);
  return ud;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  LJThreadLive node;
  GCudata *ud;
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);

  assert(L != NULL);
  g = G(L);

  memset(&node, 0, sizeof(node));
  assert(lj_thread_live_udata_acq(g, NULL) == NULL);
  assert(lj_thread_live_udata_acq(g, &node) == NULL);

  ud = new_thread_ud(L);
  setgcrefrel(node.ud, obj2gco(ud));
  assert(lj_thread_live_udata_acq(g, &node) == ud);

  lj_udata_udtype_rel(ud, UDTYPE_USERDATA);
  assert(lj_thread_live_udata_acq(g, &node) == NULL);
  lj_udata_udtype_rel(ud, UDTYPE_THREAD);

  setgcrefrel(node.ud, bad);
  assert(lj_thread_live_udata_acq(g, &node) == NULL);

  setgcrefrel(node.ud, NULL);
  assert(lj_thread_live_udata_acq(g, &node) == NULL);

  assert(lj_thread_state_udata_acq(g, NULL) == NULL);
  assert(lj_thread_state_udata_acq(g, L) == NULL);
  lj_state_mt_thread_rel(L, ud);
  assert(lj_thread_state_udata_acq(g, L) == ud);
  lj_udata_udtype_rel(ud, UDTYPE_USERDATA);
  assert(lj_thread_state_udata_acq(g, L) == NULL);
  lj_udata_udtype_rel(ud, UDTYPE_THREAD);
  setgcrefrel(L->mt_thread, bad);
  assert(lj_thread_state_udata_acq(g, L) == NULL);
  lj_state_mt_thread_clear_rel(L);
  assert(lj_thread_state_udata_acq(g, L) == NULL);

  lua_close(L);
  printf("t-threading-live-root OK: threading userdata candidates validated\n");
  return 0;
}
