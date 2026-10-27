/* Native ARM64 CALLXS through cached pointer-to-function CData. */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-funcptr requires ARM64 CALLXS admission"
#endif

static lua_State *expected_L;
static TGState *expected_tg;
static GCtrace *expected_trace;
static void *expected_func;
static uint32_t native_finishes;

static void assert_clean(TGState *tg, GCtrace *T);

static void *global_cfunc(lua_State *L, const char *name)
{
  void *func;
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(&func, cdataptr(cd), sizeof(func));
  lua_pop(L, 1);
  assert(func != NULL);
  return func;
}

static int set_errno_caller(lua_State *L)
{
  UNUSED(L);
  errno = E2BIG;
  return 0;
}

static unsigned op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == op)
      count++;
  return count;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

static GCtrace *find_loop_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) != 0 &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}

static int proto_has_op(GCproto *pt, BCOp wanted)
{
  const BCIns *bc = proto_bc(pt);
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == wanted)
      return 1;
  }
  return 0;
}

static void observe_native_finish(TGState *tg)
{
  const LJFFINativeFrame *frame;
  assert(tg == expected_tg && expected_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) &
          (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
           LJ_FFI_NATIVE_FRAME_F_POSTCALL)) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func);
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "function-pointer CALLXS Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer lua_counter(lua_State *L, const char *name)
{
  lua_Integer result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static GCtrace *run_case(lua_State *L, jit_State *J, TGState *tg,
                         const char *runner, int key_kind,
                         lua_Integer key, lua_Integer n, double expected,
                         void *func, const char *count_name, BCOp read_op)
{
  GCtrace *T;
  int status;
  expected_trace = NULL;
  expected_func = func;
  native_finishes = 0;
  run_lua(L, "jit.flush()");
  run_lua(L, "__funcptr_configure()");
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  errno = 0;
  lua_getglobal(L, "__funcptr_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__funcptr_registry");
  if (key_kind == 2)
    lua_pushboolean(L, key != 0);
  else if (key_kind == 1)
    lua_pushinteger(L, key);
  else
    lua_pushlstring(L, "zero", 4);
  lua_pushinteger(L, n);
  status = lua_pcall(L, 4, 1, 0);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  assert(errno == EDOM);
  if (native_finishes == 0)
    fprintf(stderr, "function-pointer CALLXS native entry missing: %s\n", runner);
  assert(native_finishes != 0);
  assert(lua_counter(L, count_name) == n);
  {
    GCproto *pt = global_proto(L, runner);
    assert(proto_has_op(pt, read_op));
    T = find_loop_trace(J, pt);
  }
  assert(T != NULL && T == expected_trace);
  assert_clean(tg, T);
  return T;
}

static void assert_clean(TGState *tg, GCtrace *T)
{
  uint32_t i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
         tg->ffi_xsave_nslots == 0);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  TGState *tg = L2TG(L);
  const lua_Integer n = 32;
  void *zero_func, *vararg_func;
  double vararg_expected = 0.0;
  lua_Integer i;

  expected_L = L;
  expected_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__funcptr_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_funcptr_configure(void);\n"
    "int32_t lj_callxs_funcptr_zero_count(void);\n"
    "int32_t lj_callxs_funcptr_vararg_count(void);\n"
    "int32_t lj_callxs_funcptr_zero(void);\n"
    "int32_t lj_callxs_funcptr_vararg(int32_t, ...);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_FUNCPTR_SO')))\n"
    "_G.__funcptr_library=lib\n"
    "local ready,stop=threading.channel(1),threading.channel(1)\n"
    "_G.__funcptr_stop=stop\n"
    "_G.__funcptr_peer=threading.spawn(function(r,s)\n"
    " assert(r:send(true,5)); local _,ok=s:recv(30); assert(ok); return true\n"
    "end,ready,stop)\n"
    "assert(select(2,ready:recv(5)))\n"
    "local zero_type=ffi.typeof('int32_t (*)(void)')\n"
    "local vararg_type=ffi.typeof('int32_t (*)(int32_t, ...)')\n"
    "local zero=ffi.cast(zero_type,lib.lj_callxs_funcptr_zero)\n"
    "local vararg=ffi.cast(vararg_type,lib.lj_callxs_funcptr_vararg)\n"
    "_G.__funcptr_zero=zero; _G.__funcptr_vararg=vararg\n"
    "_G.__funcptr_zero_count=lib.lj_callxs_funcptr_zero_count\n"
    "_G.__funcptr_vararg_count=lib.lj_callxs_funcptr_vararg_count\n"
    "_G.__funcptr_configure=lib.lj_callxs_funcptr_configure\n"
    "_G.__funcptr_registry={zero=zero,[1]=zero,[true]=zero,[false]=zero,call=vararg}\n"
    "function _G.__funcptr_run_string(reg,key,n)\n"
    " local sum=0.5; for i=1,n do local fn=reg[key]; sum=sum+fn() end\n"
    " return sum-0.5 end\n"
    "function _G.__funcptr_run_index(reg,key,n)\n"
    " local sum=0.5; for i=1,n do local fn=reg[key]; sum=sum+fn() end\n"
    " return sum-0.5 end\n"
    "function _G.__funcptr_run_byte(reg,key,n)\n"
    " local sum=0.5; for i=1,n do local fn=reg[1]; sum=sum+fn() end\n"
    " return sum-0.5 end\n"
    "function _G.__funcptr_run_boolean(reg,key,n)\n"
    " local sum=0.5; for i=1,n do local fn=reg[key]; sum=sum+fn() end\n"
    " return sum-0.5 end\n"
    "function _G.__funcptr_run_tgets(reg,key,n)\n"
    " local sum=0.5; for i=1,n do\n"
    "   sum=sum+reg.call(i,i+0.5,true,'abc')\n"
    " end; return sum-0.5 end\n"
    "local set_errno=__funcptr_set_errno\n"
    "function _G.__funcptr_driver(run,reg,key,n)\n"
    " set_errno(); local result=run(reg,key,n); return result end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')\n");

  run_lua(L, "if os.getenv('LJ_ARM64_TABLE_CALL_DUMP') then "
             "require('jit.dump').on('isaT') end");
  zero_func = global_cfunc(L, "__funcptr_zero");
  vararg_func = global_cfunc(L, "__funcptr_vararg");
  run_case(L, J, tg, "__funcptr_run_string", 0, 0, n,
           7.0 * (double)n, zero_func, "__funcptr_zero_count", BC_TGETV);
  run_case(L, J, tg, "__funcptr_run_index", 1, 1, n,
           7.0 * (double)n, zero_func, "__funcptr_zero_count", BC_TGETV);
  run_case(L, J, tg, "__funcptr_run_byte", 1, 1, n,
           7.0 * (double)n, zero_func, "__funcptr_zero_count", BC_TGETB);
  run_case(L, J, tg, "__funcptr_run_boolean", 2, 1, n,
           7.0 * (double)n, zero_func, "__funcptr_zero_count", BC_TGETV);
  run_case(L, J, tg, "__funcptr_run_boolean", 2, 0, n,
           7.0 * (double)n, zero_func, "__funcptr_zero_count", BC_TGETV);
  for (i = 1; i <= n; i++)
    vararg_expected += (double)(2 * i + 4);
  run_case(L, J, tg, "__funcptr_run_tgets", 0, 0, n,
           vararg_expected, vararg_func, "__funcptr_vararg_count", BC_TGETS);

  run_lua(L, "assert(__funcptr_stop:send(true,5)); "
             "local ok,result=__funcptr_peer:join(5); assert(ok and result)");

  expected_L = NULL;
  expected_tg = NULL;
  lua_close(L);
  puts("t-arm64-jit-callxs-funcptr OK: cached function-pointer CData, "
       "TGETV string/integer/boolean, TGETB, TGETS variadic CALLXS, errno and "
       "native-frame cleanup verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-funcptr SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
