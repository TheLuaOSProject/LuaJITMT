/* A worker beginning a cycle must allocate queue storage on its own arena. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

enum { NORMAL, FAIL_WEAK, FAIL_FINALIZER };
static MSize weak_cap, fin_cap;
static _Thread_local GCSize first_size, second_size;
static _Thread_local void *partial;
static _Thread_local unsigned rejected, rolled_back;

void *__real_lj_mem_new_tg_nothrow(global_State *g, GCSize size);
void *__wrap_lj_mem_new_tg_nothrow(global_State *g, GCSize size)
{
  void *p;
  if (partial && size == second_size) {
    rejected++;
    return NULL;
  }
  p = __real_lj_mem_new_tg_nothrow(g, size);
  if (size == first_size) {
    assert(!partial && p);
    partial = p;
  }
  return p;
}

void __real_lj_mem_free(global_State *g, void *p, size_t osize);
void __wrap_lj_mem_free(global_State *g, void *p, size_t osize)
{
  if (p && p == partial) {
    assert(osize == first_size && rolled_back < rejected);
    rolled_back++;
    partial = NULL;
  }
  __real_lj_mem_free(g, p, osize);
}

static void check_owner(void *p, uint32_t tid)
{
  assert(p && lj_arena_owner_acq(lj_arena_of(p)) == tid);
}

static int collect_on_worker(lua_State *L)
{
  global_State *g = G(L);
  int mode = (int)lua_tointeger(L, 1);
  uint32_t tid = lj_tg_tid_acq(L2TG(L));
  assert(tid != lj_tg_tid_acq(g->main_tg));
  assert(gc2_weak_capacity_acq(g) == 0);
  assert(gc2_finreg_cdata_preclaim_capacity_acq(g) == 0);
  if (mode == FAIL_WEAK) {
    first_size = weak_cap * sizeof(GCRef);
    second_size = weak_cap;
  } else if (mode == FAIL_FINALIZER) {
    first_size = fin_cap * sizeof(GCRef);
    second_size = fin_cap * sizeof(TValue);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (mode != NORMAL) {
    assert(rejected && rolled_back == rejected && !partial);
    if (mode == FAIL_WEAK) {
      assert(!gc2_weak_stack_acq(g) && !gc2_weak_ready_acq(g));
      assert(gc2_weak_capacity_acq(g) == 0);
    } else {
      assert(!gc2_finreg_cdata_preclaim_objvec_acq(g));
      assert(!gc2_finreg_cdata_preclaim_finvec_acq(g));
      assert(gc2_finreg_cdata_preclaim_capacity_acq(g) == 0);
    }
    first_size = second_size = 0;
    lua_gc(L, LUA_GCCOLLECT, 0);
  }
  check_owner(gc2_weak_stack_acq(g), tid);
  check_owner(gc2_weak_ready_acq(g), tid);
  check_owner(gc2_finreg_cdata_preclaim_objvec_acq(g), tid);
  check_owner(gc2_finreg_cdata_preclaim_finvec_acq(g), tid);
  weak_cap = gc2_weak_capacity_acq(g);
  fin_cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  if (mode == NORMAL) {
    GC2WeakOverflow *node;
    lua_pushinteger(L, 2 * weak_cap);
    lua_setglobal(L, "weak_count");
    ljt_lua_dostring(L,
      "weak = {}; local mt = {__mode='v'}\n"
      "for i=1,weak_count do weak[i] = setmetatable({{}}, mt) end\n");
    lua_gc(L, LUA_GCCOLLECT, 0);
    node = gc2_weak_overflow_acq(g);
    check_owner(node, tid);
    ljt_lua_dostring(L,
      "for i=1,weak_count do assert(weak[i][1] == nil) end\n");
  }
  return 0;
}

static void run(int mode)
{
  lua_State *L = luaL_newstate();
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  luaL_openlibs(L);
  lua_pushinteger(L, mode);
  lua_setglobal(L, "mode");
  lua_pushcfunction(L, collect_on_worker);
  lua_setglobal(L, "collect_on_worker");
  ljt_lua_dostring(L,
    "local t = require('threading').spawn(collect_on_worker, mode)\n"
    "assert(t:join())\n"
    "collectgarbage()\n");
  lua_close(L);
}

int main(void)
{
  run(NORMAL);
  assert(weak_cap && fin_cap);
  run(FAIL_WEAK);
  run(FAIL_FINALIZER);
  puts("worker-owned weak and finalizer queues OK");
  return 0;
}
