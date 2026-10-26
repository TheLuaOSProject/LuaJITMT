/* Native lifecycle coverage for boxed fixed-width CALLXS return values. */

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
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-i64 requires ARM64 CALLXS admission"
#endif

static uint32_t native_finishes;
static uint32_t native_roots;
static uint64_t old_epoch;
static CTypeID expected_ctype;
static uint64_t expected_bits;
static void *expected_func;
static void *expected_producer;
static uint32_t native_producers;
static uint64_t total_native_results;
static uint64_t total_native_producers;
static CTypeID const_i64_result_id;
static const char *result_stage = "unspecified";
static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;

static const uint64_t i64_patterns[] = {
  UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff),
  UINT64_C(0xffffffffffffffff), UINT64_C(0x0123456789abcdef),
  UINT64_C(0xfedcba9876543210), UINT64_C(0x8000000000000001),
  UINT64_C(0x000000007fffffff), UINT64_C(0x0000000080000000),
  UINT64_C(0x0000000100000000), UINT64_C(0xffffffff80000000),
  UINT64_C(0xffffffff7fffffff), UINT64_C(0xaaaaaaaaaaaaaaaa),
  UINT64_C(0x5555555555555555)
};
static const uint64_t u64_patterns[] = {
  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
  UINT64_C(0x8000000000000000), UINT64_C(0x0123456789abcdef),
  UINT64_C(0xfedcba9876543210), UINT64_C(0x0000000100000001),
  UINT64_C(0x000000007fffffff), UINT64_C(0x0000000080000000),
  UINT64_C(0x00000000ffffffff), UINT64_C(0x0000000100000000),
  UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555)
};

static uint64_t payload_bits(const GCcdata *cd)
{
  uint64_t bits;
  assert(cd != NULL);
  memcpy(&bits, cdataptr(cd), sizeof(bits));
  return bits;
}

static void observe_native_result(TGState *tg)
{
  const LJFFINativeFrame *frame;
  GCcdata *root;
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
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  assert(trace_native_pins_acq(expected_trace) != 0);
  if (expected_producer != NULL &&
      lj_ffi_native_frame_func_acq(frame) == expected_producer) {
    assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
    native_producers++;
    total_native_producers++;
    return;
  }
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func);
  root = lj_ffi_native_frame_result_root_acq(frame);
  if (root == NULL || root->ctypeid != expected_ctype)
    fprintf(stderr,
	    "CALLXS native result root type mismatch at %s: got=%u expected=%u\n",
	    result_stage, root != NULL ? (unsigned)root->ctypeid : 0u,
	    (unsigned)expected_ctype);
  assert(root != NULL && root->ctypeid == expected_ctype);
  if (payload_bits(root) != expected_bits)
    fprintf(stderr, "CALLXS native result payload mismatch at %s\n",
	    result_stage);
  assert(payload_bits(root) == expected_bits);
  native_roots++;
  native_finishes++;
  total_native_results++;
}

static void observe_finish(TGState *tg)
{
  observe_native_result(tg);
}

static void force_postcall_epoch(TGState *tg)
{
  uint64_t epoch;
  observe_native_result(tg);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (native_finishes == 1u)
    old_epoch = epoch;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static int set_caller_errno(lua_State *L)
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

static int proto_has_scalar_callm_producer(lua_State *L, const char *name)
{
  GCproto *pt;
  const BCIns *bc;
  MSize i;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALLM) {
      BCIns prev = (BCIns)la_load32_acq((const uint32_t *)&bc[i - 1]);
      lua_pop(L, 1);
      return bc_op(prev) == BC_CALL && bc_b(prev) == 0 && bc_c(prev) == 2;
    }
  }
  lua_pop(L, 1);
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

static GCtrace *find_result_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
	trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
	bc_op(trace_startins_acq(T)) == BC_FORL &&
	op_count(T, IR_XSAVE) == 2 && op_count(T, IR_CALLXS) == 2 &&
	op_count(T, IR_CNEW) == 2 && op_count(T, IR_XSTORE) == 2)
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
	trace_startpt_acq(T) == pt && op_count(T, IR_CALLXS) != 0)
      return 1;
  }
  return 0;
}

static GCtrace *find_callm_result_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
	trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
	bc_op(trace_startins_acq(T)) == BC_FORL &&
	op_count(T, IR_XSAVE) == 4 && op_count(T, IR_CALLXS) == 4 &&
	op_count(T, IR_CNEW) == 2 && op_count(T, IR_XSTORE) == 2)
      return T;
  }
  return NULL;
}

static lua_Integer int_getter(lua_State *L, const char *name)
{
  lua_Integer result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0 && lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static void configure(lua_State *L, int is_unsigned, int mode)
{
  lua_getglobal(L, is_unsigned ? "__i64_u64_configure" :
				     "__i64_i64_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static const char *counter_name(int is_unsigned)
{
  return is_unsigned ? "__i64_u64_count" : "__i64_i64_count";
}

static const char *first_errno_name(int is_unsigned)
{
  return is_unsigned ? "__i64_u64_first_errno" : "__i64_i64_first_errno";
}

static const char *last_errno_name(int is_unsigned)
{
  return is_unsigned ? "__i64_u64_last_errno" : "__i64_i64_last_errno";
}

static const char *vararg_count_name(int is_unsigned)
{
  return is_unsigned ? "__i64_u64_va_count" : "__i64_i64_va_count";
}

static uint64_t expected_for(int is_unsigned, int mode, int32_t selector)
{
  const uint64_t *patterns = is_unsigned ? u64_patterns : i64_patterns;
  unsigned count = is_unsigned ?
	(unsigned)(sizeof(u64_patterns) / sizeof(u64_patterns[0])) :
	(unsigned)(sizeof(i64_patterns) / sizeof(i64_patterns[0]));
  (void)selector;
  return patterns[(unsigned)mode % count];
}

static GCcdata *assert_result(lua_State *L, CTypeID id, uint64_t bits)
{
  GCcdata *cd;
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  if (cd->ctypeid != id)
    fprintf(stderr, "CALLXS boxed result type mismatch at %s: got=%u expected=%u\n",
	    result_stage, (unsigned)cd->ctypeid, (unsigned)id);
  assert(cd->ctypeid == id);
  if (payload_bits(cd) != bits)
    fprintf(stderr, "CALLXS boxed result payload mismatch at %s\n",
	    result_stage);
  assert(payload_bits(cd) == bits);
  return cd;
}

static CTypeID ctype_id_from_global(lua_State *L, const char *name)
{
  CTypeID id;
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  assert(cd->ctypeid == CTID_CTYPEID);
  memcpy(&id, cdataptr(cd), sizeof(id));
  lua_pop(L, 1);
  return id;
}

static void assert_const_signed_i64_type(CTState *cts)
{
  CType raw;
  CTypeID rawid;
  CTInfo info;
  CTSize size;
  int ok = lj_ctype_info_snapshot(cts, const_i64_result_id, &info, &size,
				  &rawid, &raw);
  assert(ok == 1);
  assert(ctype_isinteger(info) && !(info & CTF_UNSIGNED));
  assert((info & CTF_CONST) != 0 && size == sizeof(int64_t));
  (void)rawid;
}

static void capture_func(lua_State *L, const char *name)
{
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(&expected_func, cdataptr(cd), sizeof(expected_func));
  assert(expected_func != NULL);
  lua_pop(L, 1);
}

static int start_direct(lua_State *L, const char *func_name, int32_t selector)
{
  lua_getglobal(L, "__i64_direct");
  lua_getglobal(L, func_name);
  lua_pushinteger(L, selector);
  return lua_pcall(L, 2, 1, 0);
}

static int start_loop(lua_State *L, const char *runner, const char *func,
		      lua_Integer n)
{
  lua_getglobal(L, "__i64_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, func);
  lua_pushinteger(L, n);
  return lua_pcall(L, 3, 1, 0);
}

static void assert_native_clean(TGState *tg, GCtrace *T,
			       MSize callback_slot, void *ffi_func,
			       uint8_t callback_stopreq)
{
  uint32_t i;
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
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
	 tg->ffi_xsave_nslots == 0);
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

static void check_direct_patterns(lua_State *L, int is_unsigned,
				  const char *func_name, CTypeID id,
				  unsigned count)
{
  unsigned mode;
  for (mode = 0; mode < count; mode++) {
    result_stage = func_name;
    configure(L, is_unsigned, (int)mode);
    errno = E2BIG;
    assert(start_direct(L, func_name, 0) == 0);
    assert_result(L, id, expected_for(is_unsigned, (int)mode, 0));
    lua_pop(L, 1);
    assert(errno == EDOM);
    assert(int_getter(L, counter_name(is_unsigned)) == 1);
    assert(int_getter(L, first_errno_name(is_unsigned)) == E2BIG);
    assert(int_getter(L, last_errno_name(is_unsigned)) == E2BIG);
  }
}

static GCtrace *check_native_loop(lua_State *L, jit_State *J, TGState *tg,
	const char *runner, const char *func, int is_unsigned, int mode,
	CTypeID id, lua_Integer n, int force_epoch, MSize callback_slot,
	void *old_ffi_func, uint8_t callback_stopreq)
{
  GCproto *pt;
  GCtrace *T;
  int status;

  configure(L, is_unsigned, mode);
  capture_func(L, func);
  expected_tg = tg;
  expected_L = L;
  expected_ctype = id;
  result_stage = runner;
  expected_bits = expected_for(is_unsigned, mode, (int32_t)n);
  native_finishes = native_roots = 0;
  native_producers = 0;
  expected_producer = NULL;
  expected_trace = NULL;
  if (force_epoch)
    old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
	force_postcall_epoch : observe_finish);
  errno = 0;
  status = start_loop(L, runner, func, n);
  {
    int observed_errno = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert(status == 0);
    assert_result(L, id, expected_bits);
    lua_gc(L, LUA_GCCOLLECT, 0);
    assert_result(L, id, expected_bits);
    lua_pop(L, 1);
    assert(observed_errno == EDOM);
  }

  pt = global_proto(L, runner);
  T = find_result_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "boxed integer CALLXS trace missing for %s "
            "(mode=%d native=%u observed=%p)\n", runner, mode,
            (unsigned)native_finishes, (void *)expected_trace);
  assert(T != NULL);
  assert(expected_trace == T);
  assert(native_finishes != 0 && native_roots == native_finishes);
  assert(int_getter(L, counter_name(is_unsigned)) == n);
  assert(int_getter(L, first_errno_name(is_unsigned)) == E2BIG);
  assert(int_getter(L, last_errno_name(is_unsigned)) == EDOM);
  if (force_epoch) {
    assert(old_epoch != UINT64_MAX);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert(lj_trace_test_exit_calls() == native_finishes);
  }
  assert_native_clean(tg, T, callback_slot, old_ffi_func, callback_stopreq);
  return T;
}

static void check_discarded_loop(lua_State *L, jit_State *J, TGState *tg,
    int is_unsigned, int force_epoch, MSize callback_slot,
    void *old_ffi_func, uint8_t callback_stopreq)
{
  const char *runner = is_unsigned ? "__u64_discard" : "__i64_discard";
  const char *func = is_unsigned ? "__i64_u64_call" : "__i64_i64_call";
  GCtrace *T;
  int status, observed_errno;
  configure(L, is_unsigned, 1);
  capture_func(L, func);
  expected_tg = tg;
  expected_L = L;
  expected_ctype = is_unsigned ? CTID_UINT64 : CTID_INT64;
  expected_bits = expected_for(is_unsigned, 1, 44);
  result_stage = runner;
  expected_trace = NULL;
  expected_producer = NULL;
  native_finishes = native_roots = native_producers = 0;
  old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
      force_postcall_epoch : observe_finish);
  status = start_loop(L, runner, func, 44);
  observed_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  if (force_epoch && old_epoch != UINT64_MAX)
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
  assert(status == 0 && lua_tointeger(L, -1) == 44);
  lua_pop(L, 1);
  T = find_result_trace(J, global_proto(L, runner));
  assert(T != NULL && expected_trace == T);
  assert(native_finishes != 0 && native_roots == native_finishes);
  assert(int_getter(L, counter_name(is_unsigned)) == 44);
  assert(observed_errno == EDOM);
  if (force_epoch) {
    assert(old_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() == native_finishes);
  }
  assert_native_clean(tg, T, callback_slot, old_ffi_func, callback_stopreq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  MSize callback_slot = ccallback_slot_acq(&tg->cb);
  uint8_t callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  void *old_ffi_func = lj_tg_ffi_call_func_acq(tg);
  GCtrace *last_trace = NULL;
  int i;

  expected_tg = tg;
  expected_L = L;
  lua_pushcfunction(L, set_caller_errno);
  lua_setglobal(L, "__i64_set_errno");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int64_t lj_test_i64_alias;\n"
    "typedef uint64_t lj_test_u64_alias;\n"
    "void lj_callxs_arm64_i64_configure(int32_t);\n"
    "void lj_callxs_arm64_u64_configure(int32_t);\n"
    "int32_t lj_callxs_arm64_i64_count(void);\n"
    "int32_t lj_callxs_arm64_u64_count(void);\n"
    "int32_t lj_callxs_arm64_i64_first_errno(void);\n"
    "int32_t lj_callxs_arm64_i64_last_errno(void);\n"
    "int32_t lj_callxs_arm64_u64_first_errno(void);\n"
    "int32_t lj_callxs_arm64_u64_last_errno(void);\n"
    "int32_t lj_callxs_arm64_i64_va_count(void);\n"
    "int32_t lj_callxs_arm64_u64_va_count(void);\n"
    "int32_t lj_callxs_arm64_i64_callm_count(void);\n"
    "int32_t lj_callxs_arm64_i64_producer_count(void);\n"
    "int64_t lj_callxs_arm64_i64(int32_t);\n"
    "uint64_t lj_callxs_arm64_u64(int32_t);\n"
    "int64_t lj_callxs_arm64_i64_vararg(int32_t, ...);\n"
    "uint64_t lj_callxs_arm64_u64_vararg(int32_t, ...);\n"
    "double lj_callxs_arm64_i64_callm_producer(int32_t);\n"
    "int64_t lj_callxs_arm64_i64_callm(int32_t, ...);\n"
    "lj_test_i64_alias lj_callxs_arm64_i64_alias(int32_t);\n"
    "const int64_t lj_callxs_arm64_i64_const(int32_t);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_I64_SO')))\n"
    "_G.__i64_const_type = ffi.typeof('const int64_t')\n"
    "_G.__i64_i64_call = lib.lj_callxs_arm64_i64\n"
    "_G.__i64_u64_call = lib.lj_callxs_arm64_u64\n"
    "_G.__i64_i64_vararg = lib.lj_callxs_arm64_i64_vararg\n"
    "_G.__i64_u64_vararg = lib.lj_callxs_arm64_u64_vararg\n"
    "_G.__i64_callm_producer = lib.lj_callxs_arm64_i64_callm_producer\n"
    "_G.__i64_callm_consumer = lib.lj_callxs_arm64_i64_callm\n"
    "_G.__i64_alias_call = lib.lj_callxs_arm64_i64_alias\n"
    "_G.__i64_const_call = lib.lj_callxs_arm64_i64_const\n"
    "_G.__i64_i64_configure = lib.lj_callxs_arm64_i64_configure\n"
    "_G.__i64_u64_configure = lib.lj_callxs_arm64_u64_configure\n"
    "_G.__i64_i64_count = lib.lj_callxs_arm64_i64_count\n"
    "_G.__i64_u64_count = lib.lj_callxs_arm64_u64_count\n"
    "_G.__i64_i64_first_errno = lib.lj_callxs_arm64_i64_first_errno\n"
    "_G.__i64_i64_last_errno = lib.lj_callxs_arm64_i64_last_errno\n"
    "_G.__i64_u64_first_errno = lib.lj_callxs_arm64_u64_first_errno\n"
    "_G.__i64_u64_last_errno = lib.lj_callxs_arm64_u64_last_errno\n"
    "_G.__i64_i64_va_count = lib.lj_callxs_arm64_i64_va_count\n"
    "_G.__i64_u64_va_count = lib.lj_callxs_arm64_u64_va_count\n"
    "_G.__i64_callm_count = lib.lj_callxs_arm64_i64_callm_count\n"
    "_G.__i64_producer_count = lib.lj_callxs_arm64_i64_producer_count\n"
    "local set_errno = __i64_set_errno\n"
    "function _G.__i64_direct(fn, x) set_errno(); return fn(x) end\n"
    "jit.off(_G.__i64_direct)\n"
    "function _G.__i64_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i) end\n"
    "  return result\n"
    "end\n"
    "function _G.__u64_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i) end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_alias_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i) end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_const_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i) end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_loop_run(fn, n)\n"
    "  local result, i = nil, 0\n"
    "  while i < n do i = i + 1; result = fn(i) end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_discard(fn, n)\n"
    "  for i = 1, n do fn(i) end; return n\n"
    "end\n"
    "function _G.__u64_discard(fn, n)\n"
    "  for i = 1, n do fn(i) end; return n\n"
    "end\n"
    "function _G.__i64_vararg_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i, i + 0.5, true, 'i64-vararg') end\n"
    "  return result\n"
    "end\n"
    "function _G.__u64_vararg_run(fn, n)\n"
    "  local result\n"
    "  for i = 1, n do result = fn(i, i + 0.5, true, 'u64-vararg') end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_callm_run(n)\n"
    "  local result, producer, consumer\n"
    "  producer, consumer = __i64_callm_producer, __i64_callm_consumer\n"
    "  for i = 1, n do result = consumer(17, producer(i)) end\n"
    "  return result\n"
    "end\n"
    "function _G.__i64_driver(run, fn, n)\n"
    "  set_errno()\n"
    "  local result = run(fn, n); return result\n"
    "end\n"
    "function _G.__i64_callm_driver(n)\n"
    "  set_errno()\n"
    "  local result = __i64_callm_run(n); return result\n"
    "end\n"
    "jit.on(); jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=128')\n"
    "if os.getenv('LJ_ARM64_I64_DUMP') then require('jit.dump').on('birs') end\n");

  const_i64_result_id = ctype_id_from_global(L, "__i64_const_type");
  assert_const_signed_i64_type(ctype_ctsG(g));

  check_direct_patterns(L, 0, "__i64_i64_call", CTID_INT64,
	(unsigned)(sizeof(i64_patterns) / sizeof(i64_patterns[0])));
  check_direct_patterns(L, 1, "__i64_u64_call", CTID_UINT64,
	(unsigned)(sizeof(u64_patterns) / sizeof(u64_patterns[0])));
  configure(L, 0, 2);
  assert(start_direct(L, "__i64_alias_call", 1) == 0);
  assert_result(L, CTID_INT64, expected_for(0, 2, 1));
  lua_pop(L, 1);
  configure(L, 0, 3);
  result_stage = "interpreter const int64_t result";
  assert(start_direct(L, "__i64_const_call", 1) == 0);
  assert_result(L, const_i64_result_id, expected_for(0, 3, 1));
  lua_pop(L, 1);

  for (i = 0; i < (int)(sizeof(i64_patterns)/sizeof(i64_patterns[0])); i++)
    last_trace = check_native_loop(L, J, tg, "__i64_run", "__i64_i64_call",
	0, i, CTID_INT64, 40 + i, 0, callback_slot, old_ffi_func,
	callback_stopreq);
  for (i = 0; i < (int)(sizeof(u64_patterns)/sizeof(u64_patterns[0])); i++)
    last_trace = check_native_loop(L, J, tg, "__u64_run", "__i64_u64_call",
	1, i, CTID_UINT64, 40 + i, 0, callback_slot, old_ffi_func,
	callback_stopreq);

  for (i = 0; i < 4; i++) {
    last_trace = check_native_loop(L, J, tg, "__i64_vararg_run",
	"__i64_i64_vararg", 0, i, CTID_INT64, 40 + i, 0,
	callback_slot, old_ffi_func, callback_stopreq);
    assert(int_getter(L, vararg_count_name(0)) == 40 + i);
  }
  for (i = 0; i < 4; i++) {
    last_trace = check_native_loop(L, J, tg, "__u64_vararg_run",
	"__i64_u64_vararg", 1, i, CTID_UINT64, 40 + i, 0,
	callback_slot, old_ffi_func, callback_stopreq);
    assert(int_getter(L, vararg_count_name(1)) == 40 + i);
  }

  /* The same raw CTypeID must be used for typedef and const-qualified result
  ** declarations, and their boxed values must survive the same native path. */
  last_trace = check_native_loop(L, J, tg, "__i64_alias_run",
	"__i64_alias_call", 0, 4, CTID_INT64, 48, 0, callback_slot,
	old_ffi_func, callback_stopreq);
  last_trace = check_native_loop(L, J, tg, "__i64_const_run",
	"__i64_const_call", 0, 5, const_i64_result_id, 49, 0, callback_slot,
	old_ffi_func, callback_stopreq);
  /* LOOP's ordinary scalar graph accepts this root, but CALLXS lifecycle
  ** admission is deliberately FORL-only. Verify the loop remains correct
  ** through the interpreter and does not publish a native CALLXS trace. */
  configure(L, 0, 1);
  result_stage = "interpreter-only LOOP boundary";
  errno = 0;
  assert(start_loop(L, "__i64_loop_run", "__i64_i64_call", 44) == 0);
  assert_result(L, CTID_INT64, i64_patterns[1]);
  lua_pop(L, 1);
  assert(errno == EDOM);
  assert(int_getter(L, "__i64_i64_count") == 44);
  assert(!proto_has_callxs_trace(J, global_proto(L, "__i64_loop_run")));

  /* CALLM consumes the preceding scalar double CALL result as a promoted
  ** variadic value; only the consumer returns boxed I64. */
  configure(L, 0, 2);
  capture_func(L, "__i64_callm_consumer");
  {
    lua_State *current = L;
    GCproto *pt;
    GCtrace *T;
    int status;
    expected_tg = tg;
    expected_L = L;
    expected_ctype = CTID_INT64;
    result_stage = "CALLM boxed I64 result";
    expected_bits = expected_for(0, 2, 48);
    expected_trace = NULL;
    expected_producer = NULL;
    capture_func(L, "__i64_callm_producer");
    expected_producer = expected_func;
    expected_func = NULL;
    /* Keep separate identities: the observer distinguishes producer (no box)
    ** from consumer (boxed result) by the frame's function pointer. */
    {
      void *producer_func = expected_producer;
      lua_getglobal(L, "__i64_callm_consumer");
      {
        GCcdata *cd = cdataV(L->top - 1);
        memcpy(&expected_func, cdataptr(cd), sizeof(expected_func));
      }
      lua_pop(L, 1);
      expected_producer = producer_func;
    }
    native_finishes = native_roots = native_producers = 0;
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    lua_getglobal(current, "__i64_callm_driver");
    lua_pushinteger(current, 48);
    status = lua_pcall(current, 1, 1, 0);
    {
      int observed_errno = errno;
      lj_ffi_native_trace_test_set_finish_hook(NULL);
      assert(status == 0);
      assert_result(current, CTID_INT64, expected_bits);
      lua_gc(current, LUA_GCCOLLECT, 0);
      assert_result(current, CTID_INT64, expected_bits);
      lua_pop(current, 1);
      assert(observed_errno == EDOM);
    }
    pt = global_proto(current, "__i64_callm_run");
    assert(proto_has_scalar_callm_producer(current, "__i64_callm_run"));
    T = find_callm_result_trace(J, pt);
    assert(T != NULL && native_finishes > 0 && native_roots == native_finishes);
    assert(expected_trace == T);
    assert(native_producers > 0);
    assert(int_getter(current, "__i64_i64_count") == 48);
    assert(int_getter(current, "__i64_callm_count") == 48);
    assert(int_getter(current, "__i64_producer_count") == 48);
    assert_native_clean(tg, T, callback_slot, old_ffi_func, callback_stopreq);
    last_trace = T;
  }

  /* Force a POSTCALL epoch exit after each completed foreign effect. The
  ** returned boxed result and every count must survive without a replay. */
  last_trace = check_native_loop(L, J, tg, "__i64_run", "__i64_i64_call",
	0, 3, CTID_INT64, 20, 1, callback_slot, old_ffi_func,
	callback_stopreq);
  assert(trace_runnable_acq(last_trace, trace_traceno_acq(last_trace)));

  /* B=1 still roots and fills the box through native leave, but intentionally
  ** has no caller result slot. A forced postcall exit must not replay it. */
  check_discarded_loop(L, J, tg, 0, 0, callback_slot, old_ffi_func,
      callback_stopreq);
  check_discarded_loop(L, J, tg, 1, 0, callback_slot, old_ffi_func,
      callback_stopreq);
  check_discarded_loop(L, J, tg, 0, 1, callback_slot, old_ffi_func,
      callback_stopreq);

  /* A returned value stays strongly rooted across collection and trace flush. */
  configure(L, 1, 1);
  result_stage = "GC/flush retained uint64_t result";
  assert(start_direct(L, "__i64_u64_call", 0) == 0);
  assert_result(L, CTID_UINT64, u64_patterns[1]);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert_result(L, CTID_UINT64, u64_patterns[1]);
  ljt_lua_dostring(L, "jit.flush()\n");
  assert_result(L, CTID_UINT64, u64_patterns[1]);
  lua_pop(L, 1);
  assert(lj_tg_load_jit_base(tg) == NULL);

  expected_trace = NULL;
  expected_func = NULL;
  expected_L = NULL;
  expected_tg = NULL;
  lua_close(L);
  printf("boxed I64/U64 native results=%llu scalar CALLM producers=%llu\n",
         (unsigned long long)total_native_results,
         (unsigned long long)total_native_producers);
  puts("t-arm64-jit-callxs-i64 OK: signed/unsigned 64-bit boxed returns, "
       "typedef/const CType resolution, native roots, POSTCALL no-replay, "
       "errno, GC, and cleanup verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-i64 SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
