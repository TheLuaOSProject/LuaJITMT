/*
** Native lifecycle proof for the exact Darwin ARM64 double(double) CALLXS
** root. Every foreign effect is counted so no post-call exit can replay it.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

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
#error "t-arm64-jit-callxs-double-lifecycle requires ARM64 CALLXS admission"
#endif

static uint32_t finish_calls;
static uint64_t finish_old_epoch;

static void force_epoch_each(TGState *tg)
{
  uint64_t epoch;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (finish_calls == 0)
    finish_old_epoch = epoch;
  finish_calls++;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static void observe_finish(TGState *tg)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  finish_calls++;
}

static int set_caller_errno(lua_State *L)
{
  (void)L;
  errno = E2BIG;
  return 0;
}

static GCproto *global_lua_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
}

static unsigned trace_op_count(const GCtrace *T, IROp wanted)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == wanted)
      count++;
  return count;
}

static GCtrace *find_callxs_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (trace_runnable_acq(T, traceno) && trace_root_acq(T) == 0 &&
	trace_startpt_acq(T) == pt &&
	bc_op(trace_startins_acq(T)) == BC_FORL &&
	trace_op_count(T, IR_XSAVE) == 2 &&
	trace_op_count(T, IR_CALLXS) == 2)
      return T;
  }
  return NULL;
}

static void call_configure(lua_State *L, int32_t mismatch_call)
{
  lua_getglobal(L, "__callxs_double_lifecycle_configure");
  lua_pushinteger(L, mismatch_call);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static lua_Integer call_integer_getter(lua_State *L, const char *name)
{
  lua_Integer result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static lua_Number call_number_getter(lua_State *L, const char *name)
{
  lua_Number result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return result;
}

static int start_run(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__callxs_double_lifecycle_driver");
  lua_getglobal(L, "__callxs_double_lifecycle_call");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
}

static void assert_empty_frame_storage(TGState *tg)
{
  uint32_t i;
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

static void assert_clean_native_state(TGState *tg, GCtrace *T,
	MSize callback_slot, void *ffi_func, uint8_t callback_stopreq)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == callback_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == callback_stopreq);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(lj_tg_ffi_call_func_acq(tg) == ffi_func);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL);
  assert(tg->ffi_xsave_baseslot == 0);
  assert(tg->ffi_xsave_nslots == 0);
  assert(trace_native_pins_acq(T) == 0);
  assert_empty_frame_storage(tg);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  TGState *tg = L2TG(L);
  MSize old_callback_slot = ccallback_slot_acq(&tg->cb);
  void *old_ffi_func = lj_tg_ffi_call_func_acq(tg);
  uint8_t old_callback_stopreq =
    ccallback_native_had_stopreq_acq(&tg->cb);
  GCproto *pt;
  GCtrace *T;
  int observed_errno, status;

  lua_pushcfunction(L, set_caller_errno);
  lua_setglobal(L, "__callxs_double_lifecycle_set_caller_errno");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_arm64_double_lifecycle_configure(int32_t);\n"
    "int32_t lj_callxs_arm64_double_lifecycle_count(void);\n"
    "int32_t lj_callxs_arm64_double_lifecycle_first_errno_value(void);\n"
    "int32_t lj_callxs_arm64_double_lifecycle_last_errno_value(void);\n"
    "double lj_callxs_arm64_double_lifecycle_last_input_value(void);\n"
    "double lj_callxs_arm64_double_lifecycle_last_result_value(void);\n"
    "double lj_callxs_arm64_double_lifecycle(double);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv(\n"
    "  'LJ_M7_FFI_CALLXS_DOUBLE_LIFECYCLE_SO')))\n"
    "local call = lib.lj_callxs_arm64_double_lifecycle\n"
    "local set_caller_errno = __callxs_double_lifecycle_set_caller_errno\n"
    "_G.__callxs_double_lifecycle_call = call\n"
    "function _G.__callxs_double_lifecycle_configure(mismatch_call)\n"
    "  lib.lj_callxs_arm64_double_lifecycle_configure(mismatch_call)\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_count()\n"
    "  return lib.lj_callxs_arm64_double_lifecycle_count()\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_first_errno()\n"
    "  return lib.lj_callxs_arm64_double_lifecycle_first_errno_value()\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_last_errno()\n"
    "  return lib.lj_callxs_arm64_double_lifecycle_last_errno_value()\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_last_input()\n"
    "  return lib.lj_callxs_arm64_double_lifecycle_last_input_value()\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_last_result()\n"
    "  return lib.lj_callxs_arm64_double_lifecycle_last_result_value()\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_direct(value)\n"
    "  return call(value)\n"
    "end\n"
    "jit.off(_G.__callxs_double_lifecycle_direct)\n"
    "function _G.__callxs_double_lifecycle_run(fn, n)\n"
    "  for i = 1, n do\n"
    "    if fn(i) ~= i then return false end\n"
    "  end\n"
    "  return true\n"
    "end\n"
    "function _G.__callxs_double_lifecycle_driver(fn, n)\n"
    "  set_caller_errno()\n"
    "  local result = __callxs_double_lifecycle_run(fn, n)\n"
    "  return result\n"
    "end\n"
    "jit.on()\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n");

  /* The ordinary FFI path first proves that both FP argument and result keep
  ** a non-integral value. The exact generated root below intentionally uses
  ** the integer FORL geometry certified by ARM64 admission. */
  call_configure(L, 0);
  errno = 0;
  lua_getglobal(L, "__callxs_double_lifecycle_direct");
  lua_pushnumber(L, 1.25);
  status = lua_pcall(L, 1, 1, 0);
  observed_errno = errno;
  assert(status == 0 && lua_tonumber(L, -1) == 1.25);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_count") == 1);
  assert(call_number_getter(L,
	"__callxs_double_lifecycle_last_input") == 1.25);
  assert(call_number_getter(L,
	"__callxs_double_lifecycle_last_result") == 1.25);

  call_configure(L, 0);
  errno = 0;
  status = start_run(L, 400);
  observed_errno = errno;
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  pt = global_lua_proto(L, "__callxs_double_lifecycle_run");
  T = find_callxs_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "exact double CALLXS trace was not published\n");
  assert(T != NULL);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_count") == 400);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_last_errno") == EDOM);

  /* Call eight returns 8.25. Restoring its post-call snapshot must observe
  ** the failed comparison without invoking the completed call again. */
  call_configure(L, 8);
  lj_trace_test_reset_exit_stats();
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  assert(status == 0 && !lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_first_exitno() == 13);
  assert(lj_trace_test_last_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_last_exitno() == 13);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_count") == 8);
  assert(call_number_getter(L,
	"__callxs_double_lifecycle_last_input") == 8.0);
  assert(call_number_getter(L,
	"__callxs_double_lifecycle_last_result") == 8.25);

  /* Changing the handshake epoch after every generated call forces POSTCALL
  ** exit and exact cleanup. All twenty foreign effects must remain one-shot. */
  call_configure(L, 0);
  finish_calls = 0;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(finish_calls != 0);
  lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(lj_trace_test_exit_calls() == finish_calls);
  assert(lj_trace_test_first_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_first_exitno() == 5);
  assert(lj_trace_test_last_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_last_exitno() == 5);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_count") == 20);

  /* Both exit kinds must leave the same root immediately reusable. */
  call_configure(L, 0);
  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(finish_calls != 0);
  assert(observed_errno == EDOM);
  assert(trace_runnable_acq(T, trace_traceno_acq(T)));
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L,
	"__callxs_double_lifecycle_count") == 20);

  lua_close(L);
  puts("t-arm64-jit-callxs-double-lifecycle OK: double ABI, errno, "
       "result/POSTCALL no-replay and cleanup/reuse verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-double-lifecycle SKIP: requires experimental "
       "Darwin ARM64 CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
