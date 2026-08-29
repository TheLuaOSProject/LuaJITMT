/*
** Native lifecycle proof for the exact Darwin ARM64 int32_t(int32_t)
** CALLXS root. Every foreign effect is counted so no exit path can replay it.
*/

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_XSAVE_TEST_HELPERS) && defined(LJ_GC2_TEST_HELPERS) && \
    defined(LJ_TAB_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-lifecycle requires ARM64 CALLXS admission"
#endif

static uint32_t finish_calls;
static uint32_t leave_hook_calls;
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

static void force_fresh_stopreq_after_native_leave(TGState *tg)
{
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  leave_hook_calls++;
  assert(leave_hook_calls == 1);
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ | TGF_STOPREQ_FRESH);
  lj_ffi_native_trace_test_set_leave_hook(NULL);
}

static int arm_rooted_retry(lua_State *L)
{
  (void)L;
  lj_tab_test_forjit_initial_miss_after(2);
  errno = E2BIG;
  return 0;
}

static int set_caller_errno(lua_State *L)
{
  (void)L;
  errno = E2BIG;
  return 0;
}

static void clobber_wait_errno(lua_State *L)
{
  (void)L;
  errno = EILSEQ;
}

static void stopreq_wait_with_clobbered_errno(lua_State *L)
{
  (void)lj_tg_flags_or_rlx(L2TG(L), TGF_STOPREQ | TGF_STOPREQ_FRESH);
  errno = EILSEQ;
}

static int call_wait_with_caller_errno(lua_State *L)
{
  errno = E2BIG;
  lj_tab_wait_l(L);
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

static void call_configure(lua_State *L, int32_t mismatch, int callback)
{
  lua_getglobal(L, "__callxs_lifecycle_configure");
  lua_pushinteger(L, mismatch);
  lua_pushboolean(L, callback);
  assert(lua_pcall(L, 2, 0, 0) == 0);
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

static int start_run(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__callxs_lifecycle_driver");
  lua_getglobal(L, "__callxs_lifecycle_call");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
}

static int start_retry_run(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__callxs_lifecycle_retry_driver");
  lua_getglobal(L, "__callxs_lifecycle_call");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
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
  assert(lj_tg_ffi_call_func_acq(tg) == ffi_func);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL);
  assert(tg->ffi_xsave_baseslot == 0);
  assert(tg->ffi_xsave_nslots == 0);
  assert(trace_native_pins_acq(T) == 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCproto *pt;
  GCtrace *T;
  MSize old_callback_slot = ccallback_slot_acq(&tg->cb);
  void *old_ffi_func = lj_tg_ffi_call_func_acq(tg);
  uint8_t old_callback_stopreq =
    ccallback_native_had_stopreq_acq(&tg->cb);
  uint8_t old_flags;
  int observed_errno, status;

  lua_pushinteger(L, EAGAIN);
  lua_setglobal(L, "__callxs_lifecycle_eagain");
  lua_pushinteger(L, ERANGE);
  lua_setglobal(L, "__callxs_lifecycle_erange");
  lua_pushcfunction(L, arm_rooted_retry);
  lua_setglobal(L, "__callxs_lifecycle_arm_retry");
  lua_pushcfunction(L, set_caller_errno);
  lua_setglobal(L, "__callxs_lifecycle_set_caller_errno");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int32_t (*lj_callxs_arm64_lifecycle_cb_t)(int32_t);\n"
    "void lj_callxs_arm64_lifecycle_configure(int32_t, int32_t);\n"
    "void lj_callxs_arm64_lifecycle_set_callback(\n"
    "  lj_callxs_arm64_lifecycle_cb_t);\n"
    "int32_t lj_callxs_arm64_lifecycle_count(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_callback_count(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_callback_errno_value(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_first_errno_value(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_second_errno_value(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_third_errno_value(void);\n"
    "int32_t lj_callxs_arm64_lifecycle_last_errno_value(void);\n"
    "int32_t lj_callxs_arm64_lifecycle(int32_t);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv(\n"
    "  'LJ_M7_FFI_CALLXS_LIFECYCLE_SO')))\n"
    "local call = lib.lj_callxs_arm64_lifecycle\n"
    "local arm_retry = __callxs_lifecycle_arm_retry\n"
    "local set_caller_errno = __callxs_lifecycle_set_caller_errno\n"
    "local function callback_body(value)\n"
    "  assert(ffi.errno() == __callxs_lifecycle_eagain, ffi.errno())\n"
    "  collectgarbage('step', 8)\n"
    "  ffi.errno(__callxs_lifecycle_erange)\n"
    "  return value\n"
    "end\n"
    "jit.off(callback_body, true)\n"
    "local callback = ffi.cast(\n"
    "  'lj_callxs_arm64_lifecycle_cb_t', callback_body)\n"
    "lib.lj_callxs_arm64_lifecycle_set_callback(callback)\n"
    "_G.__callxs_lifecycle_callback = callback\n"
    "_G.__callxs_lifecycle_call = call\n"
    "function _G.__callxs_lifecycle_configure(mismatch, use_callback)\n"
    "  lib.lj_callxs_arm64_lifecycle_configure(\n"
    "    mismatch, use_callback and 1 or 0)\n"
    "end\n"
    "function _G.__callxs_lifecycle_count()\n"
    "  return lib.lj_callxs_arm64_lifecycle_count()\n"
    "end\n"
    "function _G.__callxs_lifecycle_callback_count()\n"
    "  return lib.lj_callxs_arm64_lifecycle_callback_count()\n"
    "end\n"
    "function _G.__callxs_lifecycle_callback_errno()\n"
    "  return lib.lj_callxs_arm64_lifecycle_callback_errno_value()\n"
    "end\n"
    "function _G.__callxs_lifecycle_first_errno()\n"
    "  return lib.lj_callxs_arm64_lifecycle_first_errno_value()\n"
    "end\n"
    "function _G.__callxs_lifecycle_second_errno()\n"
    "  return lib.lj_callxs_arm64_lifecycle_second_errno_value()\n"
    "end\n"
    "function _G.__callxs_lifecycle_third_errno()\n"
    "  return lib.lj_callxs_arm64_lifecycle_third_errno_value()\n"
    "end\n"
    "function _G.__callxs_lifecycle_last_errno()\n"
    "  return lib.lj_callxs_arm64_lifecycle_last_errno_value()\n"
    "end\n"
    "local function lifecycle_run(fn, n)\n"
    "  for i = 1, n do\n"
    "    if fn(i) ~= i then return false end\n"
    "  end\n"
    "  return true\n"
    "end\n"
    "_G.__callxs_lifecycle_run = lifecycle_run\n"
    "function _G.__callxs_lifecycle_driver(fn, n)\n"
    "  set_caller_errno()\n"
    "  local result = lifecycle_run(fn, n)\n"
    "  return result\n"
    "end\n"
    "function _G.__callxs_lifecycle_retry_driver(fn, n)\n"
    "  arm_retry()\n"
    "  local result = lifecycle_run(fn, n)\n"
    "  return result\n"
    "end\n"
    "jit.on()\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n");

  call_configure(L, INT32_MIN, 0);
  errno = 0;
  status = start_run(L, 400);
  observed_errno = errno;
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(observed_errno == EDOM);

  pt = global_lua_proto(L, "__callxs_lifecycle_run");
  T = find_callxs_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "exact lifecycle CALLXS trace was not published\n");
  assert(T != NULL);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 400);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_second_errno") == EDOM);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_last_errno") == EDOM);

  /* The first interpreted call arms one retry for the generated lookup. A
  ** deterministic wait-side clobber must not reach either generated call. */
  call_configure(L, INT32_MIN, 0);
  lj_tab_test_reset_wait_l_calls();
  lj_tab_test_set_wait_l_after_yield_hook(clobber_wait_errno);
  finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = start_retry_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  lj_tab_test_set_wait_l_after_yield_hook(NULL);
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(finish_calls != 0);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 20);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_callback_count") == 0);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_second_errno") == EDOM);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_third_errno") == EDOM);
  assert(lj_tab_test_wait_l_calls() != 0);

  /* A retry wait may observe a fresh STOPREQ after returning from the
  ** scheduler. It must throw with the pre-wait error pair. */
  old_flags = lj_tg_flags_acq(tg);
  lj_tab_test_set_wait_l_after_yield_hook(
    stopreq_wait_with_clobbered_errno);
  errno = 0;
  lua_pushcfunction(L, call_wait_with_caller_errno);
  status = lua_pcall(L, 0, 0, 0);
  observed_errno = errno;
  lj_tab_test_set_wait_l_after_yield_hook(NULL);
  assert(status != 0);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  assert(observed_errno == E2BIG);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);

  /* The result guard exits after the eighth completed foreign call. The
  ** restored post-call snapshot must not invoke that call a second time. */
  call_configure(L, 8, 0);
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
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 8);

  /* A changed handshake epoch forces POSTCALL exit/cleanup after a completed
  ** call. Every loop effect must still occur exactly once. */
  call_configure(L, INT32_MIN, 0);
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
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 20);

  /* Callback entry suspends the generated ACTIVE frame. Callback return must
  ** resume it, preserve the callback's error state for the callee, then force
  ** one post-call exit without replay. */
  call_configure(L, INT32_MIN, 1);
  finish_calls = 0;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_toboolean(L, -1));
  lua_pop(L, 1);
  assert(finish_calls != 0);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(lj_trace_test_exit_calls() == finish_calls);
  assert(lj_trace_test_first_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_first_exitno() == 5);
  assert(lj_trace_test_last_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_last_exitno() == 5);
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 20);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_callback_count") == 20);
  assert(call_integer_getter(L,
	"__callxs_lifecycle_callback_errno") == ERANGE);

  /* The root starts after one interpreted iteration. A fresh STOPREQ throws
  ** after the next, first generated foreign effect. Native frame cleanup and
  ** the final unwind landing must preserve its errno without replay. */
  call_configure(L, INT32_MIN, 0);
  old_flags = lj_tg_flags_acq(tg);
  leave_hook_calls = 0;
  lj_ffi_native_trace_test_set_leave_hook(
    force_fresh_stopreq_after_native_leave);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_leave_hook(NULL);
  assert(status != 0);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  lj_tg_flags_store_rlx(tg, old_flags);
  assert(leave_hook_calls == 1);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 2);

  /* The throwing unwind must leave the same root runnable. */
  call_configure(L, INT32_MIN, 0);
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
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L, "__callxs_lifecycle_count") == 20);

  lua_close(L);
  puts("t-arm64-jit-callxs-lifecycle OK: errno, result/POSTCALL exits, "
       "callback and STOPREQ preserved exact effects");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-lifecycle SKIP: requires experimental "
       "Darwin ARM64 CALLXS and table test hooks");
  return 0;
}

#endif
