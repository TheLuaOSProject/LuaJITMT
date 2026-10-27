/* Native lifecycle coverage for fixed uint32_t CALLXS numeric results. */

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

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-u32 requires ARM64 CALLXS admission"
#endif

static uint32_t native_finishes;
static uint32_t total_native_finishes;
static uint64_t old_epoch;
static uint32_t pair_case;
static void *expected_func;
static void *expected_alt_func;
static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;
static MSize expected_callback_slot;
static uint8_t expected_callback_stopreq;
static void *expected_ffi_func;

static const uint32_t values[] = {
  UINT32_C(0), UINT32_C(1), UINT32_C(0x7fffffff), UINT32_C(0x80000000),
  UINT32_C(0xffffffff), UINT32_C(0xaaaaaaaa), UINT32_C(0x55555555),
  UINT32_C(0xdeadbeef), UINT32_C(0x01234567), UINT32_C(0xfedcba98)
};

static uint32_t value_for(uint32_t mode, uint32_t selector, uint32_t offset)
{
  return values[(mode + selector + offset) %
                (uint32_t)(sizeof(values) / sizeof(values[0]))];
}

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

static void observe_finish(TGState *tg)
{
  const LJFFINativeFrame *frame;
  void *func;
  assert(tg == expected_tg && expected_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) &
          (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
           LJ_FFI_NATIVE_FRAME_F_POSTCALL)) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  func = lj_ffi_native_frame_func_acq(frame);
  if (pair_case)
    assert(func == expected_func || func == expected_alt_func);
  else
    assert(func == expected_func);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
  total_native_finishes++;
}

static void force_postcall_epoch(TGState *tg)
{
  uint64_t epoch;
  observe_finish(tg);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (native_finishes == 1u)
    old_epoch = epoch;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static int set_errno_caller(lua_State *L)
{
  (void)L;
  errno = E2BIG;
  return 0;
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

static GCtrace *find_callxs_trace(jit_State *J, GCproto *pt,
                                  unsigned callxs_count)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_XSAVE) == 2u * callxs_count &&
        op_count(T, IR_CALLXS) == 2u * callxs_count)
      return T;
  }
  return NULL;
}

static int trace_has_checked_intnum(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_CONV && ins.t.irt == (IRT_INT | IRT_GUARD) &&
        ins.op2 == (IRCONV_INT_NUM | IRCONV_CHECK) && ins.op1 < ref &&
        irt_type(ir_load_acq(&ir[ins.op1]).t) == IRT_NUM)
      return 1;
  }
  return 0;
}

static lua_Integer getter(lua_State *L, const char *name)
{
  lua_Integer result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0 && lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static int run_driver(lua_State *L, const char *runner,
                      const char *fn, lua_Integer n)
{
  lua_getglobal(L, "__u32_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, fn);
  lua_pushinteger(L, n);
  return lua_pcall(L, 3, 1, 0);
}

static int direct_call(lua_State *L, const char *fn, int32_t selector)
{
  lua_getglobal(L, "__u32_direct");
  lua_getglobal(L, fn);
  lua_pushinteger(L, selector);
  return lua_pcall(L, 2, 1, 0);
}

static void configure(lua_State *L, uint32_t mode)
{
  lua_getglobal(L, "__u32_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static double single_sum(uint32_t mode, uint32_t n)
{
  double sum = 0;
  uint32_t i;
  for (i = 1; i <= n; i++) sum += (double)value_for(mode, i, 0);
  return sum;
}

static double pair_sum(uint32_t mode, uint32_t n)
{
  double sum = 0;
  uint32_t i;
  double first = 0, second = 0;
  for (i = 1; i <= n; i++) {
    first += (double)value_for(mode, i, 0);
    second += (double)value_for(mode, i, 3);
  }
  sum = 2.0 * first + 5.0 * second;
  return sum;
}

static void assert_clean_native(TGState *tg, GCtrace *T)
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

static GCtrace *check_run(lua_State *L, jit_State *J, TGState *tg,
                          const char *runner, const char *fn, int paired,
                          int is_vararg,
                          uint32_t mode, uint32_t n, int force_epoch)
{
  GCtrace *T;
  GCproto *pt;
  int status, observed_errno;
  configure(L, mode);
  capture_cfunc(L, fn, &expected_func);
  if (paired)
    capture_cfunc(L, "__u32_alt_call", &expected_alt_func);
  pair_case = (uint32_t)paired;
  expected_tg = tg;
  expected_L = L;
  expected_trace = NULL;
  native_finishes = 0;
  old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
      force_postcall_epoch : observe_finish);
  errno = 0;
  status = run_driver(L, runner, fn, n);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == (paired ? pair_sum(mode, n) :
                                             single_sum(mode, n)));
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  pt = global_proto(L, runner);
  T = find_callxs_trace(J, pt, paired ? 2u : 1u);
  if (T == NULL || expected_trace != T)
    fprintf(stderr, "U32 CALLXS trace mismatch: %s mode=%u native=%u "
            "published=%p observed=%p\n", runner, (unsigned)mode,
            (unsigned)native_finishes, (void *)T, (void *)expected_trace);
  assert(T != NULL && expected_trace == T);
  assert(native_finishes != 0);
  assert(getter(L, is_vararg ? "__u32_vararg_count" : "__u32_count") ==
         (lua_Integer)n);
  if (paired)
    assert(getter(L, "__u32_alt_count") == (lua_Integer)n);
  assert(getter(L, "__u32_total_count") ==
         (lua_Integer)n * (paired ? 2 : 1));
  assert(getter(L, "__u32_first_errno") == E2BIG);
  assert(getter(L, "__u32_last_errno") == EDOM);
  if (force_epoch) {
    assert(old_epoch != UINT64_MAX);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert(lj_trace_test_exit_calls() == native_finishes);
  }
  assert_clean_native(tg, T);
  return T;
}

static GCtrace *check_retained(lua_State *L, jit_State *J, TGState *tg,
                               uint32_t mode, uint32_t n, int force_epoch)
{
  const char *runner = "__u32_retain_run";
  GCtrace *T;
  int status, observed_errno;
  configure(L, mode);
  capture_cfunc(L, "__u32_call", &expected_func);
  pair_case = 0;
  expected_tg = tg;
  expected_L = L;
  expected_trace = NULL;
  native_finishes = 0;
  old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
      force_postcall_epoch : observe_finish);
  errno = 0;
  status = run_driver(L, runner, "__u32_call", n);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == (lua_Number)value_for(mode, 0, 0));
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  T = find_callxs_trace(J, global_proto(L, runner), 1);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  assert(getter(L, "__u32_count") == (lua_Integer)n);
  assert(getter(L, "__u32_total_count") == (lua_Integer)n);
  assert(getter(L, "__u32_first_errno") == E2BIG);
  assert(getter(L, "__u32_last_errno") == EDOM);
  if (force_epoch) {
    assert(old_epoch != UINT64_MAX);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert(lj_trace_test_exit_calls() == native_finishes);
  }
  assert_clean_native(tg, T);
  return T;
}

static GCtrace *check_discarded(lua_State *L, jit_State *J, TGState *tg,
                                uint32_t mode, uint32_t n, int force_epoch)
{
  const char *runner = "__u32_discard_run";
  GCtrace *T;
  int status, observed_errno;
  configure(L, mode);
  capture_cfunc(L, "__u32_call", &expected_func);
  pair_case = 0;
  expected_tg = tg;
  expected_L = L;
  expected_trace = NULL;
  native_finishes = 0;
  old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
      force_postcall_epoch : observe_finish);
  errno = 0;
  status = run_driver(L, runner, "__u32_call", n);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_tointeger(L, -1) == (lua_Integer)n);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  T = find_callxs_trace(J, global_proto(L, runner), 1);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  assert(getter(L, "__u32_count") == (lua_Integer)n);
  assert(getter(L, "__u32_total_count") == (lua_Integer)n);
  assert(getter(L, "__u32_first_errno") == E2BIG);
  assert(getter(L, "__u32_last_errno") == EDOM);
  if (force_epoch) {
    assert(old_epoch != UINT64_MAX);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert(lj_trace_test_exit_calls() == native_finishes);
  }
  assert_clean_native(tg, T);
  return T;
}

static GCtrace *check_threshold_exit(lua_State *L, jit_State *J, TGState *tg,
                                     uint32_t n)
{
  const char *runner = "__u32_threshold_run";
  GCtrace *T;
  double expected = 4.0 + (double)(n - 4u) * (double)UINT32_MAX;
  int status, observed_errno;
  configure(L, 0);
  capture_cfunc(L, "__u32_threshold_call", &expected_func);
  pair_case = 0;
  expected_tg = tg;
  expected_L = L;
  expected_trace = NULL;
  native_finishes = 0;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = run_driver(L, runner, "__u32_threshold_call", n);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  T = find_callxs_trace(J, global_proto(L, runner), 1);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  assert(trace_has_checked_intnum(T));
  assert(lj_trace_test_exit_calls() != 0);
  assert(getter(L, "__u32_count") == (lua_Integer)n);
  assert(getter(L, "__u32_total_count") == (lua_Integer)n);
  assert(getter(L, "__u32_first_errno") == E2BIG);
  assert(getter(L, "__u32_last_errno") == EDOM);
  assert_clean_native(tg, T);
  return T;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  expected_callback_slot = ccallback_slot_acq(&tg->cb);
  expected_callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  expected_ffi_func = lj_tg_ffi_call_func_acq(tg);
  GCtrace *T;
  uint32_t mode;

  expected_tg = tg;
  expected_L = L;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__u32_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef uint32_t lj_test_u32_alias;\n"
    "void lj_callxs_arm64_u32_configure(int32_t);\n"
    "int32_t lj_callxs_arm64_u32_count(void);\n"
    "int32_t lj_callxs_arm64_u32_alt_count(void);\n"
    "int32_t lj_callxs_arm64_u32_vararg_count(void);\n"
    "int32_t lj_callxs_arm64_u32_total_count(void);\n"
    "int32_t lj_callxs_arm64_u32_first_errno(void);\n"
    "int32_t lj_callxs_arm64_u32_last_errno(void);\n"
    "uint32_t lj_callxs_arm64_u32(int32_t);\n"
    "uint32_t lj_callxs_arm64_u32_threshold(int32_t);\n"
    "uint32_t lj_callxs_arm64_u32_alt(int32_t);\n"
    "uint32_t lj_callxs_arm64_u32_vararg(int32_t, ...);\n"
    "lj_test_u32_alias lj_callxs_arm64_u32_alias(int32_t);\n"
    "const uint32_t lj_callxs_arm64_u32_const(int32_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_U32_SO')))\n"
    "local set_errno=__u32_set_errno\n"
    "_G.__u32_call=lib.lj_callxs_arm64_u32\n"
    "_G.__u32_threshold_call=lib.lj_callxs_arm64_u32_threshold\n"
    "_G.__u32_alt_call=lib.lj_callxs_arm64_u32_alt\n"
    "_G.__u32_vararg_call=lib.lj_callxs_arm64_u32_vararg\n"
    "_G.__u32_alias_call=lib.lj_callxs_arm64_u32_alias\n"
    "_G.__u32_const_call=lib.lj_callxs_arm64_u32_const\n"
    "function _G.__u32_configure(mode) lib.lj_callxs_arm64_u32_configure(mode) end\n"
    "function _G.__u32_count() return lib.lj_callxs_arm64_u32_count() end\n"
    "function _G.__u32_alt_count() return lib.lj_callxs_arm64_u32_alt_count() end\n"
    "function _G.__u32_vararg_count() return lib.lj_callxs_arm64_u32_vararg_count() end\n"
    "function _G.__u32_total_count() return lib.lj_callxs_arm64_u32_total_count() end\n"
    "function _G.__u32_first_errno() return lib.lj_callxs_arm64_u32_first_errno() end\n"
    "function _G.__u32_last_errno() return lib.lj_callxs_arm64_u32_last_errno() end\n"
    "function _G.__u32_run(fn,n) local sum=0.5; for i=1,n do sum=sum+fn(i) end; return sum-0.5 end\n"
    "function _G.__u32_retain_run(fn,n) local value=0.5; for i=1,n do value=fn(0) end; return value end\n"
    "function _G.__u32_vararg_run(fn,n) local sum=0.5; for i=1,n do sum=sum+fn(i,i+0.5,true,'u32-vararg') end; return sum-0.5 end\n"
    "function _G.__u32_pair_run(fn,n) local alt=__u32_alt_call; local a,b=0.5,0.5; for i=1,n do a=a+fn(i); b=b+alt(i) end; return 2*a+5*b-3.5 end\n"
    "function _G.__u32_alias_run(fn,n) local sum=0.5; for i=1,n do sum=sum+fn(i) end; return sum-0.5 end\n"
    "function _G.__u32_const_run(fn,n) local sum=0.5; for i=1,n do sum=sum+fn(i) end; return sum-0.5 end\n"
    "function _G.__u32_discard_run(fn,n) for i=1,n do fn(i) end; return n end\n"
    "function _G.__u32_threshold_run(fn,n) local sum=0; for i=1,n do sum=sum+fn(i) end; return sum end\n"
    "function _G.__u32_direct(fn,selector) set_errno(); return fn(selector) end\n"
    "jit.off(_G.__u32_direct)\n"
    "function _G.__u32_driver(run,fn,n) set_errno(); local result=run(fn,n); return result end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  /* Verify each requested boundary/high-bit pattern as an individual Lua
  ** number before the generated loop aggregates changing values. */
  for (mode = 0; mode < 10; mode++) {
    configure(L, mode);
    errno = 0;
    assert(direct_call(L, "__u32_call", 0) == 0);
    assert(lua_isnumber(L, -1));
    assert(lua_tonumber(L, -1) == (lua_Number)value_for(mode, 0, 0));
    lua_pop(L, 1);
    assert(errno == EDOM && getter(L, "__u32_count") == 1);
  }

  for (mode = 0; mode < 10; mode++) {
    T = check_run(L, J, tg, "__u32_run", "__u32_call", 0, 0,
                  mode, 32u + mode, 0);
    /* Exercise the live return PHI itself, not only an accumulated sum. */
    T = check_retained(L, J, tg, mode, 32u + mode, 0);
  }
  /* The same native trace carries two different uint32 CALLXS producers;
  ** per-call selectors ensure the result sources cannot be interchanged. */
  T = check_run(L, J, tg, "__u32_pair_run", "__u32_call", 1, 0,
                4, 37, 0);
  /* The U32 result composes with the already-certified promoted suffix. */
  T = check_run(L, J, tg, "__u32_vararg_run", "__u32_vararg_call", 0, 1,
                2, 31, 0);
  /* Retained and discarded results both cross a forced POSTCALL exit. */
  T = check_retained(L, J, tg, 8, 24, 1);
  assert(trace_runnable_acq(T, trace_traceno_acq(T)));
  T = check_discarded(L, J, tg, 6, 40, 1);
  /* A low uint32 result first satisfies the recorder's integerness guard;
  ** the high-bit result later exits after its completed call, without replay. */
  T = check_threshold_exit(L, J, tg, 16);

  /* Alias/const declarations resolve to the same scalar-sized Lua numeric
  ** result while preserving their ordinary C ABI declarations. */
  T = check_run(L, J, tg, "__u32_alias_run", "__u32_alias_call", 0, 0,
                3, 24, 0);
  T = check_run(L, J, tg, "__u32_const_run", "__u32_const_call", 0, 0,
                5, 24, 0);

  expected_func = NULL;
  expected_alt_func = NULL;
  expected_L = NULL;
  expected_tg = NULL;
  (void)T;
  lua_close(L);
  printf("U32 native calls observed=%u\n", (unsigned)total_native_finishes);
  puts("t-arm64-jit-callxs-u32 OK: native uint32_t numeric results, "
       "high-bit patterns, multi-call identity, POSTCALL no-replay, "
       "errno, and frame cleanup verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-u32 SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
