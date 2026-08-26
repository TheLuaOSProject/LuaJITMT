/*
** Native JIT-disabled ARM64 ISNEXT bytecode-publication regression.
**
** The companion source/object contract owns the target-before-guard ordering.
** This runtime fixture proves that the real interpreter reaches the failure
** path, preserves every non-opcode bit in both shared instruction words, and
** continues through the resulting generic iterator on later executions.
*/

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && defined(LUAJIT_DISABLE_JIT)

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_obj.h"

#include "lib/lua_fixture_helpers.h"

#if LJ_HASJIT
#error "t-arm64-isnext-publication requires a compile-time disabled JIT"
#endif

typedef struct ISNextPair {
  GCproto *pt;
  BCIns *guard;
  BCIns *target;
  BCIns guard_original;
  BCIns target_original;
} ISNextPair;

typedef struct FixtureRefs {
  int iterate;
  int wrapper;
  int saved_next;
} FixtureRefs;

static const char fixture_lua[] =
  "local saved_next = next\n"
  "local function wrapped_next(t, control)\n"
  "  return saved_next(t, control)\n"
  "end\n"
  "local function iterate(t)\n"
  "  local marker = 'isnext-frame-ok'\n"
  "  local count, sum = 0, 0\n"
  "  for _, value in next, t, nil do\n"
  "    count = count + 1\n"
  "    sum = sum + value\n"
  "  end\n"
  "  return marker, count, sum, count * 100 + sum\n"
  "end\n"
  "return iterate, wrapped_next, saved_next\n";

static BCIns load_bc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static BCIns replace_op(BCIns ins, BCOp op)
{
  setbc_op(&ins, op);
  return ins;
}

static FixtureRefs load_fixture(lua_State *L, const char *chunkname)
{
  FixtureRefs refs;
  ljt_lua_assert_ok(L,
    luaL_loadbuffer(L, fixture_lua, sizeof(fixture_lua) - 1, chunkname),
    "load ARM64 ISNEXT fixture");
  ljt_lua_pcall(L, 0, 3, "initialize ARM64 ISNEXT fixture");
  assert(lua_isfunction(L, -3));
  assert(lua_isfunction(L, -2));
  assert(lua_isfunction(L, -1));
  refs.saved_next = luaL_ref(L, LUA_REGISTRYINDEX);
  refs.wrapper = luaL_ref(L, LUA_REGISTRYINDEX);
  refs.iterate = luaL_ref(L, LUA_REGISTRYINDEX);
  return refs;
}

static void release_fixture(lua_State *L, const FixtureRefs *refs)
{
  luaL_unref(L, LUA_REGISTRYINDEX, refs->iterate);
  luaL_unref(L, LUA_REGISTRYINDEX, refs->wrapper);
  luaL_unref(L, LUA_REGISTRYINDEX, refs->saved_next);
}

static GCproto *function_proto_ref(lua_State *L, int ref)
{
  GCfunc *fn;
  GCproto *pt;
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  assert(lua_isfunction(L, -1));
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static ISNextPair find_isnext_pair(GCproto *pt)
{
  ISNextPair pair;
  BCIns *bc;
  BCPos i;
  unsigned found = 0;

  memset(&pair, 0, sizeof(pair));
  assert(pt != NULL && pt->sizebc >= 3);
  pair.pt = pt;
  bc = proto_bc(pt);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = load_bc(&bc[i]);
    if (bc_op(ins) == BC_ISNEXT) {
      ptrdiff_t target_index = (ptrdiff_t)i + 1 + bc_j(ins);
      assert(target_index >= 0 && target_index < (ptrdiff_t)pt->sizebc);
      assert(++found == 1);
      pair.guard = &bc[i];
      pair.target = &bc[target_index];
      pair.guard_original = ins;
      pair.target_original = load_bc(pair.target);
    }
  }

  assert(found == 1 && pair.guard != NULL && pair.target != NULL);
  assert(bc_op(pair.guard_original) == BC_ISNEXT);
  assert(bc_op(pair.target_original) == BC_ITERN);
  assert(bc_a(pair.guard_original) == bc_a(pair.target_original));
  assert(pair.target + 1 < bc + pt->sizebc);
  assert(bc_op(load_bc(pair.target + 1)) == BC_ITERL);
  return pair;
}

static void set_global_ref(lua_State *L, const char *name, int ref)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  assert(lua_isfunction(L, -1));
  lua_setglobal(L, name);
}

static void push_input(lua_State *L)
{
  static const int values[] = { 3, 5, 7, 11 };
  size_t i;
  lua_createtable(L, (int)(sizeof(values) / sizeof(values[0])), 0);
  for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    lua_pushinteger(L, values[i]);
    lua_rawseti(L, -2, (int)i + 1);
  }
}

static void run_iteration(lua_State *L, int iterate_ref, const char *what)
{
  int base = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, iterate_ref);
  assert(lua_isfunction(L, -1));
  push_input(L);
  ljt_lua_pcall(L, 1, 4, what);
  assert(lua_isstring(L, base + 1));
  assert(strcmp(lua_tostring(L, base + 1), "isnext-frame-ok") == 0);
  assert(lua_isnumber(L, base + 2) && lua_tointeger(L, base + 2) == 4);
  assert(lua_isnumber(L, base + 3) && lua_tointeger(L, base + 3) == 26);
  assert(lua_isnumber(L, base + 4) && lua_tointeger(L, base + 4) == 426);
  lua_settop(L, base);
}

static void assert_original_pair(const ISNextPair *pair)
{
  assert(load_bc(pair->guard) == pair->guard_original);
  assert(load_bc(pair->target) == pair->target_original);
}

static void assert_generic_pair(const ISNextPair *pair)
{
  BCIns guard = load_bc(pair->guard);
  BCIns target = load_bc(pair->target);
  assert(guard == replace_op(pair->guard_original, BC_JMP));
  assert(target == replace_op(pair->target_original, BC_ITERC));
  assert(bc_a(guard) == bc_a(pair->guard_original));
  assert(bc_d(guard) == bc_d(pair->guard_original));
  assert(bc_a(target) == bc_a(pair->target_original));
  assert(bc_b(target) == bc_b(pair->target_original));
  assert(bc_c(target) == bc_c(pair->target_original));
}

static void assert_intermediate_pair(const ISNextPair *pair)
{
  assert(load_bc(pair->guard) == pair->guard_original);
  assert(load_bc(pair->target) ==
         replace_op(pair->target_original, BC_ITERC));
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  FixtureRefs intermediate_refs, transition_refs;
  ISNextPair intermediate, transition;

  /* Exercise the only transient pair admitted by target-first publication on
  ** an independent prototype. A successful ISNEXT initializes LJ_KEYINDEX;
  ** generic ITERC/next must accept that cursor without retiring the guard. */
  intermediate_refs = load_fixture(L, "=arm64-isnext-intermediate");
  intermediate = find_isnext_pair(
    function_proto_ref(L, intermediate_refs.iterate));
  run_iteration(L, intermediate_refs.iterate, "intermediate baseline");
  assert_original_pair(&intermediate);
  lj_bc_publish_op_vm((uint32_t *)intermediate.target, BC_ITERC);
  assert_intermediate_pair(&intermediate);
  run_iteration(L, intermediate_refs.iterate, "ISNEXT plus ITERC");
  assert_intermediate_pair(&intermediate);
  release_fixture(L, &intermediate_refs);

  /* Use a separately compiled prototype for the authentic failed-ISNEXT
  ** transition. This prevents the synthetic intermediate setup above from
  ** satisfying any of the failure-path assertions. */
  transition_refs = load_fixture(L, "=arm64-isnext-transition");
  transition = find_isnext_pair(
    function_proto_ref(L, transition_refs.iterate));

  /* The unmodified builtin takes the successful specialization and must not
  ** mutate either shared bytecode word. */
  run_iteration(L, transition_refs.iterate, "builtin ISNEXT baseline");
  assert_original_pair(&transition);

  /* A Lua wrapper has the same call semantics but not FF_next_N. This forces
  ** the real failed-ISNEXT path and its terminal two-word transition. */
  set_global_ref(L, "next", transition_refs.wrapper);
  run_iteration(L, transition_refs.iterate, "wrapped next despecialization");
  assert_generic_pair(&transition);

  /* The same prototype is now permanently generic. Restore the builtin and
  ** prove JMP -> ITERC redispatch, stack BASE and PC all remain coherent. */
  set_global_ref(L, "next", transition_refs.saved_next);
  lua_gc(L, LUA_GCCOLLECT, 0);
  run_iteration(L, transition_refs.iterate, "generic next rerun");
  assert_generic_pair(&transition);

  release_fixture(L, &transition_refs);
  lua_close(L);
  puts("t-arm64-isnext-publication OK: intermediate, exact words and "
       "generic rerun verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-isnext-publication SKIP: requires the native macOS arm64 "
       "disabled-JIT bootstrap");
  return 0;
}

#endif
