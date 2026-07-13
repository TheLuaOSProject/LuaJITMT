/*
** Immutable prototype-side original-bytecode recovery regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_GC2_TEST_HELPERS) && !defined(LJ_TRACE_TEST_HELPERS)
#error "t-jit-startins-sidecar requires GC2 or trace test helpers"
#endif

#if LJ_TARGET_X64
typedef struct PatchedPC {
  GCproto *pt;
  BCIns *pc;
  BCIns original;
} PatchedPC;

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void check_fresh_geometry(GCproto *pt)
{
  BCPos i;
  assert(pt != NULL && pt->sizebc != 0);
  assert(proto_jit_startins(pt) == proto_bc(pt) + pt->sizebc);
  assert(pt->sizept >= sizeof(GCproto) +
	 (MSize)pt->sizebc * 2u * (MSize)sizeof(BCIns));
  for (i = 0; i < pt->sizebc; i++)
    assert((BCIns)la_load32_acq(
	(const uint32_t *)&proto_jit_startins(pt)[i]) == 0);
}

static PatchedPC patch_first_op(lua_State *L, const char *name, BCOp want)
{
  GCproto *pt = global_proto(L, name);
  BCIns *bc = proto_bc(pt);
  PatchedPC patch = { pt, NULL, 0 };
  BCPos i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == want) {
      patch.pc = &bc[i];
      patch.original = ins;
      proto_jit_startins_rel(pt, patch.pc, ins);
      bc_publish(patch.pc, BCINS_AD(BC_JLOOP, bc_a(ins), 1));
      assert(proto_jit_startins_acq(pt, patch.pc) == ins);
      return patch;
    }
  }
  assert(!"requested bytecode opcode not found");
  return patch;
}

static PatchedPC patch_first_return(lua_State *L, const char *name)
{
  GCproto *pt = global_proto(L, name);
  BCPos i;
  for (i = 0; i < pt->sizebc; i++) {
    BCOp op = bc_op((BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(pt)[i]));
    if (op == BC_RET || op == BC_RET0 || op == BC_RET1)
      return patch_first_op(L, name, op);
  }
  assert(!"return bytecode opcode not found");
  return patch_first_op(L, name, BC_RET);
}

static void restore_patch(PatchedPC *patch)
{
  assert(patch != NULL && patch->pt != NULL && patch->pc != NULL &&
	 patch->original != 0);
  assert(proto_jit_startins_acq(patch->pt, patch->pc) == patch->original);
  bc_publish(patch->pc, patch->original);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  PatchedPC loop_patch, ret_patch, itern_patch;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "function __sidecar_dump(x) return x * 5 + 2 end\n"
    "__sidecar_loaded = assert(loadstring(string.dump(__sidecar_dump)))\n"
    "function __sidecar_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i end\n"
    "  return i, x\n"
    "end\n"
    "jit.off(__sidecar_loop, true)\n"
    "function __sidecar_ret(x) return x + 7 end\n"
    "jit.off(__sidecar_ret, true)\n"
    "sidecar_input = {11, 22, 33, 44, 55}\n"
    "sidecar_real_next = next\n"
    "function sidecar_next_wrapper(t, k) return sidecar_real_next(t, k) end\n"
    "jit.off(sidecar_next_wrapper, true)\n"
    "function __sidecar_itern(t)\n"
    "  local n, x = 0, 0\n"
    "  for _, v in next, t do n, x = n + 1, x + v end\n"
    "  return n, x\n"
    "end\n"
    "jit.off(__sidecar_itern, true)\n");

  check_fresh_geometry(global_proto(L, "__sidecar_dump"));
  check_fresh_geometry(global_proto(L, "__sidecar_loaded"));
  check_fresh_geometry(global_proto(L, "__sidecar_loop"));
  check_fresh_geometry(global_proto(L, "__sidecar_ret"));
  check_fresh_geometry(global_proto(L, "__sidecar_itern"));

  loop_patch = patch_first_op(L, "__sidecar_loop", BC_LOOP);
  ret_patch = patch_first_return(L, "__sidecar_ret");
  itern_patch = patch_first_op(L, "__sidecar_itern", BC_ITERN);

  lj_trace_test_force_startins_retry(1);
  lua_getglobal(L, "__sidecar_loop");
  lua_pushinteger(L, 1000);
  ljt_lua_pcall(L, 1, 2, "sidecar LOOP recovery");
  assert(lua_tointeger(L, -2) == 1000);
  assert(lua_tonumber(L, -1) == 499500.0);
  lua_pop(L, 2);

  lj_trace_test_force_startins_retry(1);
  lua_getglobal(L, "__sidecar_ret");
  lua_pushinteger(L, 35);
  ljt_lua_pcall(L, 1, 1, "sidecar RET recovery");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  lua_getglobal(L, "sidecar_next_wrapper");
  lua_setglobal(L, "next");
  lj_trace_test_force_startins_retry(1);
  lua_getglobal(L, "__sidecar_itern");
  lua_getglobal(L, "sidecar_input");
  ljt_lua_pcall(L, 1, 2, "sidecar ISNEXT/ITERN recovery");
  assert(lua_tointeger(L, -2) == 5);
  assert(lua_tointeger(L, -1) == 165);
  lua_pop(L, 2);
  assert(bc_op((BCIns)la_load32_acq(
    (const uint32_t *)itern_patch.pc)) == BC_ITERC);

  lua_getglobal(L, "sidecar_real_next");
  lua_setglobal(L, "next");
  restore_patch(&loop_patch);
  restore_patch(&ret_patch);
  restore_patch(&itern_patch);
  lua_close(L);
  puts("t-jit-startins-sidecar OK: immutable opcode recovery and geometry");
  return 0;
}
#else
int main(void)
{
  puts("t-jit-startins-sidecar SKIP: x64 recovery fixture only");
  return 0;
}
#endif
