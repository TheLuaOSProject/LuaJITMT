/* Native lifecycle coverage for ordinary Lua NUM to fixed C integer args. */

#include <assert.h>
#include <errno.h>
#include <math.h>
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

#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-ffi-numeric-args requires ARM64 CALLXS admission"
#endif

enum { NUMERIC_KINDS = 9, RUNS = 24 };

static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;
static void *expected_func;
static void *force_func;
static uint32_t native_finishes;
static uint32_t forced_finishes;
static uint32_t oracle_native_finishes;
static uint64_t old_epoch = UINT64_MAX;
static MSize expected_callback_slot;
static uint8_t expected_callback_stopreq;
static void *expected_ffi_func;

static void capture_cfunc(lua_State *L, const char *name, void **out)
{
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(out, cdataptr(cd), sizeof(*out));
  assert(*out != NULL);
  lua_pop(L, 1);
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

static unsigned trace_op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == op) count++;
  return count;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        trace_op_count(T, IR_CALLXS) != 0 &&
        trace_op_count(T, IR_CALLXS) == trace_op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}

static void observe_finish(TGState *tg)
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
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
}

static void reject_oracle_native(TGState *tg)
{
  UNUSED(tg);
  oracle_native_finishes++;
}

static void finish_or_force(TGState *tg)
{
  uint64_t epoch;
  observe_finish(tg);
  if (lj_ffi_native_frame_func_acq(&tg->ffi_native_frame[0]) != force_func ||
      forced_finishes != 0)
    return;
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  old_epoch = epoch;
  assert(epoch != UINT64_MAX);
  forced_finishes++;
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static int set_errno_caller(lua_State *L)
{
  UNUSED(L);
  errno = E2BIG;
  return 0;
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "numeric CALLXS fixture Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void set_value(lua_State *L, double value)
{
  lua_getglobal(L, "__numeric_table");
  assert(lua_istable(L, -1));
  lua_pushnumber(L, value);
  lua_setfield(L, -2, "value");
  lua_pop(L, 1);
}

static void configure(lua_State *L, unsigned kind)
{
  lua_getglobal(L, "__numeric_configure");
  lua_pushinteger(L, (lua_Integer)kind);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
}

static lua_Integer getter1(lua_State *L, const char *name, lua_Integer arg)
{
  lua_Integer value;
  lua_getglobal(L, name);
  lua_pushinteger(L, arg);
  assert(lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static int call_driver(lua_State *L, const char *runner, const char *func,
                       lua_Integer n)
{
  lua_getglobal(L, "__numeric_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__numeric_table");
  lua_getglobal(L, func);
  lua_pushinteger(L, n);
  return lua_pcall(L, 4, 1, 0);
}

static const char *const runner_names[NUMERIC_KINDS] = {
  "__numeric_run_i8", "__numeric_run_u8", "__numeric_run_i16",
  "__numeric_run_u16", "__numeric_run_i32", "__numeric_run_u32",
  "__numeric_run_i64", "__numeric_run_u64", "__numeric_run_stack"
};

static const char *const function_names[NUMERIC_KINDS] = {
  "__numeric_i8", "__numeric_u8", "__numeric_i16", "__numeric_u16",
  "__numeric_i32", "__numeric_u32", "__numeric_i64", "__numeric_u64",
  "__numeric_stack"
};

static void set_jit(lua_State *L, int enabled)
{
  run_lua(L, enabled ? "jit.on()" : "jit.off()");
}

static void assert_result(lua_State *L, int status, lua_Integer n)
{
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == (double)n * 1.25);
  lua_pop(L, 1);
}

static void save_oracle(lua_State *L, unsigned kind, double value)
{
  const char *oracle = kind == NUMERIC_KINDS-1 ?
                       "__numeric_oracle_stack" : "__numeric_oracle_unary";
  int status;
  set_jit(L, 0);
  run_lua(L, "__numeric_expect_enable(false)");
  configure(L, kind);
  set_value(L, value);
  errno = 0;
  oracle_native_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(reject_oracle_native);
  status = call_driver(L, oracle, function_names[kind], RUNS);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert_result(L, status, RUNS);
  assert(oracle_native_finishes == 0);
  assert(errno == EDOM);
  assert(getter1(L, "__numeric_count", kind) == RUNS);
  assert(getter1(L, "__numeric_first_errno", kind) == E2BIG);
  assert(getter1(L, "__numeric_last_errno", kind) == EDOM);
  run_lua(L, "__numeric_save_expected()");
  run_lua(L, "__numeric_expect_enable(true)");
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
  assert(ccallback_slot_acq(&tg->cb) == expected_callback_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) ==
         expected_callback_stopreq);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(lj_tg_ffi_call_func_acq(tg) == expected_ffi_func);
  assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
    assert(lj_ffi_native_frame_old_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
    assert(lj_ffi_native_frame_trace_no_acq(frame) == 0);
  }
}

static void assert_capture_match(lua_State *L)
{
  assert(getter1(L, "__numeric_match_expected", 0) == 1);
  assert(getter1(L, "__numeric_mismatch_count", 0) == 0);
}

static const BCIns *runner_call_pc(GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  const BCIns *found = NULL;
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALL) {
      assert(found == NULL);
      found = &bc[i];
    }
  }
  assert(found != NULL);
  return found;
}

static void assert_postcall_exit(jit_State *J, GCtrace *T, GCproto *pt)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  GCtrace *exited = traceref_safe(J, parent);
  const SnapShot *snap;
  const BCIns *callpc = runner_call_pc(pt), *pc;
  MSize ofs, nent;
  uint64_t pcbase;
  assert(exited == T && exitno < trace_nsnap_acq(T));
  snap = &trace_snap_acq(T)[exitno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         nent + 1u + LJ_FR2 <= trace_nsnapmap_acq(T) - ofs);
  memcpy(&pcbase, &trace_snapmap_acq(T)[ofs + nent], sizeof(pcbase));
  pc = (const BCIns *)(uintptr_t)(pcbase >> 8);
  assert((uint8_t)pcbase == 0 && pc == callpc + 1 &&
         bc_op((BCIns)la_load32_acq((const uint32_t *)callpc)) == BC_CALL);
}

static GCtrace *run_native(lua_State *L, jit_State *J, TGState *tg,
                           unsigned kind, double value, GCtrace *reuse,
                           int force)
{
  GCtrace *T;
  GCproto *pt;
  int status, saved_errno;
  configure(L, kind);
  set_value(L, value);
  capture_cfunc(L, function_names[kind], &expected_func);
  expected_L = L;
  expected_tg = tg;
  expected_trace = reuse;
  native_finishes = 0;
  force_func = force ? expected_func : NULL;
  forced_finishes = 0;
  old_epoch = UINT64_MAX;
  pt = global_proto(L, runner_names[kind]);
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force ? finish_or_force :
                                                       observe_finish);
  errno = 0;
  status = call_driver(L, runner_names[kind], function_names[kind], RUNS);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert_result(L, status, RUNS);
  assert(saved_errno == EDOM);
  T = find_trace(J, pt);
  if (T == NULL || native_finishes == 0 || expected_trace != T)
    fprintf(stderr, "numeric CALLXS missing: kind=%u value=%.17g native=%u "
            "trace=%p observed=%p\n", kind, value, native_finishes,
            (void *)T, (void *)expected_trace);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  assert(bc_op(trace_startins_acq(T)) == BC_FORL);
  assert(getter1(L, "__numeric_count", kind) == RUNS);
  assert(getter1(L, "__numeric_first_errno", kind) == E2BIG);
  assert(getter1(L, "__numeric_last_errno", kind) == EDOM);
  assert_capture_match(L);
  if (force) {
    assert(forced_finishes == 1 && old_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() > 0);
    assert_postcall_exit(J, T, pt);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
  }
  assert_clean(tg, T);
  return T;
}

static void exercise_kind(lua_State *L, jit_State *J, TGState *tg,
                          unsigned kind, const double *values, size_t nvalues)
{
  GCtrace *T = NULL;
  size_t i;
  const double base = -3.75;
  /* Capture the interpreter's exact typed arguments first, then compare every
  ** native case against those raw argument bits (including NaN/overflow). */
  set_jit(L, 0);
  save_oracle(L, kind, base);
  set_jit(L, 1);
  T = run_native(L, J, tg, kind, base, NULL, 0);
  for (i = 0; i < nvalues; i++) {
    save_oracle(L, kind, values[i]);
    set_jit(L, 1);
    T = run_native(L, J, tg, kind, values[i], T, 0);
  }
  save_oracle(L, kind, base);
  set_jit(L, 1);
  T = run_native(L, J, tg, kind, base, T, 1);
  run_native(L, J, tg, kind, base, T, 0);
}

int main(void)
{
  static const double values8[] = {
    0.75, -0.75, -0.0, 127.75, 128.75, -128.75, -129.75,
    255.75, 256.75, -1.75, 1.0e300, -1.0e300, NAN, INFINITY, -INFINITY
  };
  static const double values16[] = {
    0.75, -0.75, -0.0, 32767.75, 32768.75, -32768.75, -32769.75,
    65535.75, 65536.75, -1.75, 1.0e300, -1.0e300, NAN, INFINITY, -INFINITY
  };
  static const double values32[] = {
    0.75, -0.75, -0.0, 2147483647.75, 2147483648.75,
    -2147483648.75, -2147483649.75, 4294967295.75, 4294967296.0,
    -1.75, 1.0e300, -1.0e300, NAN, INFINITY, -INFINITY
  };
  static const double values64[] = {
    0.75, -0.75, -0.0, 0x1p63, -0x1p63, 0x1.fffffffffffffp62,
    -0x1.fffffffffffffp62, 0x1.fffffffffffffp63, 0x1p64,
    -1.75, 1.0e300, -1.0e300, NAN, INFINITY, -INFINITY
  };
  const double *value_sets[NUMERIC_KINDS] = {
    values8, values8, values16, values16, values32, values32,
    values64, values64, values8
  };
  const size_t value_counts[NUMERIC_KINDS] = {
    sizeof(values8)/sizeof(values8[0]), sizeof(values8)/sizeof(values8[0]),
    sizeof(values16)/sizeof(values16[0]), sizeof(values16)/sizeof(values16[0]),
    sizeof(values32)/sizeof(values32[0]), sizeof(values32)/sizeof(values32[0]),
    sizeof(values64)/sizeof(values64[0]), sizeof(values64)/sizeof(values64[0]),
    sizeof(values8)/sizeof(values8[0])
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  unsigned kind;
  expected_callback_slot = ccallback_slot_acq(&tg->cb);
  expected_callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  expected_ffi_func = lj_tg_ffi_call_func_acq(tg);
  expected_L = L;
  expected_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__numeric_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_ffi_numeric_configure(int32_t);\n"
    "int32_t lj_ffi_numeric_count(int32_t);\n"
    "int32_t lj_ffi_numeric_first_errno(int32_t);\n"
    "int32_t lj_ffi_numeric_last_errno(int32_t);\n"
    "uint32_t lj_ffi_numeric_capture_low(uint32_t);\n"
    "uint32_t lj_ffi_numeric_capture_high(uint32_t);\n"
    "uint32_t lj_ffi_numeric_expected_low(uint32_t);\n"
    "uint32_t lj_ffi_numeric_expected_high(uint32_t);\n"
    "void lj_ffi_numeric_save_expected(void);\n"
    "void lj_ffi_numeric_expect_enable(int32_t);\n"
    "int32_t lj_ffi_numeric_mismatch_count(void);\n"
    "int32_t lj_ffi_numeric_match_expected(void);\n"
    "double lj_ffi_numeric_i8(int8_t);\n"
    "double lj_ffi_numeric_u8(uint8_t);\n"
    "double lj_ffi_numeric_i16(int16_t);\n"
    "double lj_ffi_numeric_u16(uint16_t);\n"
    "double lj_ffi_numeric_i32(int32_t);\n"
    "double lj_ffi_numeric_u32(uint32_t);\n"
    "double lj_ffi_numeric_i64(int64_t);\n"
    "double lj_ffi_numeric_u64(uint64_t);\n"
    "double lj_ffi_numeric_stack(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int8_t,uint8_t,int16_t,uint16_t,int32_t,uint32_t,int64_t,uint64_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_NUMERIC_ARGS_SO')))\n"
    "_G.__numeric_i8=lib.lj_ffi_numeric_i8; _G.__numeric_u8=lib.lj_ffi_numeric_u8\n"
    "_G.__numeric_i16=lib.lj_ffi_numeric_i16; _G.__numeric_u16=lib.lj_ffi_numeric_u16\n"
    "_G.__numeric_i32=lib.lj_ffi_numeric_i32; _G.__numeric_u32=lib.lj_ffi_numeric_u32\n"
    "_G.__numeric_i64=lib.lj_ffi_numeric_i64; _G.__numeric_u64=lib.lj_ffi_numeric_u64\n"
    "_G.__numeric_stack=lib.lj_ffi_numeric_stack\n"
    "function _G.__numeric_configure(k) lib.lj_ffi_numeric_configure(k) end\n"
    "function _G.__numeric_count(k) return lib.lj_ffi_numeric_count(k) end\n"
    "function _G.__numeric_first_errno(k) return lib.lj_ffi_numeric_first_errno(k) end\n"
    "function _G.__numeric_last_errno(k) return lib.lj_ffi_numeric_last_errno(k) end\n"
    "function _G.__numeric_save_expected() lib.lj_ffi_numeric_save_expected() end\n"
    "function _G.__numeric_expect_enable(v) lib.lj_ffi_numeric_expect_enable(v and 1 or 0) end\n"
    "function _G.__numeric_mismatch_count() return lib.lj_ffi_numeric_mismatch_count() end\n"
    "function _G.__numeric_match_expected() return lib.lj_ffi_numeric_match_expected() end\n"
    "function _G.__numeric_capture(i) return lib.lj_ffi_numeric_capture_low(i),lib.lj_ffi_numeric_capture_high(i) end\n"
    "function _G.__numeric_expected(i) return lib.lj_ffi_numeric_expected_low(i),lib.lj_ffi_numeric_expected_high(i) end\n"
    "_G.__numeric_table={value=-3.75}\n"
    "function _G.__numeric_run_i8(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_u8(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i16(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_u16(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i32(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_u32(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i64(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_u64(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_stack(t,fn,n) local s=.5; for i=1,n do s=s+fn(101,102,103,104,105,106,107,108,t.value,t.value,t.value,t.value,t.value,t.value,t.value,t.value) end; return s-.5 end\n"
    "local function oracle_unary(t,fn,n) local s=.5; for i=1,n do s=s+fn(t.value) end; return s-.5 end\n"
    "local function oracle_stack(t,fn,n) local s=.5; for i=1,n do s=s+fn(101,102,103,104,105,106,107,108,t.value,t.value,t.value,t.value,t.value,t.value,t.value,t.value) end; return s-.5 end\n"
    "jit.off(oracle_unary,true); jit.off(oracle_stack,true)\n"
    "_G.__numeric_oracle_unary=oracle_unary; _G.__numeric_oracle_stack=oracle_stack\n"
    "function _G.__numeric_driver(run,t,fn,n) __numeric_set_errno(); local r=run(t,fn,n); return r end\n"
    "local threading=require('threading'); _G.__numeric_ready=threading.channel(1); _G.__numeric_release=threading.channel(1)\n"
    "_G.__numeric_peer=assert(threading.spawn(function(ready,release) assert(ready:send(true,5)); assert(select(2,release:recv(120))); return true end,__numeric_ready,__numeric_release))\n"
    "assert(select(2,__numeric_ready:recv(5)))\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");
  assert(mt_active_acq(g) != 0);
  set_jit(L, 0);
  for (kind = 0; kind < NUMERIC_KINDS; kind++)
    exercise_kind(L, J, tg, kind, value_sets[kind], value_counts[kind]);
  run_lua(L, "assert(__numeric_release:send(true,5)); local ok,v=__numeric_peer:join(5); assert(ok and v)");
  assert(mt_active_acq(g) != 0);
  lua_close(L);
  puts("t-arm64-jit-ffi-numeric-args OK: interpreter/native numeric-to-integer "
       "arguments, stack layout, native frames, postcall exits and cleanup");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-ffi-numeric-args SKIP: requires experimental Darwin "
       "ARM64 CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
