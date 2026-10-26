/* Direct contract tests for rooted local-cell read/write helpers. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"

#ifndef LJ_FUNC_TEST_HELPERS
#error "t-func-rooted-cells requires LJ_FUNC_TEST_HELPERS"
#endif

static int lightud_token;
static int c_noop(lua_State *L) { (void)L; return 0; }

static void push_cdata(lua_State *L)
{
  assert(luaL_dostring(L, "return require('ffi').new('int[1]', 29)") == LUA_OK);
}

static TValue *stackoff(lua_State *L, ptrdiff_t off)
{
  return restorestack(L, off);
}

static void assert_clean(lua_State *L, uint32_t anchors, uint32_t readers,
			uint64_t scratch1, uint64_t scratch2, uint64_t scratch3)
{
  TGState *tg = L2TG(L);
  assert(lj_tg_root_anchor_top_acq(tg) == anchors);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  assert(tg->tmptv.u64 == scratch1 && tg->tmptv2.u64 == scratch2 &&
	 tg->tmptv3.u64 == scratch3);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static void make_pair(lua_State *L, ptrdiff_t *read_off,
		      ptrdiff_t *write_off)
{
  lua_getglobal(L, "make_rooted_cell_pair");
  lua_pushnumber(L, 1.25);
  lua_call(L, 1, 2);
  *read_off = savestack(L, L->top-2);
  *write_off = savestack(L, L->top-1);
}

static void call_reader(lua_State *L, ptrdiff_t read_off, uint64_t expected)
{
  lua_pushvalue(L, (int)(stackoff(L, read_off)-L->base)+1);
  lua_call(L, 0, 1);
  assert(tv_rawload_acq(L->top-1) == expected);
  lua_pop(L, 1);
}

static uint32_t push_cell_slot(lua_State *L, ptrdiff_t read_off)
{
  GCfuncL *fn = &funcV(stackoff(L, read_off))->l;
  GCobj *obj = func_uvptr_acq(fn, 0);
  TValue *slot;
  assert(obj != NULL && obj->gch.gct == (uint32_t)~LJ_TUPVAL);
  lua_pushnil(L);
  slot = L->top-1;
  setgcV(L, slot, obj, LJ_TUPVAL);
  lj_state_stack_pubtv(L, L, slot);
  return (uint32_t)(slot-L->base);
}

static void check_cell_values(lua_State *L, uint32_t slot,
			      ptrdiff_t read_off)
{
  TValue *out;
  uint64_t expected;
#define STORE_VALUE(push_) do { \
    uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L)); \
    uint32_t readers = gc2_smr_readers_acq(G(L)); \
    uint64_t scratch1 = L2TG(L)->tmptv.u64; \
    uint64_t scratch2 = L2TG(L)->tmptv2.u64; \
    uint64_t scratch3 = L2TG(L)->tmptv3.u64; \
    push_; expected = tv_rawload_acq(L->top-1); \
    errno = 733; \
    assert(lj_func_storecell_rooted_try(L, L->base, slot, L->top-1)); \
    assert(errno == 733); \
    lua_pop(L, 1); \
    lua_gc(L, LUA_GCCOLLECT, 0); \
    lua_pushliteral(L, "cell-output"); out = L->top-1; \
    errno = 811; \
    assert(lj_func_getcell_rooted_try(L, L->base, slot, out)); \
    assert(errno == 811 && tv_rawload_acq(out) == expected); \
    assert_clean(L, anchors, readers, scratch1, scratch2, scratch3); \
    lua_pop(L, 1); \
    call_reader(L, read_off, expected); \
  } while (0)
  STORE_VALUE(lua_pushnumber(L, 17.25));
  STORE_VALUE(lua_pushboolean(L, 0));
  STORE_VALUE(lua_pushboolean(L, 1));
  STORE_VALUE(lua_pushnil(L));
  STORE_VALUE(lua_pushliteral(L, "rooted-cell-string"));
  STORE_VALUE(lua_newtable(L));
  STORE_VALUE(lua_pushcfunction(L, c_noop));
  STORE_VALUE(lua_newthread(L));
  STORE_VALUE(lua_newuserdata(L, 32));
  STORE_VALUE(lua_pushlightuserdata(L, &lightud_token));
  STORE_VALUE(lua_pushnumber(L, -91.5));
  STORE_VALUE(lua_pushinteger(L, 73));
  STORE_VALUE(lua_pushnumber(L, -0.0));
  STORE_VALUE(push_cdata(L));
  STORE_VALUE(lua_pushvalue(L, (int)(stackoff(L, read_off)-L->base)+1));
#undef STORE_VALUE
}

static void check_rejections(lua_State *L, uint32_t slot,
			     uint32_t anchors,
			     uint32_t readers)
{
  TValue *out;
  uint64_t before, cellbefore;
  uint64_t scratch1 = L2TG(L)->tmptv.u64;
  uint64_t scratch2 = L2TG(L)->tmptv2.u64;
  uint64_t scratch3 = L2TG(L)->tmptv3.u64;

  cellbefore = tv_rawload_acq(L->base+slot);
  lua_pushliteral(L, "unchanged-output");
  out = L->top-1;
  before = tv_rawload_acq(out);
  errno = 971;
  assert(!lj_func_getcell_rooted_try(L, L->base, UINT32_MAX, out));
  assert(errno == 971 && tv_rawload_acq(out) == before);
  assert(!lj_func_getcell_rooted_try(L, L->base+1, slot, out));
  assert(tv_rawload_acq(out) == before);
  assert(!lj_func_getcell_rooted_try(NULL, L->base, slot, out));
  assert(tv_rawload_acq(out) == before);
  {
    LJStateOwner owner = lj_state_owner_word_acq(L);
    lj_state_owner_word_rel(L, 0);
    assert(!lj_func_getcell_rooted_try(L, L->base, slot, out));
    assert(tv_rawload_acq(out) == before);
    lj_state_owner_word_rel(L, owner);
  }
  assert(!lj_func_getcell_rooted_try(L, L->base, slot, L->base+slot));
  assert(tv_rawload_acq(L->base+slot) == cellbefore);
  assert_clean(L, anchors, readers, scratch1, scratch2, scratch3);
  lua_pop(L, 1);

  /* A local slot without a TUPVAL tag is refused before touching its value. */
  setnumV(L->base+slot, 83.5);
  lj_state_stack_pubtv(L, L, L->base+slot);
  lua_pushnumber(L, 91.25);
  before = tv_rawload_acq(L->top-1);
  errno = 1013;
  assert(!lj_func_storecell_rooted_try(L, L->base, slot, L->top-1));
  assert(errno == 1013 && tv_rawload_acq(L->top-1) == before);
  lua_pop(L, 1);
  tv_rawstore_rel(L->base+slot, cellbefore);
  lj_state_stack_pubtv(L, L, L->base+slot);

  /* Internal forwarding markers cannot become visible cell values. */
  lua_pushlightuserdata(L, NULL);
  setforwardV(L->top-1);
  before = tv_rawload_acq(L->top-1);
  errno = 1117;
  assert(!lj_func_storecell_rooted_try(L, L->base, slot, L->top-1));
  assert(errno == 1117 && tv_rawload_acq(L->top-1) == before);
  lua_pop(L, 1);
  assert_clean(L, anchors, readers, scratch1, scratch2, scratch3);
}

static void check_native_base(lua_State *L, uint32_t slot)
{
  TGState *tg = L2TG(L);
  TValue *old = lj_tg_load_jit_base(tg);
  TValue *nativebase;
  TValue *out;
  uint64_t expected, cellbits;
  /* Use an explicit native base different from stale interpreter L->base;
  ** preserve the same cell address by reducing the relative slot by one. */
  assert(slot > 0);
  lua_pushliteral(L, "native-base-out");
  out = L->top-1;
  expected = tv_rawload_acq(out);
  nativebase = L->base+1;
  lj_tg_store_jit_base(tg, nativebase);
  {
    lua_State *oldcur = lj_tg_load_cur_L(tg);
    lj_tg_store_cur_L(tg, NULL);
    assert(!lj_func_getcell_rooted_try(L, nativebase, slot-1, out));
    assert(tv_rawload_acq(out) == expected);
    lj_tg_store_cur_L(tg, oldcur);
  }
  assert(!lj_func_getcell_rooted_try(L, nativebase+1, slot-1, out));
  assert(tv_rawload_acq(out) == expected);
  cellbits = tv_rawload_acq(&gcV(L->base+slot)->uv.tv);
  assert(lj_func_getcell_rooted_try(L, nativebase, slot-1, out));
  expected = tv_rawload_acq(out);
  assert(expected == cellbits);
  lua_pop(L, 1);
  lj_tg_store_jit_base(tg, old);
}

static void check_open_cell_refusal(lua_State *L, uint32_t slot)
{
#ifdef LJ_FUNC_TEST_HELPERS
  GCobj *closed;
  GCupval *open;
  TValue *source, *out;
  ptrdiff_t sourceoff;
  uint64_t before;
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t readers = gc2_smr_readers_acq(G(L));
  uint64_t scratch1 = L2TG(L)->tmptv.u64;
  uint64_t scratch2 = L2TG(L)->tmptv2.u64;
  uint64_t scratch3 = L2TG(L)->tmptv3.u64;
  lua_pushnumber(L, 777.25);
  closed = gcV(L->base+slot);
  sourceoff = savestack(L, L->top-1);
  source = stackoff(L, sourceoff);
  open = lj_func_test_openuv(L, source);
  assert(open != NULL && mref_acq(open->v, TValue) == source);
  setgcV(L, L->base+slot, obj2gco(open), LJ_TUPVAL);
  lj_state_stack_pubtv(L, L, L->base+slot);
  lua_pushliteral(L, "open-cell-output");
  out = L->top-1;
  before = tv_rawload_acq(out);
  assert(!lj_func_getcell_rooted_try(L, L->base, slot, out));
  assert(tv_rawload_acq(out) == before);
  lua_pop(L, 1);
  lua_pushnumber(L, 888.5);
  before = tv_rawload_acq(L->base+slot);
  assert(!lj_func_storecell_rooted_try(L, L->base, slot, L->top-1));
  assert(tv_rawload_acq(L->base+slot) == before);
  lua_pop(L, 1);
  assert_clean(L, anchors, readers, scratch1, scratch2, scratch3);
  lj_func_closeuv(L, source);
  setgcV(L, L->base+slot, closed, LJ_TUPVAL);
  lj_state_stack_pubtv(L, L, L->base+slot);
  lua_pop(L, 1);
#else
  UNUSED(L); UNUSED(slot);
#endif
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  uint32_t slotno, anchors, readers;
  ptrdiff_t read_off, write_off;
  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushlightuserdata(L, &lightud_token);
  lua_setglobal(L, "rooted_cell_lightud");
  assert(luaL_dostring(L,
    "function make_rooted_cell_pair(initial)\n"
    " local value=initial\n"
    " return function() return value end, function(v) value=v; return value end\n"
    "end\n") == LUA_OK);
  make_pair(L, &read_off, &write_off);
  slotno = push_cell_slot(L, read_off);
  anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  readers = gc2_smr_readers_acq(G(L));
  check_cell_values(L, slotno, read_off);
  check_rejections(L, slotno, anchors, readers);
  check_open_cell_refusal(L, slotno);
  check_native_base(L, slotno);
  setnilV(L->base+slotno);
  lj_state_stack_pubtv(L, L, L->base+slotno);
  lua_pop(L, 3);
  if (argc >= 2 && luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "rooted cell Lua fixture failed: %s\n", lua_tostring(L, -1));
    abort();
  }
  lua_close(L);
  puts("t-func-rooted-cells OK");
  return 0;
}
