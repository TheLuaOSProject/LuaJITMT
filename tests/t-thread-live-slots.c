/* Reuse native thread roots while readers retain an earlier Lua handle. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc2.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

static unsigned slots(global_State *g)
{
  LJThreadLive *node;
  unsigned count = 0;
  for (node = lj_thread_live_head_acq(g); node;
       node = lj_thread_live_next_acq(node))
    count++;
  return count;
}

static GCudata *global_handle(lua_State *L, const char *name)
{
  GCudata *ud;
  lua_getglobal(L, name);
  assert(tvisudata(L->top - 1));
  ud = udataV(L->top - 1);
  assert(lj_udata_udtype_acq(ud) == UDTYPE_THREAD);
  lua_pop(L, 1);
  return ud;
}

static void run(int workers)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  LJGC2Lease old_lease, new_lease;
  LJThreadLive *slot;
  GCudata *old, *current;
  lua_pushinteger(L, workers);
  lua_setglobal(L, "workers");
  ljt_lua_dostring(L,
    "th = require('threading')\n"
    "th.gcworkers(workers)\n"
    "function work(x) return {value=x} end\n"
    "function start(x) return th.spawn(work, x) end\n"
    "function result(h) local ok,v=h:join(); assert(ok); return v.value end\n"
    "gate = th.channel(1)\n"
    "sentinel = th.spawn(function(c) c:recv() end, gate)\n"
    "first, second = start(1), start(2)\n");
  assert(slots(g) == 3);
  old = global_handle(L, "first");
  slot = lj_thread_live_node_acq((LJThread *)uddata(old));
  assert(slot && lj_thread_live_udata_acq(g, slot, &old_lease) == old);
  ljt_lua_dostring(L, "assert(result(first) == 1); replacement = start(3)");
  current = global_handle(L, "replacement");
  assert(current != old && lj_thread_live_node_acq((LJThread *)uddata(old)) == NULL);
  assert(lj_thread_live_node_acq((LJThread *)uddata(current)) == slot);
  assert(lj_thread_live_udata_acq(g, slot, &new_lease) == current);
  assert(lj_udata_udtype_acq(old) == UDTYPE_THREAD);
  lj_gc2_lease_release(&new_lease);
  lj_gc2_lease_release(&old_lease);
  ljt_lua_dostring(L,
    "assert(result(second) == 2 and result(replacement) == 3)\n"
    "replacement, second = nil, nil\n"
    "for i=1,16 do\n"
    "  local a, b = start(i), start(-i)\n"
    "  collectgarbage()\n"
    "  assert(result(a) == i and result(b) == -i)\n"
    "end\n"
    "assert(result(first) == 1)\n");
  assert(slots(g) == 3 && la_load32_acq(&g->threading_live_count) == 1);
  ljt_lua_dostring(L,
    "local function nested(k)\n"
    "  for i=1,8 do assert(result(start(k+i)) == k+i) end\n"
    "  return k\n"
    "end\n"
    "local a,b = th.spawn(nested,10), th.spawn(nested,20)\n"
    "local ok,n = a:join(); assert(ok and n == 10)\n"
    "ok,n = b:join(); assert(ok and n == 20)\n"
    "gate:send(1); assert(sentinel:join()); th.gcworkers(0)\n");
  assert(la_load32_acq(&g->threading_live_count) == 0);
  lua_close(L);
}

int main(void)
{
  run(0);
  run(2);
  puts("reusable thread live roots OK");
  return 0;
}
