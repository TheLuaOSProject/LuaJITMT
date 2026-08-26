/*
** Native ARM64 interpreter metamethod/metatable publication regression.
**
** The Lua cases return their live results to this C frame. The harness samples
** the owning TG's stack-dirty epoch, keeps collectable results on the Lua
** stack across a full collection, and then validates their contents. Generic
** call/return paths also dirty the stack, so the source/object contract owns
** per-family publication attribution.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && defined(LUAJIT_DISABLE_JIT)

#include "lj_bc.h"
#include "lj_obj.h"
#include "lj_tg.h"

static uint64_t marked_epoch;
static int marked_valid;

static const char *const meta_bc_names[] = {
#define META_BCNAME(name, ma, mb, mc, mt) #name,
  BCDEF(META_BCNAME)
#undef META_BCNAME
};

static uint64_t stack_dirty_epoch(lua_State *L)
{
  TGState *tg = L2TG(L);
  assert(tg != NULL);
  return lj_tg_stack_dirty_epoch_acq(tg);
}

static int meta_mark(lua_State *L)
{
  marked_epoch = stack_dirty_epoch(L);
  marked_valid = 1;
  return 0;
}

static int meta_measure(lua_State *L)
{
  uint64_t now = stack_dirty_epoch(L);
  assert(marked_valid && now >= marked_epoch);
  lua_pushnumber(L, (lua_Number)(now - marked_epoch));
  return 1;
}

static int absolute_index(lua_State *L, int idx)
{
  return idx > 0 ? idx : lua_gettop(L) + idx + 1;
}

static void expect_tag(lua_State *L, int idx, const char *tag)
{
  idx = absolute_index(L, idx);
  assert(lua_istable(L, idx));
  lua_getfield(L, idx, "tag");
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), tag) == 0);
  lua_pop(L, 1);
}

static void expect_array_tag(lua_State *L, int idx, int element,
                             const char *tag)
{
  idx = absolute_index(L, idx);
  assert(lua_istable(L, idx));
  lua_rawgeti(L, idx, element);
  expect_tag(L, -1, tag);
  lua_pop(L, 1);
}

static void expect_field_tag(lua_State *L, int idx, const char *field,
                             const char *tag)
{
  idx = absolute_index(L, idx);
  lua_getfield(L, idx, field);
  expect_tag(L, -1, tag);
  lua_pop(L, 1);
}

static void expect_field_same(lua_State *L, int idx, const char *field,
                              int expected)
{
  idx = absolute_index(L, idx);
  expected = absolute_index(L, expected);
  lua_getfield(L, idx, field);
  assert(lua_rawequal(L, -1, expected));
  lua_pop(L, 1);
}

static void expect_array_field_tag(lua_State *L, int idx, int element,
                                   const char *field, const char *tag)
{
  idx = absolute_index(L, idx);
  lua_rawgeti(L, idx, element);
  expect_field_tag(L, -1, field, tag);
  lua_pop(L, 1);
}

static void expect_array_field_same(lua_State *L, int idx, int element,
                                    const char *field, int expected)
{
  idx = absolute_index(L, idx);
  expected = absolute_index(L, expected);
  lua_rawgeti(L, idx, element);
  expect_field_same(L, -1, field, expected);
  lua_pop(L, 1);
}

static void expect_metatable(lua_State *L, int value, int mt)
{
  value = absolute_index(L, value);
  mt = absolute_index(L, mt);
  assert(lua_getmetatable(L, value) == 1);
  assert(lua_rawequal(L, -1, mt));
  lua_pop(L, 1);
}

static int proto_has_bc(GCproto *pt, BCOp wanted)
{
  BCPos pc;
  for (pc = 0; pc < pt->sizebc; pc++)
    if (bc_op(proto_bc(pt)[pc]) == wanted)
      return 1;
  return 0;
}

static int proto_has_any(GCproto *pt, const BCOp *wanted, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++)
    if (proto_has_bc(pt, wanted[i]))
      return 1;
  return 0;
}

static void dump_proto(GCproto *pt)
{
  BCPos pc;
  fprintf(stderr, "  bytecode:");
  for (pc = 0; pc < pt->sizebc; pc++) {
    BCOp op = bc_op(proto_bc(pt)[pc]);
    fprintf(stderr, " %s", op < BC__MAX ? meta_bc_names[op] : "BAD");
  }
  fputc('\n', stderr);
}

static void push_case(lua_State *L, int cases_ref, const char *name)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, cases_ref);
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, name);
  assert(lua_isfunction(L, -1));
  lua_remove(L, -2);
}

static GCproto *case_proto(lua_State *L, int cases_ref, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  push_case(L, cases_ref, name);
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void require_case_bc(lua_State *L, int cases_ref, const char *name,
                            BCOp wanted, const char *description)
{
  GCproto *pt = case_proto(L, cases_ref, name);
  if (!proto_has_bc(pt, wanted)) {
    fprintf(stderr, "%s lacks representative %s bytecode\n",
            name, description);
    dump_proto(pt);
    assert(0);
  }
}

static void require_case_any(lua_State *L, int cases_ref, const char *name,
                             const BCOp *wanted, size_t n,
                             const char *description)
{
  GCproto *pt = case_proto(L, cases_ref, name);
  if (!proto_has_any(pt, wanted, n)) {
    fprintf(stderr, "%s lacks representative %s bytecode\n",
            name, description);
    dump_proto(pt);
    assert(0);
  }
}

static void verify_case_bytecodes(lua_State *L, int cases_ref)
{
  static const BCOp lt_ops[] = { BC_ISLT, BC_ISGE };
  static const BCOp le_ops[] = { BC_ISLE, BC_ISGT };
  static const BCOp eq_ops[] = { BC_ISEQV, BC_ISNEV };
  require_case_bc(L, cases_ref, "arithmetic", BC_ADDVV, "ADDVV");
  require_case_bc(L, cases_ref, "length", BC_LEN, "LEN");
  require_case_any(L, cases_ref, "ordering", lt_ops,
                   sizeof(lt_ops) / sizeof(lt_ops[0]), "LT comparison");
  require_case_any(L, cases_ref, "ordering", le_ops,
                   sizeof(le_ops) / sizeof(le_ops[0]), "LE comparison");
  require_case_bc(L, cases_ref, "table_equality", BC_ISEQV,
                  "table equality");
  require_case_bc(L, cases_ref, "table_equality", BC_ISNEV,
                  "table inequality");
  require_case_bc(L, cases_ref, "userdata_equality", BC_ISEQV,
                  "full-userdata equality");
  require_case_bc(L, cases_ref, "userdata_equality", BC_ISNEV,
                  "full-userdata inequality");
  require_case_any(L, cases_ref, "ffi_equality", eq_ops,
                   sizeof(eq_ops) / sizeof(eq_ops[0]),
                   "cdata equality");
  require_case_bc(L, cases_ref, "concatenation", BC_CAT, "CAT");
  require_case_bc(L, cases_ref, "callable", BC_CALL, "CALL/__call");
  require_case_bc(L, cases_ref, "metatables", BC_CALL,
                  "CALL/getmetatable/setmetatable");
  require_case_bc(L, cases_ref, "getmetatable_results", BC_CALL,
                  "CALL/direct getmetatable results");
}

static int run_case(lua_State *L, int cases_ref, const char *name, int nresults)
{
  int base = lua_gettop(L);
  push_case(L, cases_ref, name);
  marked_valid = 0;
  assert(lua_pcall(L, 0, nresults, 0) == 0);
  assert(marked_valid && lua_gettop(L) == base + nresults);
  return base;
}

static uint64_t raw_delta(lua_State *L, int idx)
{
  lua_Number delta;
  assert(lua_isnumber(L, idx));
  delta = lua_tonumber(L, idx);
  assert(delta >= 0);
  return (uint64_t)delta;
}

static uint64_t observed_delta(lua_State *L, int idx, const char *name)
{
  uint64_t delta = raw_delta(L, idx);
  (void)name;
  return delta;
}

static uint64_t run_baseline(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "baseline", 2);
  uint64_t delta = raw_delta(L, base + 1);
  assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 17);
  lua_settop(L, base);
  return delta;
}

static void run_arithmetic(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "arithmetic", 4);
  (void)observed_delta(L, base + 1, "arithmetic/__add");
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_tag(L, base + 2, "add-result");
  expect_tag(L, base + 3, "add-left");
  expect_tag(L, base + 4, "add-right");
  expect_field_tag(L, base + 2, "nested", "add-result-nested");
  expect_field_same(L, base + 2, "lhs", base + 3);
  expect_field_same(L, base + 2, "rhs", base + 4);
  expect_field_tag(L, base + 3, "nested", "add-left-nested");
  expect_field_tag(L, base + 4, "nested", "add-right-nested");
  lua_settop(L, base);
}

static int run_length(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "length", 4);
  int calls;
  assert(lua_isnumber(L, base + 3));
  calls = (int)lua_tointeger(L, base + 3);
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_tag(L, base + 4, "len-input");
  expect_field_tag(L, base + 4, "nested", "len-input-nested");
  if (calls != 0) {
    assert(calls == 1);
    (void)observed_delta(L, base + 1, "length/__len");
    expect_tag(L, base + 2, "len-result");
    expect_field_tag(L, base + 2, "nested", "len-result-nested");
    expect_field_same(L, base + 2, "value", base + 4);
  } else {
    (void)raw_delta(L, base + 1);
    assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 0);
  }
  lua_settop(L, base);
  return calls != 0;
}

static void run_ordering(lua_State *L, int cases_ref)
{
  uint32_t anchors0 = lj_tg_root_anchor_top_acq(L2TG(L));
  int base = run_case(L, cases_ref, "ordering", 18);
  (void)observed_delta(L, base + 1, "ordering/__lt/__le");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_isboolean(L, base + 2) && lua_isboolean(L, base + 3));
  assert(lua_toboolean(L, base + 2) && lua_toboolean(L, base + 3));
  assert(lua_isnumber(L, base + 4) && lua_isnumber(L, base + 5));
  assert(lua_tointeger(L, base + 4) == 1);
  assert(lua_tointeger(L, base + 5) == 1);
  expect_array_tag(L, base + 6, 1, "lt-token");
  expect_array_tag(L, base + 6, 2, "le-token");
  expect_array_field_tag(L, base + 6, 1, "nested", "lt-token-nested");
  expect_array_field_tag(L, base + 6, 1, "lhs", "order-left");
  expect_array_field_tag(L, base + 6, 1, "rhs", "order-right");
  expect_array_field_tag(L, base + 6, 2, "nested", "le-token-nested");
  expect_tag(L, base + 7, "order-left");
  expect_tag(L, base + 8, "order-right");
  expect_field_tag(L, base + 7, "nested", "order-left-nested");
  expect_field_tag(L, base + 8, "nested", "order-right-nested");
  assert(lua_isboolean(L, base + 9) && lua_toboolean(L, base + 9));
  assert(lua_isnumber(L, base + 10));
  assert(lua_tointeger(L, base + 10) == 1);
  expect_array_tag(L, base + 11, 1, "le-fallback-token");
  expect_array_field_tag(L, base + 11, 1, "nested",
                         "le-fallback-token-nested");
  /* Lua 5.1's <= fallback invokes rhs.__lt(rhs, lhs), then negates it. */
  expect_array_field_tag(L, base + 11, 1, "lhs", "order-fallback-right");
  expect_array_field_tag(L, base + 11, 1, "rhs", "order-fallback-left");
  expect_tag(L, base + 12, "order-fallback-left");
  expect_tag(L, base + 13, "order-fallback-right");
  expect_field_tag(L, base + 12, "nested", "order-fallback-left-nested");
  expect_field_tag(L, base + 13, "nested", "order-fallback-right-nested");
  assert(lua_isboolean(L, base + 14) && !lua_toboolean(L, base + 14));
  assert(lua_isstring(L, base + 15));
  assert(lua_isnumber(L, base + 16) && lua_isnumber(L, base + 17));
  assert(lua_tointeger(L, base + 16) == 0);
  assert(lua_tointeger(L, base + 17) == 0);
  assert(lua_isnumber(L, base + 18));
  assert(lua_tointeger(L, base + 18) == 64);
  lua_settop(L, base);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors0);
}

static void run_table_equality(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "table_equality", 19);
  (void)observed_delta(L, base + 1, "table __eq");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_isboolean(L, base + 2) && lua_isboolean(L, base + 3));
  assert(lua_isboolean(L, base + 4) && lua_isboolean(L, base + 5));
  assert(lua_isboolean(L, base + 6) && lua_isboolean(L, base + 7));
  assert(lua_isboolean(L, base + 8));
  assert(lua_toboolean(L, base + 2));
  assert(lua_toboolean(L, base + 3));
  assert(!lua_toboolean(L, base + 4));
  assert(lua_toboolean(L, base + 5));
  assert(!lua_toboolean(L, base + 6));
  assert(lua_toboolean(L, base + 7));
  assert(!lua_toboolean(L, base + 8));
  assert(lua_isnumber(L, base + 9) && lua_isnumber(L, base + 10));
  assert(lua_isnumber(L, base + 11) && lua_isnumber(L, base + 12));
  assert(lua_isnumber(L, base + 13));
  assert(lua_tointeger(L, base + 9) == 2);
  assert(lua_tointeger(L, base + 10) == 1);
  assert(lua_tointeger(L, base + 11) == 0);
  assert(lua_tointeger(L, base + 12) == 0);
  assert(lua_tointeger(L, base + 13) == 0);
  expect_array_tag(L, base + 14, 1, "eq-same-token");
  expect_array_tag(L, base + 14, 2, "eq-same-token");
  expect_array_tag(L, base + 14, 3, "eq-shared-token");
  expect_array_field_tag(L, base + 14, 1, "nested",
                         "eq-same-token-nested");
  expect_array_field_tag(L, base + 14, 1, "lhs", "eq-same-left");
  expect_array_field_tag(L, base + 14, 1, "rhs", "eq-same-right");
  expect_array_field_tag(L, base + 14, 3, "nested",
                         "eq-shared-token-nested");
  expect_array_field_tag(L, base + 14, 3, "lhs", "eq-shared-left");
  expect_array_field_tag(L, base + 14, 3, "rhs", "eq-shared-right");
  expect_tag(L, base + 15, "eq-same-left");
  expect_tag(L, base + 16, "eq-shared-left");
  expect_tag(L, base + 17, "eq-different-left");
  expect_tag(L, base + 18, "eq-one-sided-left");
  expect_tag(L, base + 19, "eq-one-sided-right");
  expect_field_tag(L, base + 15, "nested", "eq-same-left-nested");
  expect_field_tag(L, base + 16, "nested", "eq-shared-left-nested");
  expect_field_tag(L, base + 17, "nested", "eq-different-left-nested");
  expect_field_tag(L, base + 18, "nested", "eq-one-sided-left-nested");
  expect_field_tag(L, base + 19, "nested", "eq-one-sided-right-nested");
  lua_settop(L, base);
}

static int run_userdata_equality(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "userdata_equality", 23);
  int available;
  assert(lua_isboolean(L, base + 2));
  available = lua_toboolean(L, base + 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (available) {
    int i;
    (void)observed_delta(L, base + 1, "full userdata __eq");
    for (i = 3; i <= 9; i++) assert(lua_isboolean(L, base + i));
    assert(lua_toboolean(L, base + 3));
    assert(lua_toboolean(L, base + 4));
    assert(!lua_toboolean(L, base + 5));
    assert(lua_toboolean(L, base + 6));
    assert(!lua_toboolean(L, base + 7));
    assert(lua_toboolean(L, base + 8));
    assert(!lua_toboolean(L, base + 9));
    for (i = 10; i <= 14; i++) assert(lua_isnumber(L, base + i));
    assert(lua_tointeger(L, base + 10) == 2);
    assert(lua_tointeger(L, base + 11) == 1);
    assert(lua_tointeger(L, base + 12) == 0);
    assert(lua_tointeger(L, base + 13) == 0);
    assert(lua_tointeger(L, base + 14) == 0);
    expect_array_tag(L, base + 15, 1, "ud-eq-same-token");
    expect_array_tag(L, base + 15, 2, "ud-eq-same-token");
    expect_array_tag(L, base + 15, 3, "ud-eq-shared-token");
    expect_array_field_tag(L, base + 15, 1, "nested",
                           "ud-eq-same-token-nested");
    expect_array_field_tag(L, base + 15, 3, "nested",
                           "ud-eq-shared-token-nested");
    for (i = 16; i <= 23; i++)
      assert(lua_type(L, base + i) == LUA_TUSERDATA);
    expect_array_field_same(L, base + 15, 1, "lhs", base + 16);
    expect_array_field_same(L, base + 15, 1, "rhs", base + 17);
    expect_array_field_same(L, base + 15, 2, "lhs", base + 16);
    expect_array_field_same(L, base + 15, 2, "rhs", base + 17);
    expect_array_field_same(L, base + 15, 3, "lhs", base + 18);
    expect_array_field_same(L, base + 15, 3, "rhs", base + 19);
  } else {
    (void)raw_delta(L, base + 1);
  }
  lua_settop(L, base);
  return available;
}

static int run_ffi_equality(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "ffi_equality", 7);
  int available;
  assert(lua_isboolean(L, base + 2));
  available = lua_toboolean(L, base + 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (available) {
    (void)observed_delta(L, base + 1, "FFI metatype __eq");
    assert(lua_isboolean(L, base + 3));
    assert(lua_toboolean(L, base + 3));
    assert(lua_isnumber(L, base + 4));
    assert(lua_tointeger(L, base + 4) == 1);
    expect_array_tag(L, base + 5, 1, "ffi-eq-token");
    expect_array_field_tag(L, base + 5, 1, "nested",
                           "ffi-eq-token-nested");
    assert(lua_type(L, base + 6) == LUA_TCDATA);
    assert(lua_type(L, base + 7) == LUA_TCDATA);
  } else {
    (void)raw_delta(L, base + 1);
  }
  lua_settop(L, base);
  return available;
}

static void run_concatenation(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "concatenation", 3);
  (void)observed_delta(L, base + 1, "concatenation/__concat");
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_tag(L, base + 2, "concat-result-2");
  expect_field_tag(L, base + 2, "nested", "concat-result-nested-2");
  expect_field_tag(L, base + 2, "lhs", "concat-left");
  expect_field_tag(L, base + 2, "rhs", "concat-result-1");
  lua_getfield(L, base + 2, "rhs");
  expect_field_tag(L, -1, "nested", "concat-result-nested-1");
  expect_field_tag(L, -1, "lhs", "concat-middle");
  expect_field_tag(L, -1, "rhs", "concat-right");
  lua_pop(L, 1);
  assert(lua_isnumber(L, base + 3));
  assert(lua_tointeger(L, base + 3) == 2);
  lua_settop(L, base);
}

static void run_callable(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "callable", 4);
  (void)observed_delta(L, base + 1, "callable/__call");
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_tag(L, base + 2, "call-result");
  expect_tag(L, base + 3, "callable-input");
  expect_tag(L, base + 4, "call-argument");
  expect_field_tag(L, base + 2, "nested", "call-result-nested");
  expect_field_same(L, base + 2, "self", base + 3);
  expect_field_same(L, base + 2, "argument", base + 4);
  expect_field_tag(L, base + 3, "nested", "callable-input-nested");
  expect_field_tag(L, base + 4, "nested", "call-argument-nested");
  lua_settop(L, base);
}

static void run_metatables(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "metatables", 12);
  (void)observed_delta(L, base + 1, "getmetatable/setmetatable");
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_tag(L, base + 2, "metatable-target");
  expect_tag(L, base + 3, "metatable-first");
  expect_tag(L, base + 4, "metatable-second");
  expect_tag(L, base + 5, "metatable-protected");
  expect_tag(L, base + 6, "metatable-protected-token");
  expect_field_tag(L, base + 2, "nested", "metatable-target-nested");
  expect_field_tag(L, base + 3, "nested", "metatable-first-nested");
  expect_field_tag(L, base + 4, "nested", "metatable-second-nested");
  expect_field_tag(L, base + 5, "nested", "metatable-protected-nested");
  expect_field_tag(L, base + 6, "nested",
                   "metatable-protected-token-nested");
  expect_metatable(L, base + 2, base + 5);
  assert(lua_isboolean(L, base + 7));
  assert(!lua_toboolean(L, base + 7));
  assert(lua_isstring(L, base + 8));
  assert(lua_isboolean(L, base + 9));
  assert(!lua_toboolean(L, base + 9));
  assert(lua_isstring(L, base + 10));
  assert(lua_isboolean(L, base + 11));
  assert(!lua_toboolean(L, base + 11));
  assert(lua_isstring(L, base + 12));
  lua_settop(L, base);
}

static void run_getmetatable_results(lua_State *L, int cases_ref)
{
  int base = run_case(L, cases_ref, "getmetatable_results", 5);
  (void)observed_delta(L, base + 1, "direct getmetatable results");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_isnil(L, base + 2));
  expect_tag(L, base + 3, "getmetatable-raw-result");
  expect_field_tag(L, base + 3, "nested",
                   "getmetatable-raw-result-nested");
  expect_tag(L, base + 4, "getmetatable-protected-result");
  expect_field_tag(L, base + 4, "nested",
                   "getmetatable-protected-result-nested");
  assert(lua_isboolean(L, base + 5) && !lua_toboolean(L, base + 5));
  lua_settop(L, base);
}

static void run_control(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != 0) {
    fprintf(stderr, "control chunk failed: %s\n", lua_tostring(L, -1));
    assert(status == 0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  const char *root = getenv("LJ_TEST_ROOT");
  char path[4096];
  int cases_ref, i, len_supported, userdata_supported, ffi_supported, pathlen;
  uint64_t baseline = 0;
  uint32_t anchors0;

  assert(L != NULL && root != NULL && root[0] != '\0');
  pathlen = snprintf(path, sizeof(path),
                     "%s/tests/t-arm64-meta-publication.lua", root);
  assert(pathlen > 0 && (size_t)pathlen < sizeof(path));
  luaL_openlibs(L);

  assert(luaL_loadfile(L, path) == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(lua_isfunction(L, -1));
  lua_pushcfunction(L, meta_mark);
  lua_pushcfunction(L, meta_measure);
  assert(lua_pcall(L, 2, 1, 0) == 0);
  assert(lua_istable(L, -1));
  cases_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  verify_case_bytecodes(L, cases_ref);

  /* Concurrency pressure only: no assertion depends on worker timing. */
  run_control(L, "local t=require('threading'); "
                 "_G.__arm64_meta_oldworkers=t.gcworkers(1)");
  for (i = 0; i < 4; i++) {
    uint64_t delta = run_baseline(L, cases_ref);
    if (delta > baseline) baseline = delta;
  }
  anchors0 = lj_tg_root_anchor_top_acq(L2TG(L));

  run_arithmetic(L, cases_ref);
  len_supported = run_length(L, cases_ref);
  run_ordering(L, cases_ref);
  run_table_equality(L, cases_ref);
  userdata_supported = run_userdata_equality(L, cases_ref);
  ffi_supported = run_ffi_equality(L, cases_ref);
  run_concatenation(L, cases_ref);
  run_callable(L, cases_ref);
  run_metatables(L, cases_ref);
  run_getmetatable_results(L, cases_ref);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors0);

  run_control(L, "local t=require('threading'); "
                 "assert(t.gcworkers(_G.__arm64_meta_oldworkers) >= 0); "
                 "_G.__arm64_meta_oldworkers=nil");
  luaL_unref(L, LUA_REGISTRYINDEX, cases_ref);
  lua_close(L);
  printf("t-arm64-meta-publication OK: baseline=%llu, __len=%s, "
         "userdata __eq=%s, FFI __eq=%s\n",
         (unsigned long long)baseline,
         len_supported ? "covered" : "unsupported",
         userdata_supported ? "covered" : "unavailable",
         ffi_supported ? "covered" : "unavailable");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-meta-publication SKIP: requires the native macOS arm64 "
       "disabled-JIT bootstrap");
  return 0;
}

#endif
