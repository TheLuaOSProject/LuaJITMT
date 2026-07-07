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
  th->ud = ud;
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

  lua_close(L);
  printf("t-threading-live-root OK: live-root userdata candidates validated\n");
  return 0;
}
