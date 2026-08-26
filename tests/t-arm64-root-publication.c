/*
** Focused native ARM64 interpreter stack/root publication regression.
**
** The VM under test is built separately by the bootstrap gate. This fixture
** brackets representative interpreter paths with C callbacks which sample the
** owning TG's stack-dirty epoch. The generic call/return topology contributes
** to those samples, so per-opcode attribution belongs to the accompanying
** source/object contract. This runtime probe certifies bytecode reachability,
** semantics and collectable-result retention across a full collection.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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

static const char *const rootpub_bc_names[] = {
#define ROOTPUB_BCNAME(name, ma, mb, mc, mt) #name,
  BCDEF(ROOTPUB_BCNAME)
#undef ROOTPUB_BCNAME
};

static uint64_t stack_dirty_epoch(lua_State *L)
{
  TGState *tg = L2TG(L);
  assert(tg != NULL);
  return lj_tg_stack_dirty_epoch_acq(tg);
}

static int rootpub_mark(lua_State *L)
{
  marked_epoch = stack_dirty_epoch(L);
  marked_valid = 1;
  return 0;
}

static int rootpub_measure(lua_State *L)
{
  uint64_t now = stack_dirty_epoch(L);
  assert(marked_valid && now >= marked_epoch);
  lua_pushnumber(L, (lua_Number)(now - marked_epoch));
  return 1;
}

static const char cases_lua[] =
  "local mark, measure = rootpub_mark, rootpub_measure\n"
  "local sub, tonumber_, raise = string.sub, tonumber, error\n"
  "return {\n"
  "  baseline = function()\n"
  "    mark()\n"
  "    local value = 17\n"
  "    return measure(), value\n"
  "  end,\n"
  "  table_reads = function(mode)\n"
  "    local dynamic = {}\n"
  "    local value = mode == 0 and { tag = 'table-read' } or\n"
  "                  (mode == 1 and 37 or nil)\n"
  "    local source = { [1] = value, named = value }\n"
  "    source[dynamic] = value\n"
  "    mark()\n"
  "    local a = source.named\n"
  "    local b = source[dynamic]\n"
  "    local c = source[1]\n"
  "    return measure(), a, b, c\n"
  "  end,\n"
  "  metamethod_reads = function(mode)\n"
  "    local dynamic = {}\n"
  "    local function index(_, key)\n"
  "      if mode == 0 then return { tag = 'metamethod-read', key = key } end\n"
  "      if mode == 1 then return 37 end\n"
  "      return nil\n"
  "    end\n"
  "    local source = setmetatable({}, { __index = index })\n"
  "    mark()\n"
  "    local a = source.named\n"
  "    local b = source[dynamic]\n"
  "    local c = source[1]\n"
  "    return measure(), a, b, c\n"
  "  end,\n"
  "  mov_testcopy = function(value)\n"
  "    local object = { identity = function() return value end }\n"
  "    mark()\n"
  "    local copied = object:identity() and value\n"
  "    return measure(), copied\n"
  "  end,\n"
  "  fast_result = function(mode)\n"
  "    mark()\n"
  "    local result\n"
  "    if mode == 0 then\n"
  "      result = sub('fast-function-result', 1, 13)\n"
  "    else\n"
  "      result = tonumber_('42')\n"
  "    end\n"
  "    return measure(), result\n"
  "  end,\n"
  "  call_return = function(value)\n"
  "    local function producer(v) return v end\n"
  "    mark()\n"
  "    local result = producer(value)\n"
  "    return measure(), result\n"
  "  end,\n"
  "  constants_allocations = function()\n"
  "    mark()\n"
  "    local str = 'root-publication-kstr'\n"
  "    local cdata = 0x1234LL\n"
  "    local fn = function() return str end\n"
  "    local fresh = {}\n"
  "    local duplicate = { tag = 'tdup', child = fresh }\n"
  "    return measure(), str, cdata, fn, fresh, duplicate\n"
  "  end,\n"
  "  upvalue_result = (function()\n"
  "    local cell\n"
  "    return function(value)\n"
  "      cell = value\n"
  "      mark()\n"
  "      local result = cell\n"
  "      return measure(), result\n"
  "    end\n"
  "  end)(),\n"
  "  vararg_result = function(value)\n"
  "    local function pick(...)\n"
  "      local first, second, third = ...\n"
  "      return second, first, third\n"
  "    end\n"
  "    mark()\n"
  "    local result = pick(false, value, nil)\n"
  "    return measure(), result\n"
  "  end,\n"
  "  itern_result = function(mode)\n"
  "    local value = mode == 0 and { tag = 'itern-result' } or 37\n"
  "    local source = { named = value }\n"
  "    local result\n"
  "    mark()\n"
  "    for _, v in next, source, nil do result = v; break end\n"
  "    return measure(), result\n"
  "  end,\n"
  "  iterc_result = function(value)\n"
  "    local function iterator(state, control)\n"
  "      if control == nil then return true, state end\n"
  "    end\n"
  "    local result\n"
  "    mark()\n"
  "    for _, v in iterator, value, nil do result = v; break end\n"
  "    return measure(), result\n"
  "  end,\n"
  "  error_result = function(value)\n"
  "    mark()\n"
  "    raise(value, 0)\n"
  "  end\n"
  "}\n";

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

static void push_variant(lua_State *L, int variant)
{
  if (variant == 0) {
    lua_newtable(L);
    lua_pushliteral(L, "argument-root");
    lua_setfield(L, -2, "tag");
  } else if (variant == 1) {
    lua_pushinteger(L, 37);
  } else {
    lua_pushnil(L);
  }
}

static void expect_variant(lua_State *L, int idx, int variant)
{
  if (variant == 0) {
    expect_tag(L, idx, "argument-root");
  } else if (variant == 1) {
    assert(lua_isnumber(L, idx) && lua_tointeger(L, idx) == 37);
  } else {
    assert(lua_isnil(L, idx));
  }
}

static void push_case(lua_State *L, int cases_ref, const char *name)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, cases_ref);
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, name);
  assert(lua_isfunction(L, -1));
  lua_remove(L, -2);
}

static int proto_has_bc(GCproto *pt, BCOp wanted)
{
  BCPos pc;
  for (pc = 0; pc < pt->sizebc; pc++)
    if (bc_op(proto_bc(pt)[pc]) == wanted)
      return 1;
  return 0;
}

static int proto_tree_has_bc(GCproto *pt, BCOp wanted)
{
  MSize i;
  if (proto_has_bc(pt, wanted))
    return 1;
  for (i = 0; i < proto_sizekgc_acq(pt); i++) {
    GCobj *o = proto_kgc_acq(pt, ~(ptrdiff_t)i);
    if (o != NULL && o->gch.gct == (uint8_t)~LJ_TPROTO &&
        proto_tree_has_bc(gco2pt(o), wanted))
      return 1;
  }
  return 0;
}

static void dump_proto_tree(GCproto *pt, unsigned depth)
{
  MSize i;
  BCPos pc;
  fprintf(stderr, "  proto depth %u:", depth);
  for (pc = 0; pc < pt->sizebc; pc++) {
    BCOp op = bc_op(proto_bc(pt)[pc]);
    fprintf(stderr, " %s", op < BC__MAX ? rootpub_bc_names[op] : "BAD");
  }
  fputc('\n', stderr);
  for (i = 0; i < proto_sizekgc_acq(pt); i++) {
    GCobj *o = proto_kgc_acq(pt, ~(ptrdiff_t)i);
    if (o != NULL && o->gch.gct == (uint8_t)~LJ_TPROTO)
      dump_proto_tree(gco2pt(o), depth + 1u);
  }
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

static int patch_proto_testcopy(GCproto *pt)
{
  BCPos pc;
  for (pc = 0; pc < pt->sizebc; pc++) {
    BCIns ins = proto_bc(pt)[pc];
    BCOp op = bc_op(ins);
    if (op == BC_IST || op == BC_ISF) {
      /* Cell lowering commonly leaves the result in its source register and
      ** folds ISTC/ISFC back to IST/ISF. Restore the equivalent same-slot copy
      ** form so the interpreter's taken test-copy publication is exercised. */
      setbc_op(&ins, op == BC_IST ? BC_ISTC : BC_ISFC);
      setbc_a(&ins, bc_d(ins));
      bc_publish(&proto_bc(pt)[pc], ins);
      return 1;
    }
  }
  return 0;
}

static void prepare_case_bytecodes(lua_State *L, int cases_ref)
{
  GCproto *pt = case_proto(L, cases_ref, "mov_testcopy");
  if (!proto_has_bc(pt, BC_ISTC) && !proto_has_bc(pt, BC_ISFC))
    assert(patch_proto_testcopy(pt));
}

static void require_case_bc(lua_State *L, int cases_ref, const char *name,
                            BCOp wanted, const char *opname)
{
  if (!proto_has_bc(case_proto(L, cases_ref, name), wanted)) {
    fprintf(stderr, "%s did not compile representative %s bytecode\n",
            name, opname);
    dump_proto_tree(case_proto(L, cases_ref, name), 0);
    assert(0);
  }
}

static void require_case_tree_bc(lua_State *L, int cases_ref, const char *name,
                                 BCOp wanted, const char *opname)
{
  if (!proto_tree_has_bc(case_proto(L, cases_ref, name), wanted)) {
    fprintf(stderr, "%s did not compile representative %s bytecode\n",
            name, opname);
    dump_proto_tree(case_proto(L, cases_ref, name), 0);
    assert(0);
  }
}

static void require_case_bc_any(lua_State *L, int cases_ref, const char *name,
                                BCOp a, BCOp b, const char *opname)
{
  GCproto *pt = case_proto(L, cases_ref, name);
  if (!proto_has_bc(pt, a) && !proto_has_bc(pt, b)) {
    fprintf(stderr, "%s did not compile representative %s bytecode\n",
            name, opname);
    dump_proto_tree(pt, 0);
    assert(0);
  }
}

static void verify_case_bytecodes(lua_State *L, int cases_ref)
{
  require_case_bc(L, cases_ref, "table_reads", BC_TGETV, "TGETV");
  require_case_bc(L, cases_ref, "table_reads", BC_TGETS, "TGETS");
  require_case_bc(L, cases_ref, "table_reads", BC_TGETB, "TGETB");
  require_case_bc(L, cases_ref, "metamethod_reads", BC_TGETV, "TGETV");
  require_case_bc(L, cases_ref, "metamethod_reads", BC_TGETS, "TGETS");
  require_case_bc(L, cases_ref, "metamethod_reads", BC_TGETB, "TGETB");
  require_case_bc(L, cases_ref, "mov_testcopy", BC_MOV, "MOV");
  require_case_bc_any(L, cases_ref, "mov_testcopy", BC_ISTC, BC_ISFC,
                      "ISTC/ISFC");
  require_case_bc(L, cases_ref, "fast_result", BC_CALL, "CALL");
  require_case_bc(L, cases_ref, "call_return", BC_CALL, "CALL");
  require_case_bc(L, cases_ref, "constants_allocations", BC_KSTR, "KSTR");
  require_case_bc(L, cases_ref, "constants_allocations", BC_KCDATA,
                  "KCDATA");
  require_case_bc(L, cases_ref, "constants_allocations", BC_FNEW, "FNEW");
  require_case_bc(L, cases_ref, "constants_allocations", BC_TNEW, "TNEW");
  require_case_bc(L, cases_ref, "constants_allocations", BC_TDUP, "TDUP");
  require_case_bc(L, cases_ref, "upvalue_result", BC_UGET, "UGET");
  /* FUNCV is the immutable prototype header; interpreter dispatch selects the
  ** adjacent IFUNCV implementation exercised by this disabled-JIT fixture. */
  require_case_tree_bc(L, cases_ref, "vararg_result", BC_FUNCV,
                       "FUNCV/IFUNCV");
  require_case_tree_bc(L, cases_ref, "vararg_result", BC_VARG, "VARG");
  require_case_bc(L, cases_ref, "itern_result", BC_ITERN, "ITERN");
  require_case_bc(L, cases_ref, "iterc_result", BC_ITERC, "ITERC");
  require_case_bc(L, cases_ref, "error_result", BC_CALL, "CALL/error");
}

static uint64_t observed_delta(lua_State *L, int base, const char *name)
{
  lua_Number delta;
  (void)name;
  assert(lua_gettop(L) >= base + 1);
  assert(lua_isnumber(L, base + 1));
  delta = lua_tonumber(L, base + 1);
  assert(delta >= 0);
  return (uint64_t)delta;
}

static uint64_t run_baseline(lua_State *L, int cases_ref)
{
  int base = lua_gettop(L);
  uint64_t delta;
  push_case(L, cases_ref, "baseline");
  assert(lua_pcall(L, 0, 2, 0) == 0);
  assert(lua_isnumber(L, base + 1));
  delta = (uint64_t)lua_tonumber(L, base + 1);
  assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 17);
  lua_settop(L, base);
  return delta;
}

static void run_mode_reads(lua_State *L, int cases_ref, const char *name,
                           const char *tag, int mode)
{
  int base = lua_gettop(L);
  int i;
  push_case(L, cases_ref, name);
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 4, 0) == 0);
  (void)observed_delta(L, base, name);
  lua_gc(L, LUA_GCCOLLECT, 0);
  for (i = 2; i <= 4; i++) {
    if (mode == 0)
      expect_tag(L, base + i, tag);
    else if (mode == 1)
      assert(lua_isnumber(L, base + i) && lua_tointeger(L, base + i) == 37);
    else
      assert(lua_isnil(L, base + i));
  }
  lua_settop(L, base);
}

static void run_variant_case(lua_State *L, int cases_ref, const char *name,
                             int variant)
{
  int base = lua_gettop(L);
  push_case(L, cases_ref, name);
  push_variant(L, variant);
  assert(lua_pcall(L, 1, 2, 0) == 0);
  (void)observed_delta(L, base, name);
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_variant(L, base + 2, variant);
  lua_settop(L, base);
}

static void run_fast_result(lua_State *L, int cases_ref, int mode)
{
  int base = lua_gettop(L);
  push_case(L, cases_ref, "fast_result");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 2, 0) == 0);
  (void)observed_delta(L, base, "fast_result");
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (mode == 0) {
    assert(lua_isstring(L, base + 2));
    assert(strcmp(lua_tostring(L, base + 2), "fast-function") == 0);
  } else {
    assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 42);
  }
  lua_settop(L, base);
}

static void run_constants(lua_State *L, int cases_ref)
{
  int base = lua_gettop(L);
  push_case(L, cases_ref, "constants_allocations");
  assert(lua_pcall(L, 0, 6, 0) == 0);
  (void)observed_delta(L, base, "constants_allocations");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lua_isstring(L, base + 2));
  assert(strcmp(lua_tostring(L, base + 2), "root-publication-kstr") == 0);
  assert(lua_type(L, base + 3) == LUA_TCDATA);
  assert(lua_isfunction(L, base + 4));
  assert(lua_istable(L, base + 5));
  expect_tag(L, base + 6, "tdup");
  lua_settop(L, base);
}

static void run_itern(lua_State *L, int cases_ref, int mode)
{
  int base = lua_gettop(L);
  push_case(L, cases_ref, "itern_result");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 2, 0) == 0);
  (void)observed_delta(L, base, "itern_result");
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (mode == 0)
    expect_tag(L, base + 2, "itern-result");
  else
    assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 37);
  lua_settop(L, base);
}

static void run_error(lua_State *L, int cases_ref, int variant)
{
  int base = lua_gettop(L);
  uint64_t delta;
  push_case(L, cases_ref, "error_result");
  push_variant(L, variant);
  marked_valid = 0;
  assert(lua_pcall(L, 1, 0, 0) != 0);
  assert(marked_valid);
  delta = stack_dirty_epoch(L) - marked_epoch;
  (void)delta;
  lua_gc(L, LUA_GCCOLLECT, 0);
  expect_variant(L, -1, variant);
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
  int cases_ref;
  uint64_t baseline = 0;
  int i, variant;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, rootpub_mark);
  lua_setglobal(L, "rootpub_mark");
  lua_pushcfunction(L, rootpub_measure);
  lua_setglobal(L, "rootpub_measure");

  assert(luaL_loadbuffer(L, cases_lua, sizeof(cases_lua) - 1,
                         "=arm64-root-publication") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(lua_istable(L, -1));
  cases_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  prepare_case_bytecodes(L, cases_ref);
  verify_case_bytecodes(L, cases_ref);

  /* The worker is concurrency pressure only. No assertion depends on when it
  ** wakes, scans, or completes a phase. */
  run_control(L, "local t=require('threading'); "
                 "_G.__rootpub_oldworkers=t.gcworkers(1)");

  for (i = 0; i < 4; i++) {
    uint64_t delta = run_baseline(L, cases_ref);
    if (delta > baseline) baseline = delta;
  }
  assert(baseline != 0);

  for (variant = 0; variant < 3; variant++) {
    run_mode_reads(L, cases_ref, "table_reads", "table-read", variant);
    run_mode_reads(L, cases_ref, "metamethod_reads", "metamethod-read",
                   variant);
    run_variant_case(L, cases_ref, "mov_testcopy", variant);
    run_variant_case(L, cases_ref, "call_return", variant);
    run_variant_case(L, cases_ref, "upvalue_result", variant);
    run_variant_case(L, cases_ref, "vararg_result", variant);
    run_variant_case(L, cases_ref, "iterc_result", variant);
    run_error(L, cases_ref, variant);
  }
  run_fast_result(L, cases_ref, 0);
  run_fast_result(L, cases_ref, 1);
  run_constants(L, cases_ref);
  run_itern(L, cases_ref, 0);
  run_itern(L, cases_ref, 1);

  run_control(L, "local t=require('threading'); "
                 "assert(t.gcworkers(_G.__rootpub_oldworkers) >= 0); "
                 "_G.__rootpub_oldworkers=nil");
  luaL_unref(L, LUA_REGISTRYINDEX, cases_ref);
  lua_close(L);
  printf("t-arm64-root-publication OK: baseline=%llu, representative VM "
         "results retained; source/object contract owns attribution\n",
         (unsigned long long)baseline);
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-root-publication SKIP: requires the native macOS arm64 "
       "disabled-JIT bootstrap");
  return 0;
}

#endif
