/*
** Focused x64 VM coverage for rooted rawget/TGETR point reads.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_arch.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_obj.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_tg.h"

/* Built by the M5 harness with LJ_GC2_TEST_HELPERS enabled. */

#if LJ_TARGET_X64

static uint32_t global_anchor_gc_hits;

static void global_anchor_full_gc_hook(lua_State *L, TGState *tg,
				       uint32_t idx, TValue *slot)
{
  TValue snap;
  UNUSED(tg);
  UNUSED(idx);
  lj_tv_load_acq(&snap, slot);
  assert(tvisnil(&snap));  /* First meta-chain anchor, before env capture. */
  global_anchor_gc_hits++;
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);
  la_store64_rel(&g->gc2.hs_epoch, epoch);
  la_store32_rel(&tg->reqmask, actions);
  la_store32_rel(&tg->poll, 1);
}

static int arm_retry(lua_State *L)
{
  UNUSED(L);
  lj_tab_test_forjit_initial_miss_once();
  return 0;
}

static int arm_stopreq_retry(lua_State *L)
{
  lj_tab_test_forjit_initial_miss_once();
  publish_manual(G(L), L2TG(L), LJ_GC2_HS_STOPREQ);
  return 0;
}

static int arm_global_anchor_gc_retry(lua_State *L)
{
  UNUSED(L);
  lj_tg_root_test_set_push_hook(global_anchor_full_gc_hook);
  lj_tab_test_forjit_initial_miss_once();
  return 0;
}

static void assert_lua_ok(lua_State *L, int rc, const char *where)
{
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "%s: %s\n", where, err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
}

static void patch_tgetv_to_tgetr(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  BCIns *bc;
  BCPos i;
  int found = 0;

  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  bc = proto_bc(pt);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_TGETV) {
      /* This deliberately exercises the valid table/result alias shape. */
      assert(bc_a(ins) == bc_b(ins));
      setbc_op(&ins, BC_TGETR);
      bc_publish(&bc[i], ins);
      found++;
    }
  }
  lua_pop(L, 1);
  assert(found == 1);
}

static void push_read_args(lua_State *L, const char *name, int key,
			   lua_CFunction arm)
{
  lua_getglobal(L, name);
  lua_getglobal(L, "x64_rooted_read_table");
  lua_pushinteger(L, key);
  lua_pushcfunction(L, arm);
}

static void call_read_string(lua_State *L, const char *name, int key,
			     const char *want)
{
  int rc;
  push_read_args(L, name, key, arm_retry);
  rc = lua_pcall(L, 3, 1, 0);
  assert_lua_ok(L, rc, name);
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), want) == 0);
  lua_pop(L, 1);
}

static void expect_read_stopreq(lua_State *L, const char *name, int key)
{
  TGState *tg = L2TG(L);
  const char *err;
  int rc;

  push_read_args(L, name, key, arm_stopreq_retry);
  rc = lua_pcall(L, 3, 1, 0);
  assert(rc != LUA_OK);
  err = lua_tostring(L, -1);
  assert(err != NULL);
  assert(strstr(err, "thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
}

static void exercise_rooted_vm_reads(lua_State *L)
{
  int rc = luaL_dostring(L,
    "jit.off()\n"
    "x64_rooted_read_table = {\n"
    "  [7] = 'array-value',\n"
    "  [100000] = 'integer-hash-value',\n"
    "  named = 'named-value'\n"
    "}\n"
    "function x64_raw_read(t, k, arm)\n"
    "  arm()\n"
    "  local v = rawget(t, k)\n"
    "  return v\n"
    "end\n"
    "function x64_tgetr_table_alias(t, k, arm)\n"
    "  arm()\n"
    "  t = t[k]\n"
    "  return t\n"
    "end\n");
  assert_lua_ok(L, rc, "load rooted x64 read functions");

  patch_tgetv_to_tgetr(L, "x64_tgetr_table_alias");

  call_read_string(L, "x64_raw_read", 7, "array-value");
  call_read_string(L, "x64_raw_read", 100000, "integer-hash-value");
  call_read_string(L, "x64_tgetr_table_alias", 7, "array-value");
  call_read_string(L, "x64_tgetr_table_alias", 100000,
		   "integer-hash-value");
  expect_read_stopreq(L, "x64_raw_read", 7);
  expect_read_stopreq(L, "x64_tgetr_table_alias", 7);
}

static void exercise_library_tgetr(lua_State *L)
{
  int rc = luaL_dostring(L,
    "jit.off()\n"
    "local a = { 1, 2, 3, 4, 5 }\n"
    "assert(table.remove(a, 3) == 3)\n"
    "assert(table.concat(a, ',') == '1,2,4,5')\n"
    "local b = {}\n"
    "assert(table.move(a, 1, #a, 2, b) == b)\n"
    "assert(b[2] == 1 and b[5] == 5)\n"
    "local sum = 0\n"
    "table.foreachi(a, function(_, v) sum = sum + v end)\n"
    "assert(sum == 12)\n");
  assert_lua_ok(L, rc, "library TGETR semantics");
}

static void call_global_read(lua_State *L, lua_CFunction arm,
			     const char *want, int expect_ok)
{
  int rc;
  lua_getglobal(L, "x64_global_rooted_read");
  lua_pushcfunction(L, arm);
  rc = lua_pcall(L, 1, 1, 0);
  if (expect_ok) {
    assert_lua_ok(L, rc, "rooted global read");
    assert(lua_isstring(L, -1));
    assert(strcmp(lua_tostring(L, -1), want) == 0);
  } else {
    const char *err;
    assert(rc != LUA_OK);
    err = lua_tostring(L, -1);
    assert(err != NULL);
    assert(strstr(err, "thread interrupted: VM shutdown") != NULL);
  }
  lua_pop(L, 1);
}

static void call_global_write(lua_State *L, lua_CFunction arm,
			      const char *value, int expect_ok)
{
  int rc;
  lua_getglobal(L, "x64_global_rooted_write");
  lua_pushstring(L, value);
  lua_pushcfunction(L, arm);
  rc = lua_pcall(L, 2, 1, 0);
  if (expect_ok) {
    assert_lua_ok(L, rc, "rooted global write");
    assert(lua_isstring(L, -1));
    assert(strcmp(lua_tostring(L, -1), value) == 0);
  } else {
    const char *err;
    assert(rc != LUA_OK);
    err = lua_tostring(L, -1);
    assert(err != NULL);
    assert(strstr(err, "thread interrupted: VM shutdown") != NULL);
  }
  lua_pop(L, 1);
}

static void exercise_rooted_global_env(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint32_t hooks0 = global_anchor_gc_hits;
  int rc = luaL_dostring(L,
    "jit.off()\n"
    "local th = require('threading')\n"
    "assert(({ th.spawn(function() return true end):join(5) })[1])\n"
    "local readleaf = { x64_rooted_global_value = 'global-rooted' }\n"
    "local writeleaf = {}\n"
    "local env = { x64_rooted_global_writeleaf = writeleaf }\n"
    "setmetatable(env, { __index = readleaf, __newindex = writeleaf })\n"
    "function x64_global_rooted_read(arm)\n"
    "  arm(); return x64_rooted_global_value\n"
    "end\n"
    "setfenv(x64_global_rooted_read, env)\n"
    "function x64_global_rooted_write(value, arm)\n"
    "  arm(); x64_rooted_global_sink = value\n"
    "  return x64_rooted_global_writeleaf.x64_rooted_global_sink\n"
    "end\n"
    "setfenv(x64_global_rooted_write, env)\n"
    "local fnenv = {}\n"
    "fnenv.x64_rooted_function_env = fnenv\n"
    "setmetatable(fnenv, {\n"
    "  __index = function(_, key)\n"
    "    if key == 'x64_rooted_global_call_value' then\n"
    "      return 'global-call-rooted'\n"
    "    end\n"
    "  end,\n"
    "  __newindex = function(self, key, value)\n"
    "    rawset(self, 'captured_' .. key, value)\n"
    "  end\n"
    "})\n"
    "function x64_global_rooted_call_read(arm)\n"
    "  arm(); return x64_rooted_global_call_value\n"
    "end\n"
    "setfenv(x64_global_rooted_call_read, fnenv)\n"
    "function x64_global_rooted_call_write(value, arm)\n"
    "  arm(); x64_rooted_global_call_sink = value\n"
    "  return x64_rooted_function_env.captured_x64_rooted_global_call_sink\n"
    "end\n"
    "setfenv(x64_global_rooted_call_write, fnenv)\n");
  assert_lua_ok(L, rc, "load rooted global environment functions");

  call_global_read(L, arm_global_anchor_gc_retry, "global-rooted", 1);
  call_global_write(L, arm_global_anchor_gc_retry, "global-written", 1);
  lua_getglobal(L, "x64_global_rooted_call_read");
  lua_setglobal(L, "x64_global_rooted_read");
  call_global_read(L, arm_global_anchor_gc_retry, "global-call-rooted", 1);
  lua_getglobal(L, "x64_global_rooted_call_write");
  lua_setglobal(L, "x64_global_rooted_write");
  call_global_write(L, arm_global_anchor_gc_retry, "global-call-written", 1);
  assert(global_anchor_gc_hits == hooks0 + 4u);

  call_global_read(L, arm_stopreq_retry, NULL, 0);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
  call_global_write(L, arm_stopreq_retry, "not-written", 0);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
}

#endif

int main(void)
{
#if LJ_TARGET_X64
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  exercise_rooted_vm_reads(L);
  exercise_library_tgetr(L);
  exercise_rooted_global_env(L);
  lua_close(L);
  printf("t-x64-rooted-reads OK: rawget/TGETR/global env use rooted VM reads\n");
#else
  printf("t-x64-rooted-reads SKIP: x64-only VM coverage\n");
#endif
  return 0;
}
