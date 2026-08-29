/*
** Native lifecycle proof for the exact Darwin ARM64
** const char *(const char *) CALLXS root. The hot loop retains and returns
** the last boxed pointer without recording cdata equality.
*/

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
#include "lj_ctype.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-pointer-lifecycle requires ARM64 CALLXS admission"
#endif

#define NATIVE_FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static lua_State *test_L;
static TGState *test_tg;
static GCtrace *test_trace;
static void *test_func;
static const char *test_input;
static const char *expected_result;
static GCcdata *last_result_root;
static uint32_t finish_calls;
static uint32_t result_root_observations;
static uint32_t unexpected_finish_calls;
static uint64_t finish_old_epoch;

static const char *cdata_pointer(const GCcdata *cd)
{
  const char *result;
  assert(cd != NULL);
  memcpy(&result, cdataptr(cd), sizeof(result));
  return result;
}

static void observe_result_root(TGState *tg)
{
  const LJFFINativeFrame *frame;
  GCcdata *root;
  uint32_t depth, flags;
  assert(tg == test_tg && test_L != NULL && test_trace != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  depth = lj_ffi_native_frame_depth_acq(tg);
  assert(depth == 1u);
  frame = &tg->ffi_native_frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert((flags & NATIVE_FRAME_STATE) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN) == 0);
  assert(lj_ffi_native_frame_trace_acq(frame) == test_trace);
  assert(lj_ffi_native_frame_L_acq(frame) == test_L);
  assert(lj_ffi_native_frame_func_acq(frame) == test_func);
  assert(lj_ffi_native_frame_trace_no_acq(frame) ==
	 trace_traceno_acq(test_trace));
  assert(trace_native_pins_acq(test_trace) != 0);
  root = lj_ffi_native_frame_result_root_acq(frame);
  assert(root != NULL && root->ctypeid == CTID_P_CCHAR);
  assert(cdata_pointer(root) == expected_result);
  last_result_root = root;
  result_root_observations++;
  finish_calls++;
}

static void force_epoch_each(TGState *tg)
{
  uint64_t epoch;
  observe_result_root(tg);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (finish_calls == 1u)
    finish_old_epoch = epoch;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static void observe_finish(TGState *tg)
{
  observe_result_root(tg);
}

static void observe_unexpected_finish(TGState *tg)
{
  assert(tg == test_tg);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  unexpected_finish_calls++;
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

static GCtrace *find_exact_callxs_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
	trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
	bc_op(trace_startins_acq(T)) == BC_FORL &&
	trace_op_count(T, IR_XSAVE) == 2 &&
	trace_op_count(T, IR_CALLXS) == 2 &&
	trace_op_count(T, IR_CNEW) == 2 &&
	trace_op_count(T, IR_XSTORE) == 2)
      return T;
  }
  return NULL;
}

static int proto_has_callxs_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
	trace_startpt_acq(T) == pt && trace_op_count(T, IR_CALLXS) != 0)
      return 1;
  }
  return 0;
}

static void call_configure(lua_State *L, int32_t shift)
{
  lua_getglobal(L, "__callxs_pointer_lifecycle_configure");
  lua_pushinteger(L, shift);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static void call_negative_configure(lua_State *L)
{
  lua_getglobal(L, "__callxs_pointer_negative_configure");
  assert(lua_pcall(L, 0, 0, 0) == 0);
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

static int start_named_run(lua_State *L, const char *run_name,
	const char *call_name, lua_Integer n)
{
  lua_getglobal(L, run_name);
  lua_getglobal(L, call_name);
  lua_pushinteger(L, n);
  lua_getglobal(L, "__callxs_pointer_lifecycle_string");
  return lua_pcall(L, 3, 1, 0);
}

static int start_run(lua_State *L, lua_Integer n)
{
  return start_named_run(L, "__callxs_pointer_lifecycle_driver",
	"__callxs_pointer_lifecycle_call", n);
}

static GCcdata *assert_pointer_result(lua_State *L, CTypeID id,
	const char *pointer, GCcdata *same_root)
{
  GCcdata *cd;
  assert(tviscdata(L->top-1));
  cd = cdataV(L->top-1);
  assert(cd->ctypeid == id);
  assert(cdata_pointer(cd) == pointer);
  if (same_root != NULL)
    assert(cd == same_root);
  return cd;
}

static void assert_pointer_result_survives_gc(lua_State *L, CTypeID id,
	const char *pointer, GCcdata *same_root)
{
  GCcdata *result = assert_pointer_result(L, id, pointer, same_root);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(assert_pointer_result(L, id, pointer, same_root) == result);
}

static void capture_test_values(lua_State *L)
{
  GCcdata *cd;
  lua_getglobal(L, "__callxs_pointer_lifecycle_string");
  assert(tvisstr(L->top-1));
  test_input = strdata(strV(L->top-1));
  assert(strlen(test_input) > 1u);
  lua_pop(L, 1);

  lua_getglobal(L, "__callxs_pointer_lifecycle_call");
  assert(tviscdata(L->top-1));
  cd = cdataV(L->top-1);
  memcpy(&test_func, cdataptr(cd), sizeof(test_func));
  assert(test_func != NULL);
  lua_pop(L, 1);
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

  test_L = L;
  test_tg = tg;
  lua_pushcfunction(L, set_caller_errno);
  lua_setglobal(L, "__callxs_pointer_lifecycle_set_caller_errno");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_arm64_pointer_lifecycle_configure(int32_t);\n"
    "int32_t lj_callxs_arm64_pointer_lifecycle_count(void);\n"
    "int32_t lj_callxs_arm64_pointer_lifecycle_first_errno_value(void);\n"
    "int32_t lj_callxs_arm64_pointer_lifecycle_last_errno_value(void);\n"
    "const char *lj_callxs_arm64_pointer_lifecycle(const char *);\n"
    "void lj_callxs_arm64_pointer_negative_configure(void);\n"
    "int32_t lj_callxs_arm64_pointer_negative_count(void);\n"
    "const void *lj_callxs_arm64_pointer_same_void(const void *);\n"
    "const char *lj_callxs_arm64_pointer_vararg(const char *, ...);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv(\n"
    "  'LJ_M7_FFI_CALLXS_POINTER_LIFECYCLE_SO')))\n"
    "local call = lib.lj_callxs_arm64_pointer_lifecycle\n"
    "local set_caller_errno =\n"
    "  __callxs_pointer_lifecycle_set_caller_errno\n"
    "_G.__callxs_pointer_lifecycle_call = call\n"
    "_G.__callxs_pointer_same_void =\n"
    "  lib.lj_callxs_arm64_pointer_same_void\n"
    "_G.__callxs_pointer_vararg = lib.lj_callxs_arm64_pointer_vararg\n"
    "_G.__callxs_pointer_lifecycle_string =\n"
    "  'arm64-pointer-lifecycle'\n"
    "function _G.__callxs_pointer_lifecycle_configure(shift)\n"
    "  lib.lj_callxs_arm64_pointer_lifecycle_configure(shift)\n"
    "end\n"
    "function _G.__callxs_pointer_lifecycle_count()\n"
    "  return lib.lj_callxs_arm64_pointer_lifecycle_count()\n"
    "end\n"
    "function _G.__callxs_pointer_lifecycle_first_errno()\n"
    "  return lib.lj_callxs_arm64_pointer_lifecycle_first_errno_value()\n"
    "end\n"
    "function _G.__callxs_pointer_lifecycle_last_errno()\n"
    "  return lib.lj_callxs_arm64_pointer_lifecycle_last_errno_value()\n"
    "end\n"
    "function _G.__callxs_pointer_negative_configure()\n"
    "  lib.lj_callxs_arm64_pointer_negative_configure()\n"
    "end\n"
    "function _G.__callxs_pointer_negative_count()\n"
    "  return lib.lj_callxs_arm64_pointer_negative_count()\n"
    "end\n"
    "function _G.__callxs_pointer_lifecycle_direct(s)\n"
    "  return call(s)\n"
    "end\n"
    "jit.off(_G.__callxs_pointer_lifecycle_direct)\n"
    "function _G.__callxs_pointer_lifecycle_run(fn, n, s)\n"
    "  local result\n"
    "  for _ = 1, n do\n"
    "    result = fn(s)\n"
    "  end\n"
    "  return result\n"
    "end\n"
    "function _G.__callxs_pointer_lifecycle_driver(fn, n, s)\n"
    "  set_caller_errno()\n"
    "  local result = __callxs_pointer_lifecycle_run(fn, n, s)\n"
    "  return result\n"
    "end\n"
    "function _G.__callxs_pointer_same_void_run(fn, n, s)\n"
    "  local result\n"
    "  for _ = 1, n do\n"
    "    result = fn(s)\n"
    "  end\n"
    "  return result\n"
    "end\n"
    "function _G.__callxs_pointer_vararg_run(fn, n, s)\n"
    "  local result\n"
    "  for _ = 1, n do\n"
    "    result = fn(s)\n"
    "  end\n"
    "  return result\n"
    "end\n"
    "jit.on()\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n");

  capture_test_values(L);

  /* Prove the interpreter's pointer type, exact address and errno pair. */
  call_configure(L, 0);
  errno = E2BIG;
  lua_getglobal(L, "__callxs_pointer_lifecycle_direct");
  lua_getglobal(L, "__callxs_pointer_lifecycle_string");
  status = lua_pcall(L, 1, 1, 0);
  observed_errno = errno;
  assert(status == 0);
  assert_pointer_result(L, CTID_P_CCHAR, test_input, NULL);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_count") == 1);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_last_errno") == E2BIG);

  /* Publish the exact retained-result root and prove all 400 effects. */
  call_configure(L, 0);
  errno = 0;
  status = start_run(L, 400);
  observed_errno = errno;
  assert(status == 0);
  assert_pointer_result(L, CTID_P_CCHAR, test_input, NULL);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  pt = global_lua_proto(L, "__callxs_pointer_lifecycle_run");
  T = find_exact_callxs_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "exact pointer CALLXS trace was not published\n");
  assert(T != NULL);
  test_trace = T;
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_count") == 400);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_last_errno") == EDOM);

  /* Reject generated entry after allocating/staging its hidden result root.
  ** Interpreter replay must perform each foreign effect exactly once. */
  call_configure(L, 1);
  unexpected_finish_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_unexpected_finish);
  assert(lj_tg_in_native_acq(tg) == 0);
  lj_tg_in_native_rel(tg, 1);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  assert(lj_tg_in_native_acq(tg) == 1);
  lj_tg_in_native_rel(tg, 0);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0);
  assert_pointer_result_survives_gc(L, CTID_P_CCHAR,
	test_input + 1, NULL);
  lua_pop(L, 1);
  assert(unexpected_finish_calls == 0);
  assert(observed_errno == EDOM);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_count") == 20);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_first_errno") == E2BIG);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_last_errno") == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);

  /* Force every generated return through the pointer POSTCALL snapshot. The
  ** final frame root must become the exact cdata returned to the Lua caller. */
  call_configure(L, 1);
  expected_result = test_input + 1;
  last_result_root = NULL;
  finish_calls = 0;
  result_root_observations = 0;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch_each);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  if (finish_calls != 0)
    lj_tg_hs_epoch_ack_rel(tg, finish_old_epoch);
  assert(status == 0 && finish_calls != 0);
  assert(result_root_observations == finish_calls);
  assert(last_result_root != NULL);
  assert_pointer_result_survives_gc(L, CTID_P_CCHAR,
	test_input + 1, last_result_root);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(lj_trace_test_exit_calls() == finish_calls);
  assert(lj_trace_test_first_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_first_exitno() == 7);
  assert(lj_trace_test_last_exit_parent() == trace_traceno_acq(T));
  assert(lj_trace_test_last_exitno() == 7);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_count") == 20);

  /* Ordinary completion must reuse the same trace and result-root handoff. */
  call_configure(L, 0);
  expected_result = test_input;
  last_result_root = NULL;
  finish_calls = 0;
  result_root_observations = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = start_run(L, 20);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && finish_calls != 0);
  assert(result_root_observations == finish_calls);
  assert(last_result_root != NULL);
  assert_pointer_result_survives_gc(L, CTID_P_CCHAR,
	test_input, last_result_root);
  lua_pop(L, 1);
  assert(observed_errno == EDOM);
  assert(trace_runnable_acq(T, trace_traceno_acq(T)));
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);
  assert(call_integer_getter(L,
	"__callxs_pointer_lifecycle_count") == 20);

  /* Same-void pointers and varargs are the two closest real signatures which
  ** must remain outside this exact recorder/assembler certificate. */
  call_negative_configure(L);
  status = start_named_run(L, "__callxs_pointer_same_void_run",
	"__callxs_pointer_same_void", 400);
  assert(status == 0);
  assert_pointer_result(L, CTID_P_CVOID, test_input, NULL);
  lua_pop(L, 1);
  assert(call_integer_getter(L,
	"__callxs_pointer_negative_count") == 400);
  pt = global_lua_proto(L, "__callxs_pointer_same_void_run");
  assert(!proto_has_callxs_trace(J, pt));
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);

  call_negative_configure(L);
  status = start_named_run(L, "__callxs_pointer_vararg_run",
	"__callxs_pointer_vararg", 400);
  assert(status == 0);
  assert_pointer_result(L, CTID_P_CCHAR, test_input, NULL);
  lua_pop(L, 1);
  assert(call_integer_getter(L,
	"__callxs_pointer_negative_count") == 400);
  pt = global_lua_proto(L, "__callxs_pointer_vararg_run");
  assert(!proto_has_callxs_trace(J, pt));
  assert_clean_native_state(tg, T, old_callback_slot, old_ffi_func,
	old_callback_stopreq);

  last_result_root = NULL;
  test_trace = NULL;
  test_tg = NULL;
  test_L = NULL;
  lua_close(L);
  puts("t-arm64-jit-callxs-pointer-lifecycle OK: pointer ABI, boxed root, "
       "errno, entry/POSTCALL no-replay and cleanup/reuse verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-pointer-lifecycle SKIP: requires experimental "
       "Darwin ARM64 CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
