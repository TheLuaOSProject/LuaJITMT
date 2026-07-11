/*
** Focused regression test for one-upvalue FNEW allocation/publication paths.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

/* Built by the M6 harness with LJ_FUNC_TEST_HELPERS enabled. */

static void run_script(lua_State *L, const char *code, const char *label)
{
  UNUSED(label);
  ljt_lua_dostring(L, code);
}

static int is_rex(uint8_t byte)
{
  return (byte & 0xf0u) == 0x40u;
}

static size_t find_bitmap_op(const uint8_t *mc, size_t len, size_t start,
			     int locked, uint8_t opcode, uint32_t disp)
{
  size_t i;
  for (i = start; i < len; i++) {
    size_t j = i;
    uint32_t got;
    if (locked) {
      if (mc[j++] != 0xf0u)
	continue;
    } else {
      if (mc[j] == 0xf0u || (i != 0 && mc[i-1] == 0xf0u) ||
	  (i > 1 && is_rex(mc[i-1]) && mc[i-2] == 0xf0u))
	continue;
    }
    if (j < len && is_rex(mc[j]))
      j++;
    if (j + 7u > len || mc[j] != 0x0fu || mc[j+1] != opcode)
      continue;
    got = (uint32_t)mc[j+3] | ((uint32_t)mc[j+4] << 8) |
	  ((uint32_t)mc[j+5] << 16) | ((uint32_t)mc[j+6] << 24);
    if (got == disp)
      return i;
  }
  return (size_t)-1;
}

static size_t find_gct_store(const uint8_t *mc, size_t len, uint8_t gct)
{
  size_t i;
  for (i = 0; i + 4u < len; i++) {
    uint8_t modrm, rm;
    size_t disp;
    if (mc[i] != 0xc6u)
      continue;
    modrm = mc[i+1];
    if ((modrm & 0xf8u) != 0x40u)  /* MOV byte [base+disp8], imm8. */
      continue;
    rm = modrm & 7u;
    disp = i + 2u + (rm == 4u);  /* Skip SIB for rsp/r12 bases. */
    if (disp + 1u < len && mc[disp] == offsetof(GCupval, gct) &&
	mc[disp+1] == gct)
      return i;
  }
  return (size_t)-1;
}

static size_t find_i32_store(const uint8_t *mc, size_t len, size_t start,
			     uint32_t field, uint32_t value)
{
  size_t i;
  for (i = start; i + 10u <= len; i++) {
    uint8_t modrm, rm;
    size_t disp;
    uint32_t got_field, got_value;
    if (mc[i] != 0xc7u)
      continue;
    modrm = mc[i+1];
    if ((modrm & 0xf8u) != 0x80u)  /* MOV dword [base+disp32], imm32. */
      continue;
    rm = modrm & 7u;
    disp = i + 2u + (rm == 4u);
    if (disp + 8u > len)
      continue;
    got_field = (uint32_t)mc[disp] | ((uint32_t)mc[disp+1] << 8) |
		((uint32_t)mc[disp+2] << 16) | ((uint32_t)mc[disp+3] << 24);
    got_value = (uint32_t)mc[disp+4] | ((uint32_t)mc[disp+5] << 8) |
		((uint32_t)mc[disp+6] << 16) | ((uint32_t)mc[disp+7] << 24);
    if (got_field == field && got_value == value)
      return i;
  }
  return (size_t)-1;
}

static void assert_traced_fnew_publication_order(lua_State *L)
{
  int traceno;
  for (traceno = 1; traceno <= 256; traceno++) {
    size_t len, fn_header, uv_header, mark_set, mark_clear, first_mark, block;
    size_t pending_hint;
    const uint8_t *mc;
    lua_getglobal(L, "require");
    lua_pushliteral(L, "jit.util");
    ljt_lua_pcall(L, 1, 1, "load jit.util for FNEW mcode");
    lua_getfield(L, -1, "tracemc");
    lua_pushinteger(L, traceno);
    ljt_lua_pcall(L, 1, 1, "fetch FNEW trace mcode");
    lua_remove(L, -2);  /* jit.util table. */
    mc = (const uint8_t *)lua_tolstring(L, -1, &len);
    if (mc == NULL) {
      lua_pop(L, 1);
      continue;
    }
    mark_set = find_bitmap_op(mc, len, 0, 1, 0xabu,
			      (uint32_t)offsetof(GCArena, mark));
    block = find_bitmap_op(mc, len, 0, 0, 0xabu,
			   (uint32_t)offsetof(GCArena, block));
    if (mark_set == (size_t)-1 || block == (size_t)-1) {
      lua_pop(L, 1);
      continue;
    }
    fn_header = find_gct_store(mc, len, (uint8_t)~LJ_TFUNC);
    uv_header = find_gct_store(mc, len, (uint8_t)~LJ_TUPVAL);
    mark_clear = find_bitmap_op(mc, len, 0, 1, 0xb3u,
				(uint32_t)offsetof(GCArena, mark));
    pending_hint = find_i32_store(mc, len, block,
				  (uint32_t)offsetof(global_State,
						    gcroot_pending_hint), 1u);
    assert(fn_header != (size_t)-1 && uv_header != (size_t)-1);
    assert(mark_clear != (size_t)-1);
    assert(pending_hint != (size_t)-1);
    first_mark = mark_set < mark_clear ? mark_set : mark_clear;
    /* x64 emits backwards: this guards the order in executable mcode, not the
    ** visually misleading order of emit_* calls in lj_asm_x86.h. */
    assert(fn_header < first_mark);
    assert(uv_header < first_mark);
    assert(first_mark < block);
    assert(block < pending_hint);
    lua_pop(L, 1);
    return;
  }
  assert(0 && "missing traced inline FNEW bitmap publication");
}

static void reset_gc1num_bump_counters(void)
{
  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
}

static uint32_t gc1num_bump_helper_calls(void)
{
  return lj_func_test_gc1num_bump_fast_calls() +
	 lj_func_test_gc1num_bump_fallback_calls();
}

static void prime_traversable_bump_window(TGState *tg)
{
  TGAlloc *alloc = &tg->alloc;
  GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
  assert(a != NULL);
  lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
  lj_arena_next_rel(a, alloc->owned[LJ_ARENAK_TRAVERSABLE]);
  alloc->owned[LJ_ARENAK_TRAVERSABLE] = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].a = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].cell = LJ_AFIRST_CELL;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].end = LJ_ARENA_CELLS;
  assert(lj_arena_alloc_register_existing(alloc));
}

static void suppress_new_trace_recording(lua_State *L, global_State *g)
{
  run_script(L, "jit.opt.start('hotloop=1000000', 'hotexit=1000000')\n",
	     "suppress fresh trace recording");
  lj_dispatch_init_hotcount(g);
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

static void test_traced_immutable_numeric_inline(lua_State *L, global_State *g)
{
  TGState *tg = L2TG(L);
  uint32_t helper0, helper1;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "__fnew_hint_t = {}\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_hint_run(offset)\n"
    "  local t = assert(__fnew_hint_t)\n"
    "  for i = 1, 100 do\n"
    "    local x = i + offset\n"
    "    t[i] = function()\n"
    "      return x\n"
    "    end\n"
    "  end\n"
    "  return t[1](), t[100]()\n"
    "end\n"
    "local a, b = __fnew_hint_run(0.5)\n"
    "assert(a == 1.5 and b == 100.5)\n"
    "assert(util.traceinfo(1), 'immutable numeric FNEW loop did not trace')\n"
    "assert(debug.upvalueid(__fnew_hint_t[1], 1) ~= "
    "debug.upvalueid(__fnew_hint_t[2], 1))\n";

  run_script(L, setup, "immutable numeric FNEW traced inline setup");
  run_script(L, warm, "immutable numeric FNEW traced inline warmup");
  assert_traced_fnew_publication_order(L);
  (void)lj_gc_flush_root_pending(g);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  lj_gcroot_pending_hint_rel(g, 0);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lua_getglobal(L, "__fnew_hint_run");
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, 1000.5);
  ljt_lua_pcall(L, 1, 2, "immutable numeric FNEW traced inline rerun");
  assert(lua_tonumber(L, -2) == 1001.5);
  assert(lua_tonumber(L, -1) == 1100.5);
  lua_pop(L, 2);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) > 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_hint_t");
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_hint_run");
}

static void test_traced_active_black_inline(lua_State *L, global_State *g,
					    TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  GCobj *pending0;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_active_run(n, offset)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i + offset\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "assert(__fnew_active_run(100, 0) == 5150)\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n";

  run_script(L, setup, "numeric FNEW traced active-black inline setup");
  run_script(L, warm, "numeric FNEW traced active-black inline warmup");
  suppress_new_trace_recording(L, g);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  prime_traversable_bump_window(tg);

  lua_getglobal(L, "__fnew_active_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 100);
  lua_pushnumber(L, 1000);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);
  ljt_lua_pcall(L, 2, 1, "numeric FNEW traced active-black inline rerun");
  assert(lua_tonumber(L, -1) == 105150);
  lua_pop(L, 1);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  assert(lj_tg_gcroot_pending_acq(tg) != pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) > 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_active_run");
}

static void test_traced_mark_active_white_fallback(lua_State *L,
						   global_State *g,
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

  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 0);

  run_script(L, code, "numeric FNEW traced active-white fallback");

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 > helper0);
}

static void test_traced_alloc_black_inline(lua_State *L, global_State *g,
					   TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_allocblack_run(n, offset)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i + offset\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "assert(__fnew_allocblack_run(100, 0) == 5150)\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n";

  run_script(L, setup, "numeric FNEW traced alloc-black inline setup");
  run_script(L, warm, "numeric FNEW traced alloc-black inline warmup");
  suppress_new_trace_recording(L, g);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  prime_traversable_bump_window(tg);
  lj_tg_mark_active_rel(tg, 0);
  lj_tg_alloc_black_rel(tg, 1);

  lua_getglobal(L, "__fnew_allocblack_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 100);
  lua_pushnumber(L, 1000);
  ljt_lua_pcall(L, 2, 1, "numeric FNEW traced alloc-black inline rerun");
  assert(lua_tonumber(L, -1) == 105150);
  lua_pop(L, 1);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_allocblack_run");
}

static void test_traced_post_sweep_bump_refill(lua_State *L)
{
  uint32_t fallback0, fallback1;
  uint32_t fast0, fast1;
  const char *setup =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function __fnew_refill_run(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local n = 20000\n"
    "assert(__fnew_refill_run(n) == n * (n + 3) / 2)\n"
    "assert(util.traceinfo(1), 'numeric FNEW refill loop did not trace')\n"
    "collectgarbage('collect')\n";
  const char *rerun =
    "local n = 20000\n"
    "assert(__fnew_refill_run(n) == n * (n + 3) / 2)\n";

  run_script(L, setup, "numeric FNEW post-sweep refill setup");

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  run_script(L, rerun, "numeric FNEW post-sweep refill rerun");

  fast1 = lj_func_test_gc1num_bump_fast_calls();
  fallback1 = lj_func_test_gc1num_bump_fallback_calls();
  assert(fallback1 == fallback0);
  assert(fast1 > fast0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_refill_run");
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
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  uv = func_uv_acq(&fn->l, 0);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tvisnumber(&uv->tv));
  assert((int32_t)numberVnum(&uv->tv) == value);
  assert(tvisgcv(slot) && gcV(slot) == obj2gco(uv));
}

static void assert_nil_closed_cell(GCupval *uv)
{
  assert(uv != NULL);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tvisnil(&uv->tv));
  assert(!lj_uv_immutable(uv));
}

static void quiet_gc_for_bump(global_State *g, TGState *tg)
{
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
}

static void test_uvcell_bump_direct(lua_State *L, global_State *g, TGState *tg)
{
  uint32_t bump0, bump1, bump2;
  TValue slots[8];
  GCupval *uv;

  quiet_gc_for_bump(g, tg);

  lj_func_test_reset_uvcell_bump_calls();
  bump0 = lj_func_test_uvcell_bump_calls();
  uv = lj_func_newuvcell(L);
  assert_nil_closed_cell(uv);
  bump1 = lj_func_test_uvcell_bump_calls();
  assert(bump1 > bump0);
  assert(lj_tg_local_total_acq(tg) > 0);

  setnilV(&slots[3]);
  uv = lj_func_newuvcell_forjit(L, slots, 3);
  assert_nil_closed_cell(uv);
  assert(tvisgcv(&slots[3]) && gcV(&slots[3]) == obj2gco(uv));
  bump2 = lj_func_test_uvcell_bump_calls();
  assert(bump2 > bump1);
}

static void assert_gc1num_bump_blocked(lua_State *L, TValue *slots,
				       GCproto *child, GCfuncL *parent,
				       int32_t slotno, int32_t value)
{
  uint32_t fast0 = lj_func_test_gc1num_bump_fast_calls();
  uint32_t fallback0 = lj_func_test_gc1num_bump_fallback_calls();
  GCfunc *fn;

  setnumV(&slots[slotno], value);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, parent, slotno, value);
  assert_one_upvalue_result(fn, &slots[slotno], value);
  assert(lj_func_test_gc1num_bump_fast_calls() == fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() > fallback0);
}

static void load_no_upvalue_fixture(lua_State *L, GCfunc **parentp,
				    GCproto **childp)
{
  GCfunc *parent;
  GCproto *child;

  assert(luaL_loadstring(L,
    "return function()\n"
    "  return function()\n"
    "    return 42\n"
    "  end\n"
    "end\n") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  parent = top_lfunc(L);
  child = first_child_proto(funcproto(parent));
  assert(child->sizeuv == 0);
  *parentp = parent;
  *childp = child;
}

static void test_bump_allocator_gate_direct(lua_State *L, global_State *g,
					    TGState *tg)
{
  uint32_t old_workers = gc2_n_workers_acq(g);
  uint32_t old_allocf_arena = la_load32_acq(&g->allocf_arena);
  uint32_t fast0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  int32_t slotno;

  assert(mt_entering_acq(g) == 0);
  assert(old_workers == 0);
  assert(old_allocf_arena != 0);

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();

  assert(mt_entering_add_rlx(g, 1) == 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 11);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  gc2_n_workers_rel(g, 1);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 22);
  gc2_n_workers_rel(g, old_workers);

  la_store32_rel(&g->allocf_arena, 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 33);
  la_store32_rel(&g->allocf_arena, old_allocf_arena);
  lua_pop(L, 1);

  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_uvcell_bump_calls();
  fast0 = lj_func_test_uvcell_bump_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  uv = lj_func_newuvcell(L);
  assert_nil_closed_cell(uv);
  assert(lj_func_test_uvcell_bump_calls() == fast0);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  load_no_upvalue_fixture(L, &parent, &child);
  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_gc0_bump_trace_calls();
  fast0 = lj_func_test_gc0_bump_trace_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  fn = lj_func_newL_gc_forjit(L, NULL, child, &parent->l);
  assert(isluafunc(fn));
  assert(lj_funcL_nupvalues(&fn->l) == 0);
  assert(lj_func_test_gc0_bump_trace_calls() == fast0);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);
  lua_pop(L, 1);
}

static void test_interpreter_numeric_fast_path(lua_State *L)
{
  uint32_t interp0;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 40 do\n"
    "  local x = i + 0.25\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(t[1]() == 2.25)\n"
    "assert(t[2]() == 3.25)\n"
    "assert(t[40]() == 41.25)\n"
    "assert(t[1]() == 3.25)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "local name = debug.setupvalue(t[1], 1, 50.5)\n"
    "assert(name == 'x', name)\n"
    "assert(t[1]() == 51.5)\n"
    "assert(t[2]() == 4.25)\n"
    "local r = {}\n"
    "for i = 1, 40 do\n"
    "  local x = i + 0.5\n"
    "  r[i] = function() return x end\n"
    "end\n"
    "assert(r[1]() == 1.5)\n"
    "assert(r[40]() == 40.5)\n"
    "local a, b\n"
    "do\n"
    "  local x = 10.25\n"
    "  a = function() x = x + 1; return x end\n"
    "  b = function() x = x + 1; return x end\n"
    "end\n"
    "assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))\n"
    "assert(a() == 11.25)\n"
    "assert(b() == 12.25)\n"
    "jit.on()\n";

  lj_func_test_reset_gc1num_bump_interp_calls();
  interp0 = lj_func_test_gc1num_bump_interp_calls();
  run_script(L, code, "interpreter numeric FNEW fast path");
  assert(lj_func_test_gc1num_bump_interp_calls() > interp0);
}

static void test_interpreter_generic_oneuv_chain(lua_State *L)
{
  uint32_t chain0, chain1;
  const char *code =
    "jit.off()\n"
    "local s = {}\n"
    "for i = 1, 40 do\n"
    "  local x = 'v' .. i\n"
    "  s[i] = function()\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(s[1]() == 'v1')\n"
    "assert(s[40]() == 'v40')\n"
    "assert(debug.upvalueid(s[1], 1) ~= debug.upvalueid(s[2], 1))\n"
    "local name = debug.setupvalue(s[1], 1, 'changed')\n"
    "assert(name == 'x', name)\n"
    "assert(s[1]() == 'changed')\n"
    "assert(s[2]() == 'v2')\n"
    "local a, b\n"
    "do\n"
    "  local x = { n = 10 }\n"
    "  a = function() x = { n = x.n + 1 }; return x.n end\n"
    "  b = function() x = { n = x.n + 1 }; return x.n end\n"
    "end\n"
    "assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))\n"
    "assert(a() == 11)\n"
    "assert(b() == 12)\n"
    "jit.on()\n";

  lj_func_test_reset_gc1uv_chain_calls();
  chain0 = lj_func_test_gc1uv_chain_calls();
  run_script(L, code, "interpreter generic one-upvalue FNEW chain");
  chain1 = lj_func_test_gc1uv_chain_calls();
  assert(chain1 > chain0);
}

static void test_interpreter_multiuv_afterfn(lua_State *L)
{
  uint32_t after0, after1;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 40 do\n"
    "  local a = 'a' .. i\n"
    "  local b = { n = i }\n"
    "  t[i] = function()\n"
    "    a = a .. '!'\n"
    "    b = { n = b.n + 1 }\n"
    "    return a, b.n\n"
    "  end\n"
    "end\n"
    "local a1, b1 = t[1](); assert(a1 == 'a1!' and b1 == 2)\n"
    "local a2, b2 = t[2](); assert(a2 == 'a2!' and b2 == 3)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "assert(debug.upvalueid(t[1], 2) ~= debug.upvalueid(t[2], 2))\n"
    "local name = debug.setupvalue(t[1], 1, 'z')\n"
    "assert(name == 'a', name)\n"
    "a1, b1 = t[1](); assert(a1 == 'z!' and b1 == 3)\n"
    "a2, b2 = t[2](); assert(a2 == 'a2!!' and b2 == 4)\n"
    "jit.on()\n";

  lj_func_test_reset_uv_afterfn_calls();
  after0 = lj_func_test_uv_afterfn_calls();
  run_script(L, code, "interpreter multi-upvalue FNEW after-function links");
  after1 = lj_func_test_uv_afterfn_calls();
  assert(after1 > after0);
}

static void test_interpreter_no_upvalue_fast_path(lua_State *L)
{
  uint32_t fast0, fast1;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 80 do\n"
    "  t[i] = function() return 42 end\n"
    "end\n"
    "assert(t[1]() == 42)\n"
    "assert(t[80]() == 42)\n"
    "assert(t[1] ~= t[2])\n"
    "assert(debug.getupvalue(t[1], 1) == nil)\n"
    "jit.on()\n";

  lj_func_test_reset_gc0_bump_interp_calls();
  fast0 = lj_func_test_gc0_bump_interp_calls();
  run_script(L, code, "interpreter no-upvalue FNEW fast path");
  fast1 = lj_func_test_gc0_bump_interp_calls();
  assert(fast1 > fast0);
}

static void test_traced_no_upvalue_fast_path(lua_State *L)
{
  uint32_t fast0, fast1;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "local t = {}\n"
    "for i = 1, 120 do\n"
    "  t[i] = function() return 42 end\n"
    "end\n"
    "assert(util.traceinfo(1), 'no-upvalue FNEW loop did not trace')\n"
    "assert(t[1]() == 42)\n"
    "assert(t[120]() == 42)\n"
    "assert(t[1] ~= t[2])\n"
    "assert(debug.getupvalue(t[1], 1) == nil)\n";

  lj_func_test_reset_gc0_bump_trace_calls();
  fast0 = lj_func_test_gc0_bump_trace_calls();
  run_script(L, code, "traced no-upvalue FNEW fast path");
  fast1 = lj_func_test_gc0_bump_trace_calls();
  assert(fast1 > fast0);
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

static void test_active_black_direct_publishes_exact(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t fast0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  GCobj *pending0;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  setnumV(&slots[slotno], 321);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 321);
  assert_one_upvalue_result(fn, &slots[slotno], 321);
  uv = func_uv_acq(&fn->l, 0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_funcL_nupvalues(&fn->l) == 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fn));
  assert(lj_obj_gcw_acq(obj2gco(fn)) == obj2gco(uv));
  assert(lj_obj_gcw_acq(obj2gco(uv)) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);
  lua_pop(L, 1);
}

static void test_active_black_direct_keeps_exact_cells(
  lua_State *L, global_State *g, TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  GCobj *pending0;
  GCArena *a;
  uint32_t fncell, uvcell;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  lua_gc(L, LUA_GCCOLLECT, 0);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);

  quiet_gc_for_bump(g, tg);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  setnumV(&slots[slotno], 654);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 654);
  assert_one_upvalue_result(fn, &slots[slotno], 654);
  uv = func_uv_acq(&fn->l, 0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_funcL_nupvalues(&fn->l) == 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fn));
  assert(lj_obj_gcw_acq(obj2gco(fn)) == obj2gco(uv));
  assert(lj_obj_gcw_acq(obj2gco(uv)) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  a = lj_arena_of(fn);
  assert(a == lj_arena_of(uv));
  fncell = lj_arena_cellof(fn);
  uvcell = lj_arena_cellof(uv);
  assert(lj_arena_bm_get(a->block, fncell));
  assert(lj_arena_bm_get(a->block, uvcell));
  assert(lj_arena_bm_get(a->mark, fncell));
  assert(lj_arena_bm_get(a->mark, uvcell));
  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);
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
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
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

  test_uvcell_bump_direct(L, g, tg);
  test_bump_allocator_gate_direct(L, g, tg);
  test_interpreter_numeric_fast_path(L);
  test_accounting_fast_direct(L, g, tg);
  test_active_black_direct_publishes_exact(L, g, tg);
  test_active_black_direct_keeps_exact_cells(L, g, tg);
  test_accounting_fallback(L, g, tg);
  test_interpreter_generic_oneuv_chain(L);
  test_interpreter_multiuv_afterfn(L);
  test_interpreter_no_upvalue_fast_path(L);
  test_traced_active_black_inline(L, g, tg);
  test_traced_immutable_numeric_inline(L, g);
  if (getenv("LJ_TEST_TRACED_FNEW") != NULL) {
    test_traced_behavior(L);
    test_traced_mark_active_white_fallback(L, g, tg);
    test_traced_alloc_black_inline(L, g, tg);
    test_traced_post_sweep_bump_refill(L);
    test_traced_no_upvalue_fast_path(L);
  }

  lua_close(L);
  puts("t-jit-fnew-bump OK");
  return 0;
}
