/* Direct contract tests for rooted closed-upvalue read/write helpers. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_func.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"

#ifndef LJ_FUNC_TEST_HELPERS
#error "t-func-rooted-upvalues requires LJ_FUNC_TEST_HELPERS"
#endif

static int lightud_token;
static int c_noop(lua_State *L) { (void)L; return 0; }

static GCproto *check_lua_proto_arg(lua_State *L)
{
  GCfunc *fn;
  luaL_checktype(L, 1, LUA_TFUNCTION);
  fn = funcV(L->base);
  if (!isluafunc(fn)) {
    luaL_error(L, "expected Lua closure");
    return NULL;
  }
  return funcproto(fn);
}

static MSize find_usetn_constants(GCproto *pt, int one_based_index)
{
  const BCIns *pc = proto_bc(pt);
  MSize i, count = 0;
  BCReg uv = (BCReg)(one_based_index - 1);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = pc[i];
    if (bc_op(ins) == BC_USETN && bc_a(ins) == uv &&
        (MSize)bc_d(ins) < pt->sizekn) {
      count++;
    }
  }
  return count;
}

/* Compare an upvalue with the exact proto TValue used by its BC_USETN. */
static int rooted_uv_literal_bits(lua_State *L)
{
  int index = (int)luaL_checkinteger(L, 2);
  GCproto *pt;
  MSize count;
  const char *name;
  uint64_t actual;
  if (index < 1)
    return luaL_error(L, "upvalue index must be positive");
  pt = check_lua_proto_arg(L);
  count = find_usetn_constants(pt, index);
  if (count == 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  name = lua_getupvalue(L, 1, index);
  if (name == NULL) {
    lua_pushboolean(L, 0);
    return 1;
  }
  actual = tv_rawload_acq(L->top-1);
  lua_pop(L, 1);
  for (MSize i = 0; i < pt->sizebc; i++) {
    BCIns ins = proto_bc(pt)[i];
    if (bc_op(ins) == BC_USETN && bc_a(ins) == (BCReg)(index-1) &&
        (MSize)bc_d(ins) < pt->sizekn &&
        actual == tv_rawload_acq(proto_knumtv(pt, bc_d(ins)))) {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}

/* Test-only mutation for a freshly loaded, not-yet-executed test function.
** Convert its USETN constants to double TValues before any trace is recorded. */
static int rooted_uv_force_double_literal(lua_State *L)
{
  int index = (int)luaL_checkinteger(L, 2);
  lua_Number value = luaL_checknumber(L, 3);
  GCproto *pt;
  MSize count, i;
  if (index < 1)
    return luaL_error(L, "upvalue index must be positive");
  pt = check_lua_proto_arg(L);
  count = find_usetn_constants(pt, index);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = proto_bc(pt)[i];
    if (bc_op(ins) == BC_USETN && bc_a(ins) == (BCReg)(index-1) &&
        (MSize)bc_d(ins) < pt->sizekn) {
      TValue *constant = proto_knumtv(pt, bc_d(ins));
      setnumV(constant, value);
    }
  }
  lua_pushboolean(L, count != 0);
  return 1;
}

static void push_cdata(lua_State *L)
{
  assert(luaL_dostring(L, "return require('ffi').new('int[1]', 29)") == LUA_OK);
}

static TValue *stackoff(lua_State *L, ptrdiff_t off)
{
  return restorestack(L, off);
}

static void make_pair(lua_State *L, ptrdiff_t *read_off, ptrdiff_t *write_off)
{
  lua_getglobal(L, "make_rooted_uv_pair");
  lua_pushnumber(L, 1.25);
  lua_call(L, 1, 2);
  *read_off = savestack(L, L->top-2);
  *write_off = savestack(L, L->top-1);
}

static void assert_clean(lua_State *L, uint32_t anchors, uint32_t readers)
{
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static void get_equals(lua_State *L, ptrdiff_t fn_off, uint32_t index,
                       uint64_t expected)
{
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t readers = gc2_smr_readers_acq(G(L));
  lua_pushliteral(L, "out-sentinel");
  errno = 719;
  assert(lj_func_getuv_rooted_try(L, stackoff(L, fn_off), index, L->top-1));
  assert(errno == 719 && tv_rawload_acq(L->top-1) == expected);
  assert_clean(L, anchors, readers);
  lua_pop(L, 1);
}

static void store_from_top(lua_State *L, ptrdiff_t fn_off, uint32_t index)
{
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t readers = gc2_smr_readers_acq(G(L));
  errno = 811;
  assert(lj_func_storeuv_rooted_try(L, stackoff(L, fn_off), index, L->top-1));
  assert(errno == 811);
  assert_clean(L, anchors, readers);
}

static void check_values(lua_State *L, ptrdiff_t read_off,
                         ptrdiff_t write_off)
{
  uint64_t expected;
#define STORE_VALUE(push_) do { \
    push_; expected = tv_rawload_acq(L->top-1); \
    store_from_top(L, write_off, 0); lua_pop(L, 1); \
    lua_gc(L, LUA_GCCOLLECT, 0); \
    get_equals(L, read_off, 0, expected); \
  } while (0)
  STORE_VALUE(lua_pushnumber(L, 17.25));
  STORE_VALUE(lua_pushboolean(L, 0));
  STORE_VALUE(lua_pushboolean(L, 1));
  STORE_VALUE(lua_pushnil(L));
  STORE_VALUE(lua_pushliteral(L, "rooted-upvalue-string"));
  STORE_VALUE(lua_newtable(L));
  STORE_VALUE(lua_pushcfunction(L, c_noop));
  STORE_VALUE(lua_newthread(L));
  STORE_VALUE(lua_newuserdata(L, 32));
  STORE_VALUE(lua_pushlightuserdata(L, &lightud_token));
  STORE_VALUE(push_cdata(L));
  STORE_VALUE(lua_pushnumber(L, -91.5));
#undef STORE_VALUE
}

static void check_refusals(lua_State *L, ptrdiff_t read_off,
                           ptrdiff_t write_off)
{
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t readers = gc2_smr_readers_acq(G(L));
  TValue *out;
  uint64_t before, expected;

  lua_pushliteral(L, "preserve-output-on-refusal");
  out = L->top-1;
  before = tv_rawload_acq(out);
  assert(!lj_func_getuv_rooted_try(L, stackoff(L, read_off), 99, out));
  assert(tv_rawload_acq(out) == before);
  assert_clean(L, anchors, readers);
  lua_pop(L, 1);

  lua_pushnumber(L, 12.75);
  expected = tv_rawload_acq(L->top-1);
  store_from_top(L, write_off, 0);
  lua_pop(L, 1);
  lua_pushnumber(L, 99.5);
  errno = 887;
  assert(!lj_func_storeuv_rooted_try(L, stackoff(L, write_off), 99,
                                      L->top-1));
  assert(errno == 887);
  lua_pop(L, 1);
  get_equals(L, read_off, 0, expected);

  lua_pushcfunction(L, c_noop);
  {
    ptrdiff_t cfn = savestack(L, L->top-1);
    lua_pushliteral(L, "keep-me");
    out = L->top-1;
    before = tv_rawload_acq(out);
    assert(!lj_func_getuv_rooted_try(L, stackoff(L, cfn), 0, out));
    assert(tv_rawload_acq(out) == before);
    lua_pop(L, 2);
    lua_pushcfunction(L, c_noop);
    cfn = savestack(L, L->top-1);
    lua_pushnumber(L, 987.25);
    errno = 913;
    assert(!lj_func_storeuv_rooted_try(L, stackoff(L, cfn), 0,
                                        L->top-1));
    assert(errno == 913);
    lua_pop(L, 2);
  }

  /* Reserved internal TValue markers are not storable Lua values. */
  lua_pushlightuserdata(L, NULL);
  setforwardV(L->top-1);
  before = tv_rawload_acq(L->top-1);
  errno = 1013;
  assert(!lj_func_storeuv_rooted_try(L, stackoff(L, write_off), 0,
                                      L->top-1));
  assert(errno == 1013);
  assert(tv_rawload_acq(L->top-1) == before);
  assert_clean(L, anchors, readers);
  lua_pop(L, 1);
  get_equals(L, read_off, 0, expected);
}

static void check_join(lua_State *L, ptrdiff_t first_read,
                       ptrdiff_t first_write)
{
  ptrdiff_t second_read, second_write;
  uint64_t expected;
  make_pair(L, &second_read, &second_write);
  /* The second pair follows the first on the Lua stack; join by absolute Lua
  ** indexes while retaining the byte offsets used by the direct helpers. */
  {
    int first_index = (int)(restorestack(L, first_read) - L->base) + 1;
    int second_index = (int)(restorestack(L, second_read) - L->base) + 1;
    lua_upvaluejoin(L, first_index, 1, second_index, 1);
  }
  lua_pushliteral(L, "joined-through-second-cell");
  expected = tv_rawload_acq(L->top-1);
  store_from_top(L, second_write, 0);
  lua_pop(L, 1);
  get_equals(L, first_read, 0, expected);
  lua_pop(L, 2);
  (void)first_write;
}

static void check_root_aliases(lua_State *L)
{
  ptrdiff_t read_off, write_off;
  uint64_t fnbits;
  TValue *fnroot;
  make_pair(L, &read_off, &write_off);
  fnroot = stackoff(L, read_off);
  fnbits = tv_rawload_acq(fnroot);
  /* A TValue input may also be the store source when the cell stores its own
  ** closure. Both snapshots must be taken before the cell publication. */
  assert(lj_func_storeuv_rooted_try(L, fnroot, 0, fnroot));
  get_equals(L, write_off, 0, fnbits);

  /* Output aliasing the function root is safe after the function/cell snapshot
  ** has been acquired; the output then intentionally replaces that stack slot. */
  make_pair(L, &read_off, &write_off);
  fnroot = stackoff(L, read_off);
  assert(lj_func_getuv_rooted_try(L, fnroot, 0, fnroot));
  assert(numberVnum(fnroot) == 1.25);
  lua_pop(L, 4);
}

static void check_open_refusal(lua_State *L, ptrdiff_t fn_off)
{
#ifdef LJ_FUNC_TEST_HELPERS
  GCfuncL *fn = &funcV(stackoff(L, fn_off))->l;
  GCobj *old = func_uvptr_acq(fn, 0);
  GCupval *uv;
  TValue *slot, *out;
  ptrdiff_t slot_off;
  uint64_t before;
  lua_pushnumber(L, 777.25);
  slot_off = savestack(L, L->top-1);
  slot = stackoff(L, slot_off);
  uv = lj_func_test_openuv(L, slot);
  assert(uv != NULL && mref_acq(uv->v, TValue) == slot);
  setgcrefrel(fn->uvptr[0], obj2gco(uv));
  lua_pushliteral(L, "open-output");
  out = L->top-1;
  before = tv_rawload_acq(out);
  assert(!lj_func_getuv_rooted_try(L, stackoff(L, fn_off), 0, out));
  assert(tv_rawload_acq(out) == before);
  lua_pop(L, 1);
  lua_pushnumber(L, 888.5);
  slot = stackoff(L, slot_off);
  before = tv_rawload_acq(slot);
  errno = 1117;
  assert(!lj_func_storeuv_rooted_try(L, stackoff(L, fn_off), 0,
                                      L->top-1));
  assert(errno == 1117);
  assert(tv_rawload_acq(slot) == before);
  lua_pop(L, 1);
  setgcrefrel(fn->uvptr[0], old);
  slot = stackoff(L, slot_off);
  lj_func_closeuv(L, slot);
  lua_pop(L, 1);
#else
  UNUSED(L); UNUSED(fn_off);
#endif
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  ptrdiff_t read_off, write_off;
  uint32_t anchors, readers;
  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushlightuserdata(L, &lightud_token);
  lua_setglobal(L, "rooted_uv_lightud");
  lua_pushcfunction(L, rooted_uv_literal_bits);
  lua_setglobal(L, "rooted_uv_literal_bits");
  lua_pushcfunction(L, rooted_uv_force_double_literal);
  lua_setglobal(L, "rooted_uv_force_double_literal");
  assert(luaL_dostring(L,
    "function make_rooted_uv_pair(initial)\n"
    " local value=initial\n"
    " return function() return value end, function(v) value=v; return value end\n"
    "end\n") == LUA_OK);
  make_pair(L, &read_off, &write_off);
  anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  readers = gc2_smr_readers_acq(G(L));
  check_values(L, read_off, write_off);
  check_refusals(L, read_off, write_off);
  check_open_refusal(L, read_off);
  check_join(L, read_off, write_off);
  check_root_aliases(L);
  assert_clean(L, anchors, readers);
  lua_pop(L, 2);

  /* Run the optional Lua fixture only after the direct open-cell contract
  ** checks, before its active peer can affect open-upvalue creation. */
  if (argc >= 2 && luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "rooted upvalue Lua fixture: %s\n", lua_tostring(L, -1));
    abort();
  }
  lua_close(L);
  puts("t-func-rooted-upvalues OK");
  return 0;
}
