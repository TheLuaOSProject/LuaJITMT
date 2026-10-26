/* Native lifecycle proof for fixed I64/U64 CALLXS arguments. */

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
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-i64-args requires ARM64 CALLXS admission"
#endif

#define NATIVE_FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static lua_State *test_L;
static TGState *test_tg;
static GCtrace *observed_trace;
static void *expected_func;
static void *boxed_source_func;
static void *boxed_consumer_func;
static CTypeID expected_box_type;
static GCcdata *expected_box_roots[2];
static uint32_t total_native_calls;
static uint32_t native_calls;
static uint32_t native_sources;
static uint32_t native_consumers;
static uint64_t forced_epoch;
static const char *test_stage = "unset";
static GCcdata *expected_arg_roots[9];
static CTypeID expected_arg_type;
static unsigned expected_arg_count = 9;
static unsigned expected_box_count = 1;

static uint64_t cdata_bits(const GCcdata *cd)
{
  uint64_t bits;
  assert(cd != NULL);
  memcpy(&bits, cdataptr(cd), sizeof(bits));
  return bits;
}

static void assert_rooted_arguments(const LJFFINativeFrame *frame,
                                    unsigned count)
{
  uint64_t root_offset = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t top_offset = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(test_L->stack);
  TValue *maxstack = tvref(test_L->maxstack);
  TValue *root, *top, *tv;
  unsigned i;
  assert(root_offset <= top_offset &&
         (root_offset % sizeof(TValue)) == 0 &&
         (top_offset % sizeof(TValue)) == 0);
  assert((uintptr_t)maxstack >= (uintptr_t)stack &&
         top_offset + sizeof(TValue) <=
           (uint64_t)((uintptr_t)maxstack - (uintptr_t)stack));
  root = (TValue *)((char *)stack + root_offset);
  top = (TValue *)((char *)stack + top_offset);
  assert(root <= top);
  for (i = 0; i < count; i++) {
    int found = 0;
    for (tv = root; tv <= top; tv++) {
      if (tviscdata(tv)) {
        GCcdata *cd = cdataV(tv);
        if (cd == expected_arg_roots[i] && cd->ctypeid == expected_arg_type) {
          found = 1;
          break;
        }
      }
    }
    if (!found)
      fprintf(stderr, "CALLXS argument root %u missing at %s\n", i, test_stage);
    assert(found);
  }
}

static void observe_native(TGState *tg)
{
  const LJFFINativeFrame *frame;
  void *func;
  GCcdata *result_root;
  assert(tg == test_tg && test_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) & NATIVE_FRAME_STATE) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == test_L);
  if (observed_trace == NULL)
    observed_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(observed_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == observed_trace);
  assert(trace_native_pins_acq(observed_trace) != 0);
  total_native_calls++;
  func = lj_ffi_native_frame_func_acq(frame);
  result_root = lj_ffi_native_frame_result_root_acq(frame);
  if (func == expected_func) {
    assert(result_root == NULL);  /* The nine-argument helpers return int32. */
    assert_rooted_arguments(frame, expected_arg_count);
    native_calls++;
  } else if (func == boxed_source_func) {
    assert(result_root != NULL && result_root->ctypeid == expected_box_type);
    {
      unsigned index = native_sources % expected_box_count;
      uint64_t bits = UINT64_C(0x8000000000000000) |
        (uint64_t)(index+1u);
      assert(cdata_bits(result_root) == bits);
      expected_box_roots[index] = result_root;
    }
    native_sources++;
  } else if (func == boxed_consumer_func) {
    assert(result_root == NULL);
    unsigned i;
    for (i = 0; i < expected_box_count; i++) {
      assert(expected_box_roots[i] != NULL);
      expected_arg_roots[i] = expected_box_roots[i];
    }
    assert_rooted_arguments(frame, 9);
    native_consumers++;
  } else {
    fprintf(stderr, "unexpected native CALLXS function at %s: %p\n",
            test_stage, func);
    assert(0);
  }
}

static void finish_native(TGState *tg)
{ observe_native(tg); }

static void force_postcall_epoch(TGState *tg)
{
  uint64_t epoch;
  observe_native(tg);
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  if (forced_epoch == UINT64_MAX) forced_epoch = epoch;
  assert(epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static int set_errno(lua_State *L)
{
  (void)L;
  errno = E2BIG;
  return 0;
}

static GCproto *global_proto(lua_State *L, const char *name)
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
  for (ref = REF_FIRST; ref < trace_nins_acq((GCtrace *)T); ref++)
    if (ir_load_acq(&ir[ref]).o == wanted) count++;
  return count;
}

static GCtrace *find_callxs_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        trace_op_count(T, IR_CALLXS) != 0)
      return T;
  }
  return NULL;
}

static int has_callxs_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) &&
        trace_startpt_acq(T) == pt && trace_op_count(T, IR_CALLXS) != 0)
      return 1;
  }
  return 0;
}

static void capture_func(lua_State *L, const char *name, void **out)
{
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  cd = cdataV(L->top-1);
  memcpy(out, cdataptr(cd), sizeof(*out));
  assert(*out != NULL);
  lua_pop(L, 1);
}

static int push_table_item(lua_State *L, const char *table, int index);

static void capture_arg_roots(lua_State *L, const char *table)
{
  int i;
  for (i = 0; i < 9; i++) {
    push_table_item(L, table, i+1);
    assert(tviscdata(L->top-1));
    expected_arg_roots[i] = cdataV(L->top-1);
    lua_pop(L, 1);
  }
}

static lua_Integer getter(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0 && lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static int push_table_item(lua_State *L, const char *table, int index)
{
  lua_getglobal(L, table);
  assert(lua_istable(L, -1));
  lua_rawgeti(L, -1, index);
  lua_remove(L, -2);
  return 1;
}

static int start_fixed(lua_State *L, const char *runner, const char *func,
                       lua_Integer n, const char *table)
{
  int i;
  lua_getglobal(L, "__i64args_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, func);
  lua_pushinteger(L, n);
  for (i = 1; i <= 9; i++) push_table_item(L, table, i);
  return lua_pcall(L, 12, 1, 0);
}

static int start_pair(lua_State *L, lua_Integer n)
{
  int i;
  lua_getglobal(L, "__i64args_pair_driver");
  lua_getglobal(L, "__i64args_run_boxed_pair");
  lua_getglobal(L, "__i64args_source");
  lua_getglobal(L, "__i64args_consume");
  lua_pushinteger(L, n);
  for (i = 1; i <= 8; i++) push_table_item(L, "__i64args_signed_values", i);
  return lua_pcall(L, 12, 1, 0);
}

static int start_vararg(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__i64args_vararg_driver");
  lua_getglobal(L, "__i64args_run_vararg");
  lua_getglobal(L, "__i64args_vararg");
  lua_pushinteger(L, n);
  push_table_item(L, "__i64args_signed_values", 1);
  return lua_pcall(L, 4, 1, 0);
}

static void assert_native_cleanup(TGState *tg, GCtrace *T,
                                 MSize slot, void *ffi_func,
                                 uint8_t old_stopreq)
{
  unsigned i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == old_stopreq);
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
  }
}

static void configure(lua_State *L, int mode)
{
  lua_getglobal(L, "__i64args_select");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == 0);
  lua_getglobal(L, "__i64args_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static void run_native_case(lua_State *L, jit_State *J, TGState *tg,
    const char *runner, const char *func, const char *value_table,
    const char *counter, unsigned mode, lua_Integer n, int force_epoch,
    MSize old_slot, void *old_ffi_func, uint8_t old_stopreq)
{
  GCproto *pt;
  GCtrace *T;
  int status;
  uint64_t exits;
  configure(L, (int)mode);
  test_stage = runner;
  expected_arg_count = 9;
  capture_arg_roots(L, value_table);
  expected_arg_type = strcmp(value_table, "__i64args_unsigned_values") == 0 ?
    CTID_UINT64 : CTID_INT64;
  if (strcmp(value_table, "__i64args_const_values") == 0) {
    lua_getglobal(L, "__i64args_const_type");
    assert(tviscdata(L->top-1) && cdataV(L->top-1)->ctypeid == CTID_CTYPEID);
    memcpy(&expected_arg_type, cdataptr(cdataV(L->top-1)),
           sizeof(expected_arg_type));
    lua_pop(L, 1);
  }
  expected_func = NULL;
  capture_func(L, func, &expected_func);
  boxed_source_func = boxed_consumer_func = NULL;
  observed_trace = NULL;
  native_calls = native_sources = native_consumers = 0;
  forced_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_epoch ?
      force_postcall_epoch : finish_native);
  errno = 0;
  status = start_fixed(L, runner, func, n, value_table);
  exits = lj_trace_test_exit_calls();
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0);
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  assert(errno == EDOM);
  assert(getter(L, counter) == n);
  assert(getter(L, "__i64args_first_errno") == E2BIG);
  assert(getter(L, "__i64args_last_errno") == EDOM);
  pt = global_proto(L, runner);
  T = find_callxs_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "missing native fixed I64 CALLXS trace at %s, native=%u\n",
            runner, native_calls);
  assert(T != NULL && observed_trace == T && native_calls > 0);
  if (force_epoch) {
    assert(forced_epoch != UINT64_MAX);
    lj_tg_hs_epoch_ack_rel(tg, forced_epoch);
    assert(exits > 0);
  }
  assert_native_cleanup(tg, T, old_slot, old_ffi_func, old_stopreq);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void run_boxed_pair(lua_State *L, jit_State *J, TGState *tg,
                          MSize old_slot, void *old_ffi_func,
                          uint8_t old_stopreq)
{
  GCproto *pt;
  GCtrace *T;
  int status;
  lua_Integer n = 40;
  configure(L, 0);
  test_stage = "boxed producer to fixed I64 argument";
  capture_arg_roots(L, "__i64args_signed_values");
  memmove(&expected_arg_roots[1], &expected_arg_roots[0],
          8u * sizeof(expected_arg_roots[0]));
  expected_arg_roots[0] = NULL;
  expected_arg_type = CTID_INT64;
  expected_arg_count = 9;
  expected_box_count = 1;
  expected_box_roots[0] = NULL;
  expected_func = NULL;
  capture_func(L, "__i64args_consume", &boxed_consumer_func);
  capture_func(L, "__i64args_source", &boxed_source_func);
  expected_box_type = CTID_INT64;
  expected_func = NULL;
  observed_trace = NULL;
  native_calls = native_sources = native_consumers = 0;
  lj_ffi_native_trace_test_set_finish_hook(finish_native);
  errno = 0;
  status = start_pair(L, n);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  assert(errno == EDOM);
  assert(getter(L, "__i64args_source_count") == n);
  assert(getter(L, "__i64args_consumer_count") == n);
  assert(getter(L, "__i64args_first_errno") == E2BIG);
  assert(getter(L, "__i64args_last_errno") == EDOM);
  pt = global_proto(L, "__i64args_run_boxed_pair");
  T = find_callxs_trace(J, pt);
  assert(T != NULL && observed_trace == T);
  assert(native_sources > 0 && native_consumers == native_sources);
  assert_native_cleanup(tg, T, old_slot, old_ffi_func, old_stopreq);
}

static void run_fixed_prefix_vararg(lua_State *L, jit_State *J, TGState *tg,
                                    MSize old_slot, void *old_ffi_func,
                                    uint8_t old_stopreq)
{
  GCproto *pt;
  GCtrace *T;
  lua_Integer n = 40;
  int status;
  configure(L, 0);
  test_stage = "fixed I64 prefix with promoted scalar varargs";
  capture_arg_roots(L, "__i64args_signed_values");
  expected_arg_type = CTID_INT64;
  expected_arg_count = 1;
  capture_func(L, "__i64args_vararg", &expected_func);
  boxed_source_func = boxed_consumer_func = NULL;
  observed_trace = NULL;
  native_calls = native_sources = native_consumers = 0;
  lj_ffi_native_trace_test_set_finish_hook(finish_native);
  errno = 0;
  status = start_vararg(L, n);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  assert(errno == EDOM);
  assert(getter(L, "__i64args_vararg_count") == n);
  assert(getter(L, "__i64args_first_errno") == E2BIG);
  assert(getter(L, "__i64args_last_errno") == EDOM);
  pt = global_proto(L, "__i64args_run_vararg");
  T = find_callxs_trace(J, pt);
  if (T == NULL)
    fprintf(stderr, "missing native fixed-prefix variadic CALLXS trace\n");
  assert(T != NULL && observed_trace == T && native_calls > 0);
  assert_native_cleanup(tg, T, old_slot, old_ffi_func, old_stopreq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  MSize old_slot = ccallback_slot_acq(&tg->cb);
  uint8_t old_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  void *old_ffi_func = lj_tg_ffi_call_func_acq(tg);
  int mode;

  test_L = L;
  test_tg = tg;
  lua_pushcfunction(L, set_errno);
  lua_setglobal(L, "__i64args_set_errno");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int64_t lj_test_i64_alias; typedef uint64_t lj_test_u64_alias;\n"
    "void lj_callxs_i64_args_configure(int32_t);\n"
    "int32_t lj_callxs_i64_args_signed_count(void);\n"
    "int32_t lj_callxs_i64_args_unsigned_count(void);\n"
    "int32_t lj_callxs_i64_args_alias_count(void);\n"
    "int32_t lj_callxs_i64_args_const_count(void);\n"
    "int32_t lj_callxs_i64_args_source_count(void);\n"
    "int32_t lj_callxs_i64_args_consumer_count(void);\n"
    "int32_t lj_callxs_i64_args_consumer2_count(void);\n"
    "int32_t lj_callxs_i64_args_callm_count(void);\n"
    "int32_t lj_callxs_i64_args_vararg_count(void);\n"
    "int32_t lj_callxs_i64_args_first_errno(void);\n"
    "int32_t lj_callxs_i64_args_last_errno(void);\n"
    "int32_t lj_callxs_i64_args_fixed(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_u64_args_fixed(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n"
    "int32_t lj_callxs_i64_args_alias(lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias,lj_test_i64_alias);\n"
    "int32_t lj_callxs_i64_args_const(const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t);\n"
    "int64_t lj_callxs_i64_args_source(int32_t);\n"
    "int32_t lj_callxs_i64_args_consume(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_i64_args_consume2(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_i64_args_callm_consumer(int32_t,...);\n"
    "int32_t lj_callxs_i64_args_vararg(int64_t,...);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_I64_ARGS_SO')))\n"
    "_G.__i64args_configure = lib.lj_callxs_i64_args_configure\n"
    "_G.__i64args_signed_count = lib.lj_callxs_i64_args_signed_count\n"
    "_G.__i64args_unsigned_count = lib.lj_callxs_i64_args_unsigned_count\n"
    "_G.__i64args_alias_count = lib.lj_callxs_i64_args_alias_count\n"
    "_G.__i64args_const_count = lib.lj_callxs_i64_args_const_count\n"
    "_G.__i64args_source_count = lib.lj_callxs_i64_args_source_count\n"
    "_G.__i64args_consumer_count = lib.lj_callxs_i64_args_consumer_count\n"
    "_G.__i64args_consumer2_count = lib.lj_callxs_i64_args_consumer2_count\n"
    "_G.__i64args_callm_count = lib.lj_callxs_i64_args_callm_count\n"
    "_G.__i64args_vararg_count = lib.lj_callxs_i64_args_vararg_count\n"
    "_G.__i64args_first_errno = lib.lj_callxs_i64_args_first_errno\n"
    "_G.__i64args_last_errno = lib.lj_callxs_i64_args_last_errno\n"
    "_G.__i64args_fixed = lib.lj_callxs_i64_args_fixed\n"
    "_G.__i64args_unsigned = lib.lj_callxs_u64_args_fixed\n"
    "_G.__i64args_alias = lib.lj_callxs_i64_args_alias\n"
    "_G.__i64args_const = lib.lj_callxs_i64_args_const\n"
    "_G.__i64args_source = lib.lj_callxs_i64_args_source\n"
    "_G.__i64args_consume = lib.lj_callxs_i64_args_consume\n"
    "_G.__i64args_consume2 = lib.lj_callxs_i64_args_consume2\n"
    "_G.__i64args_callm_consumer = lib.lj_callxs_i64_args_callm_consumer\n"
    "_G.__i64args_vararg = lib.lj_callxs_i64_args_vararg\n"
    "_G.__i64args_const_type = ffi.typeof('const int64_t')\n"
    "local S = { ffi.new('int64_t',0x8000000000000000LL), ffi.new('int64_t',0x7fffffffffffffffLL), ffi.new('int64_t',-1), ffi.new('int64_t',0x0123456789abcdefLL), ffi.new('int64_t',-81985529216486896LL), ffi.new('int64_t',-9223372036854775807LL), ffi.new('int64_t',0x7fffffff), ffi.new('int64_t',0x80000000), ffi.new('int64_t',0x100000000) }\n"
    "local S2 = { ffi.new('int64_t',-6148914691236517206LL), ffi.new('int64_t',6148914691236517205LL), ffi.new('int64_t',-2147483648), ffi.new('int64_t',-2147483649LL), ffi.new('int64_t',4294967295ULL), ffi.new('int64_t',4294967297LL), ffi.new('int64_t',-9223372036854775807LL-1), ffi.new('int64_t',0x7fffffffffffffffLL), ffi.new('int64_t',0x0123456789abcdefLL) }\n"
    "local U = { ffi.new('uint64_t',0ULL), ffi.new('uint64_t',0xffffffffffffffffULL), ffi.new('uint64_t',0x8000000000000000ULL), ffi.new('uint64_t',0x0123456789abcdefULL), ffi.new('uint64_t',0xfedcba9876543210ULL), ffi.new('uint64_t',0x100000001ULL), ffi.new('uint64_t',0x7fffffffULL), ffi.new('uint64_t',0x80000000ULL), ffi.new('uint64_t',0xffffffffULL) }\n"
    "local U2 = { ffi.new('uint64_t',0xaaaaaaaaaaaaaaaaULL), ffi.new('uint64_t',0x5555555555555555ULL), ffi.new('uint64_t',0x100000000ULL), ffi.new('uint64_t',0xffffffffULL), ffi.new('uint64_t',0x8000000000000001ULL), ffi.new('uint64_t',0x7fffffffffffffffULL), ffi.new('uint64_t',0xffffffffffffffffULL), ffi.new('uint64_t',1ULL), ffi.new('uint64_t',0xfedcba9876543210ULL) }\n"
    "_G.__i64args_signed_values = S\n"
    "_G.__i64args_unsigned_values = U\n"
    "_G.__i64args_alias_values = S\n"
    "_G.__i64args_const_values = { ffi.cast('const int64_t',S[1]),ffi.cast('const int64_t',S[2]),ffi.cast('const int64_t',S[3]),ffi.cast('const int64_t',S[4]),ffi.cast('const int64_t',S[5]),ffi.cast('const int64_t',S[6]),ffi.cast('const int64_t',S[7]),ffi.cast('const int64_t',S[8]),ffi.cast('const int64_t',S[9]) }\n"
    "function _G.__i64args_select(m) if m==0 then _G.__i64args_signed_values=S; _G.__i64args_unsigned_values=U; _G.__i64args_alias_values=S; _G.__i64args_const_values={ffi.cast('const int64_t',S[1]),ffi.cast('const int64_t',S[2]),ffi.cast('const int64_t',S[3]),ffi.cast('const int64_t',S[4]),ffi.cast('const int64_t',S[5]),ffi.cast('const int64_t',S[6]),ffi.cast('const int64_t',S[7]),ffi.cast('const int64_t',S[8]),ffi.cast('const int64_t',S[9])} else _G.__i64args_signed_values=S2; _G.__i64args_unsigned_values=U2; _G.__i64args_alias_values=S2; _G.__i64args_const_values={ffi.cast('const int64_t',S2[1]),ffi.cast('const int64_t',S2[2]),ffi.cast('const int64_t',S2[3]),ffi.cast('const int64_t',S2[4]),ffi.cast('const int64_t',S2[5]),ffi.cast('const int64_t',S2[6]),ffi.cast('const int64_t',S2[7]),ffi.cast('const int64_t',S2[8]),ffi.cast('const int64_t',S2[9])} end end\n"
    "local set_errno=__i64args_set_errno\n"
    "function _G.__i64args_driver(run,fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) set_errno(); local r=run(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8); return r end\n"
    "function _G.__i64args_pair_driver(run,source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8) set_errno(); local r=run(source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8); return r end\n"
    "function _G.__i64args_pair2_driver(run,source,consume,n,a1,a2,a3,a4,a5,a6,a7) set_errno(); local r=run(source,consume,n,a1,a2,a3,a4,a5,a6,a7); return r end\n"
    "function _G.__i64args_callm_driver(run,source,consume,n) set_errno(); local r=run(source,consume,n); return r end\n"
    "function _G.__i64args_vararg_driver(run,fn,n,box) set_errno(); local r=run(fn,n,box); return r end\n"
    "function _G.__i64args_run_fixed(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_unsigned(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_alias(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_const(fn,n,a0,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do r=fn(a0,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_vararg(fn,n,box) local r; for i=1,n do r=fn(box,i+0.5,true,'i64-args-vararg') end; return r end\n"
    "function _G.__i64args_run_boxed_pair(source,consume,n,a1,a2,a3,a4,a5,a6,a7,a8) local r; for i=1,n do local box=source(1); r=consume(box,a1,a2,a3,a4,a5,a6,a7,a8) end; return r end\n"
    "function _G.__i64args_run_boxed_pair2(source,consume,n,a1,a2,a3,a4,a5,a6,a7) local r; for i=1,n do local box1=source(1); local box2=source(2); r=consume(box1,box2,a1,a2,a3,a4,a5,a6,a7) end; return r end\n"
    "function _G.__i64args_callm_closed(source,consume,n) local r; for i=1,n do r=consume(17,source(1)) end; return r end\n"
    "_G.__i64args_callm_source=lib.lj_callxs_i64_args_source\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n"
    "if os.getenv('LJ_ARM64_I64_ARGS_DUMP') then require('jit.dump').on('biras') end\n");

  for (mode = 0; mode < 2; mode++) {
    lua_getglobal(L, "__i64args_select");
    lua_pushinteger(L, mode);
    assert(lua_pcall(L, 1, 0, 0) == 0);
    run_native_case(L, J, tg, "__i64args_run_fixed", "__i64args_fixed",
      "__i64args_signed_values", "__i64args_signed_count", (unsigned)mode,
      40, mode == 1, old_slot, old_ffi_func, old_stopreq);
    run_native_case(L, J, tg, "__i64args_run_unsigned", "__i64args_unsigned",
      "__i64args_unsigned_values", "__i64args_unsigned_count", (unsigned)mode,
      40, 0, old_slot, old_ffi_func, old_stopreq);
  }
  run_native_case(L, J, tg, "__i64args_run_alias", "__i64args_alias",
    "__i64args_alias_values", "__i64args_alias_count", 0, 40, 0,
    old_slot, old_ffi_func, old_stopreq);
  run_native_case(L, J, tg, "__i64args_run_const", "__i64args_const",
    "__i64args_const_values", "__i64args_const_count", 0, 40, 0,
    old_slot, old_ffi_func, old_stopreq);

  configure(L, 0);
  boxed_source_func = boxed_consumer_func = NULL;
  run_boxed_pair(L, J, tg, old_slot, old_ffi_func, old_stopreq);
  run_fixed_prefix_vararg(L, J, tg, old_slot, old_ffi_func, old_stopreq);

  /* A boxed CData CALLM vararg remains closed. Correct interpreter behavior
  ** is checked, and the runner must not publish a native CALLXS trace. */
  configure(L, 0);
  {
    GCproto *pt = global_proto(L, "__i64args_callm_closed");
    lua_getglobal(L, "__i64args_callm_driver");
    lua_getglobal(L, "__i64args_callm_closed");
    lua_getglobal(L, "__i64args_callm_source");
    lua_getglobal(L, "__i64args_callm_consumer");
    lua_pushinteger(L, 20);
    assert(lua_pcall(L, 4, 1, 0) == 0);
    assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 1);
    lua_pop(L, 1);
    assert(getter(L, "__i64args_callm_count") == 20);
    assert(getter(L, "__i64args_source_count") == 20);
    assert(!has_callxs_trace(J, pt));
  }

  lua_close(L);
  printf("fixed I64/U64 argument native observations: %u\n", total_native_calls);
  puts("t-arm64-jit-callxs-i64-args OK: fixed signed/unsigned I64 inputs, "
       "eight-register/stack split, typedef/const declarations, boxed CALL "
       "producer input, postcall epoch, errno and cleanup");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-i64-args SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
