/*
** Focused x64 VM test for direct TSET barriers.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_tab.h"

#include "lib/lua_fixture_helpers.h"

static uint32_t pubtabtv_calls;

void __real_lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv);

void __wrap_lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv)
{
  pubtabtv_calls++;
  __real_lj_gc_pubtabtv_vm(L, t, tv);
}

static void make_table_black(GCtab *t)
{
  lj_obj_masksetgcflags(obj2gco(t), LJ_GC_COLORS, LJ_GC_BLACK);
  assert(isblack(obj2gco(t)));
  assert((t->marked & LJ_GC_WEAK) == 0);
}

static GCtab *get_global_table(lua_State *L, const char *name)
{
  GCtab *t;
  lua_getglobal(L, name);
  assert(lua_istable(L, -1));
  t = tabV(L->top - 1);
  lua_pop(L, 1);
  return t;
}

static void reset_counter(void)
{
  pubtabtv_calls = 0;
}

static void test_nongc_direct_stores_skip_barrier(lua_State *L, GCtab *t)
{
  make_table_black(t);
  reset_counter();
  ljt_lua_dostring(L,
    "local t = lj_m7_tset_barrier_t\n"
    "local k = lj_m7_tset_barrier_k\n"
    "local dk = lj_m7_tset_barrier_dk\n"
    "local rk = lj_m7_tset_barrier_rk\n"
    "t[1] = 101\n"
    "t[k] = 202\n"
    "t[dk] = -12.5\n"
    "for i = rk, rk do t[i] = 303 end\n"
    "t.stable = 404\n"
    "assert(t[1] == 101 and t[2] == 202 and t[3] == 303)\n"
    "assert(t[4] == -12.5)\n"
    "assert(t.stable == 404)\n");
  assert(pubtabtv_calls == 0);
}

static void test_gc_direct_stores_keep_barrier(lua_State *L, GCtab *t,
					       GCtab *child)
{
  make_table_black(t);
  make_table_black(child);
  reset_counter();
  ljt_lua_dostring(L,
    "local t = lj_m7_tset_barrier_t\n"
    "local k = lj_m7_tset_barrier_k\n"
    "local dk = lj_m7_tset_barrier_dk\n"
    "local rk = lj_m7_tset_barrier_rk\n"
    "local child = lj_m7_tset_barrier_child\n"
    "t[1] = child\n"
    "t[k] = child\n"
    "t[dk] = child\n"
    "for i = rk, rk do t[i] = child end\n"
    "t.stable = child\n"
    "assert(t[1] == child and t[2] == child and t[3] == child)\n"
    "assert(t[4] == child)\n"
    "assert(t.stable == child)\n");
  assert(pubtabtv_calls >= 5u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  GCtab *t, *child;

  ljt_lua_dostring(L,
    "jit.off()\n"
    "lj_m7_tset_barrier_t = { 1, 2, 3, 4, stable = 5 }\n"
    "lj_m7_tset_barrier_k = 2\n"
    "lj_m7_tset_barrier_dk = 4\n"
    "lj_m7_tset_barrier_rk = 3\n"
    "lj_m7_tset_barrier_child = {}\n");

  t = get_global_table(L, "lj_m7_tset_barrier_t");
  child = get_global_table(L, "lj_m7_tset_barrier_child");

  test_nongc_direct_stores_skip_barrier(L, t);
  test_gc_direct_stores_keep_barrier(L, t, child);

  lua_close(L);
  printf("t-x64-tset-nongc-barrier OK: non-GC direct TSET skips VM barrier\n");
  return 0;
}
