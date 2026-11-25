/* An exact VM array update preserves a string leaf without invalidating the
** current strong parent scan.  Keep raw and metatable cases conservative. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_tab.h"

static int move_source, expect_barrier;
static uint32_t previous_mark;

LJGC2TableStoreGuardResult __real_lj_gc2_table_store_finish(
  lua_State *L, LJGC2TableStoreGuard *guard, int committed);
LJGC2TableStoreGuardResult __wrap_lj_gc2_table_store_finish(
  lua_State *L, LJGC2TableStoreGuard *guard, int committed)
{
  LJGC2TableStoreGuardResult result =
    __real_lj_gc2_table_store_finish(L, guard, committed);
  if (move_source && committed) {
    TValue *before = tvref(L->stack);
    move_source = 0;
    assert(lua_checkstack(L, 4096));
    assert(tvref(L->stack) != before);
    previous_mark = lj_tg_mark_active_acq(L2TG(L));
    lj_tg_mark_active_rel(L2TG(L), 1);
    expect_barrier = 1;
  }
  return result;
}

void __real_lj_gc_pubtabtv_vm(lua_State *L, GCtab *parent, cTValue *value);
void __wrap_lj_gc_pubtabtv_vm(lua_State *L, GCtab *parent, cTValue *value)
{
  if (expect_barrier) {
    assert(value == L->top - 1);
    lj_tg_mark_active_rel(L2TG(L), previous_mark);
    expect_barrier = 0;
  }
  __real_lj_gc_pubtabtv_vm(L, parent, value);
}

static void drive(lua_State *L, GCtab *table)
{
  global_State *g = G(L);
  unsigned pass;
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  for (pass = 0; pass < 3000; pass++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  for (pass = 0; pass < 10000; pass++) {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_sweep_bridge_ready_acq(g) &&
        lj_gc2_test_table_scan_current(g, table))
      break;
    (void)lj_gc2_step_explicit(L, 1);
  }
  assert(pass < 10000);
}

static void put_leaf(lua_State *L, GCtab *table, const char *text)
{
  TValue *slot, *value;
  lua_pushstring(L, text);
  value = L->top - 1;
  slot = tvref(table->array) + 1;
  assert(table->asize > 1);
  (void)lj_tab_storetv_forvm_array(L, table, slot, value, 1);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *table;
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  lua_createtable(L, 4, 0);
  table = tabV(L->top - 1);
  lua_pushliteral(L, "old array leaf");
  lua_rawseti(L, 1, 1);
  drive(L, table);

  put_leaf(L, table, "new array leaf");
  assert(lj_gc2_test_table_scan_current(g, table));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_rawgeti(L, 1, 1);
  assert(strcmp(lua_tostring(L, -1), "new array leaf") == 0);
  lua_pop(L, 1);

  drive(L, table);
  lua_pushliteral(L, "put by raw API");
  lua_rawseti(L, 1, 1);
  assert(!lj_gc2_test_table_scan_current(g, table));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  drive(L, table);
  move_source = 1;
  put_leaf(L, table, "weak fallback leaf");
  assert(!move_source && !expect_barrier);
  assert(!lj_gc2_test_table_scan_current(g, table));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_rawgeti(L, 1, 1);
  assert(strcmp(lua_tostring(L, -1), "weak fallback leaf") == 0);

  lua_close(L);
  puts("t-gc2-array-leaf OK");
  return 0;
}
