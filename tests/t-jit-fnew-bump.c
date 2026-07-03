/*
** Focused regression test for the numeric one-upvalue FNEW bump-pair path.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_FUNC_TEST_HELPERS
#error "t-jit-fnew-bump requires LJ_FUNC_TEST_HELPERS"
#endif

static void run_script(lua_State *L, const char *code, const char *label)
{
  UNUSED(label);
  ljt_lua_dostring(L, code);
}

static GCproto *first_child_proto(GCproto *pt)
{
  MSize i;
  assert((pt->flags & PROTO_CHILD) != 0);
  for (i = 0; i < pt->sizekgc; i++) {
    GCobj *o = proto_kgc(pt, ~(ptrdiff_t)i);
    if (o->gch.gct == ~LJ_TPROTO)
      return gco2pt(o);
  }
  assert(0 && "missing child proto");
  return NULL;
}

static GCfunc *top_lfunc(lua_State *L)
{
  GCfunc *fn;
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  return fn;
}

static void test_traced_behavior(lua_State *L)
{
  uint32_t fallback0;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 100 do\n"
    "  local x = i\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n"
    "assert(t[1]() == 2)\n"
    "assert(t[2]() == 3)\n"
    "assert(t[100]() == 101)\n"
    "assert(t[1]() == 3)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "local name = debug.setupvalue(t[1], 1, 50)\n"
    "assert(name == 'x', name)\n"
    "assert(t[1]() == 51)\n"
    "assert(t[2]() == 4)\n";

  lj_func_test_reset_gc1num_bump_fallback_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  run_script(L, code, "numeric FNEW traced behavior");

  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
}

static void test_traced_mark_active_fallback(lua_State *L, global_State *g,
					     TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 100 do\n"
    "  local x = i\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n"
    "assert(t[1]() == 2)\n"
    "assert(t[2]() == 3)\n"
    "assert(t[100]() == 101)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n";

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  helper0 = lj_func_test_gc1num_bump_fast_calls() +
	    lj_func_test_gc1num_bump_fallback_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  run_script(L, code, "numeric FNEW traced mark-active fallback");

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = lj_func_test_gc1num_bump_fast_calls() +
	    lj_func_test_gc1num_bump_fallback_calls();
  assert(helper1 > helper0);
}

static void test_traced_alloc_black_inline(lua_State *L, global_State *g,
					   TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 100 do\n"
    "  local x = i\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n"
    "assert(t[1]() == 2)\n"
    "assert(t[2]() == 3)\n"
    "assert(t[100]() == 101)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n";

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  helper0 = lj_func_test_gc1num_bump_fast_calls() +
	    lj_func_test_gc1num_bump_fallback_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  lj_tg_mark_active_rel(tg, 0);
  lj_tg_alloc_black_rel(tg, 1);

  run_script(L, code, "numeric FNEW traced alloc-black inline");

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = lj_func_test_gc1num_bump_fast_calls() +
	    lj_func_test_gc1num_bump_fallback_calls();
  assert(helper1 == helper0);
}

static void load_one_upvalue_fixture(lua_State *L, GCfunc **parentp,
				     GCproto **childp, int32_t *slotnop)
{
  GCfunc *parent;
  GCproto *child;
  uint32_t uvdesc;

  assert(luaL_loadstring(L,
    "return function()\n"
    "  local x = 0\n"
    "  return function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  parent = top_lfunc(L);
  child = first_child_proto(funcproto(parent));
  assert(child->sizeuv == 1);
  assert(proto_celluv(child));
  uvdesc = proto_uv(child)[0];
  assert((uvdesc & PROTO_UV_LOCAL) != 0);
  *slotnop = (int32_t)(uvdesc & 0xffu);
  *parentp = parent;
  *childp = child;
}

static void assert_one_upvalue_result(GCfunc *fn, TValue *slot, int32_t value)
{
  GCupval *uv;
  assert(fn->l.nupvalues == 1);
  uv = func_uv_acq(&fn->l, 0);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tvisnumber(&uv->tv));
  assert((int32_t)numberVnum(&uv->tv) == value);
  assert(tvisgcv(slot) && gcV(slot) == obj2gco(uv));
}

static void test_accounting_fast_direct(lua_State *L, global_State *g,
					TGState *tg)
{
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  int32_t slotno;
  UNUSED(g);

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  la_store64_rel(&tg->local_total, 0);
  setnumV(&slots[slotno], 123);

  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 123);
  assert_one_upvalue_result(fn, &slots[slotno], 123);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_tg_local_total_acq(tg) > 0);
  lua_pop(L, 1);
}

static void test_accounting_fallback(lua_State *L, global_State *g,
				     TGState *tg)
{
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  setnumV(&slots[slotno], 123);

  lj_gc_threshold_store(g, lj_gc_total_load(g) + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH - 1u);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 123);
  assert_one_upvalue_result(fn, &slots[slotno], 123);
  assert(lj_func_test_gc1num_bump_fallback_calls() > fallback0);
  assert(lj_func_test_gc1num_bump_fast_calls() == fast0);
  assert(lj_tg_local_total_acq(tg) < LJ_GC2_ACCT_FLUSH);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = L2TG(L);

  test_traced_behavior(L);
  test_accounting_fast_direct(L, g, tg);
  test_accounting_fallback(L, g, tg);
  test_traced_mark_active_fallback(L, g, tg);
  test_traced_alloc_black_inline(L, g, tg);

  lua_close(L);
  puts("t-jit-fnew-bump OK");
  return 0;
}
