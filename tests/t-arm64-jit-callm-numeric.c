/* Native NUM/FLOAT/U32 producer -> fixed-integer CALLM lifecycle coverage. */

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
#error "t-arm64-jit-callm-numeric requires ARM64 CALLXS admission"
#endif

enum { CALLM_NUM_KINDS = 8, CALLM_NUM_FAMILIES = 3, RUNS = 24 };

static const char *const family_names[CALLM_NUM_FAMILIES] = {
  "double", "float", "u32"
};
static const char *const kind_names[CALLM_NUM_KINDS] = {
  "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"
};
static GCproto *runner_protos[CALLM_NUM_FAMILIES][2][CALLM_NUM_KINDS];

static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;
static void *expected_producer, *expected_consumer, *force_func;
static uint32_t native_finishes, producer_finishes, consumer_finishes;
static uint32_t forced_finishes, oracle_native_finishes, recording_retries;
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
    unsigned calls;
    if (T == NULL || !trace_runnable_acq(T, no) || trace_root_acq(T) != 0 ||
        trace_startpt_acq(T) != pt ||
        bc_op(trace_startins_acq(T)) != BC_FORL) continue;
    calls = trace_op_count(T, IR_CALLXS);
    if (calls >= 2u && calls == trace_op_count(T, IR_XSAVE)) return T;
  }
  return NULL;
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
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  func = lj_ffi_native_frame_func_acq(frame);
  if (func == expected_producer) producer_finishes++;
  else {
    assert(func == expected_consumer);
    consumer_finishes++;
  }
  native_finishes++;
}

static void finish_or_force(TGState *tg)
{
  uint64_t epoch;
  void *func = lj_ffi_native_frame_func_acq(&tg->ffi_native_frame[0]);
  observe_finish(tg);
  if (func != force_func || forced_finishes != 0) return;
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  old_epoch = epoch;
  assert(epoch != UINT64_MAX);
  forced_finishes++;
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static void reject_oracle_native(TGState *tg)
{ UNUSED(tg); oracle_native_finishes++; }

static int set_errno_caller(lua_State *L)
{ UNUSED(L); errno = E2BIG; return 0; }

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "numeric CALLM fixture Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void set_value(lua_State *L, double value)
{
  lua_getglobal(L, "__callm_num_table");
  assert(lua_istable(L, -1));
  lua_pushnumber(L, value);
  lua_setfield(L, -2, "value");
  lua_pop(L, 1);
}

static void configure(lua_State *L, unsigned family, unsigned kind, int stack)
{
  lua_getglobal(L, "__callm_num_configure");
  lua_pushinteger(L, (lua_Integer)family);
  lua_pushinteger(L, (lua_Integer)kind);
  lua_pushboolean(L, stack);
  assert(lua_pcall(L, 3, 0, 0) == LUA_OK);
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

static lua_Integer getter0(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static void make_names(char *runner, size_t rcap, char *producer,
                       size_t pcap, char *consumer, size_t ccap,
                       unsigned family, unsigned kind, int stack)
{
  (void)snprintf(producer, pcap, "__callm_num_produce_%s", family_names[family]);
  if (stack) {
    (void)snprintf(runner, rcap, "__callm_num_run_stack_%s_%s",
                   family_names[family], kind_names[kind]);
    (void)snprintf(consumer, ccap, "__callm_num_stack_%s", kind_names[kind]);
  } else {
    (void)snprintf(runner, rcap, "__callm_num_run_%s_%s",
                   family_names[family], kind_names[kind]);
    (void)snprintf(consumer, ccap, "__callm_num_%s", kind_names[kind]);
  }
}

static void save_runner_protos(lua_State *L)
{
  unsigned family, stack, kind, f, s, k;
  for (family = 0; family < CALLM_NUM_FAMILIES; family++)
    for (stack = 0; stack < 2; stack++)
      for (kind = 0; kind < CALLM_NUM_KINDS; kind++) {
        char runner[80], producer[64], consumer[64];
        GCproto *pt;
        make_names(runner, sizeof(runner), producer, sizeof(producer),
                   consumer, sizeof(consumer), family, kind, (int)stack);
        pt = global_proto(L, runner);
        assert(pt != NULL);
        for (f = 0; f <= family; f++)
          for (s = 0; s < 2; s++)
            for (k = 0; k < CALLM_NUM_KINDS; k++) {
              if (f == family && s == stack && k >= kind) break;
              assert(runner_protos[f][s][k] != pt);
            }
        runner_protos[family][stack][kind] = pt;
      }
}

static int call_driver(lua_State *L, const char *runner,
                       const char *producer, const char *consumer,
                       lua_Integer n)
{
  lua_getglobal(L, "__callm_num_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__callm_num_table");
  lua_getglobal(L, producer);
  lua_getglobal(L, consumer);
  lua_pushinteger(L, n);
  return lua_pcall(L, 5, 1, 0);
}

static void set_jit(lua_State *L, int enabled)
{ run_lua(L, enabled ? "jit.on()" : "jit.off()"); }

static void assert_result(lua_State *L, int status)
{
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == (double)RUNS * 1.25);
  lua_pop(L, 1);
}

static void save_oracle(lua_State *L, unsigned family, unsigned kind,
                        int stack, double value)
{
  char runner[80], producer[64], consumer[64];
  const char *oracle = stack ? "__callm_num_oracle_stack" :
                               "__callm_num_oracle_unary";
  int status;
  make_names(runner, sizeof(runner), producer, sizeof(producer),
             consumer, sizeof(consumer), family, kind, stack);
  set_jit(L, 0);
  run_lua(L, "__callm_num_expect_enable(false)");
  configure(L, family, kind, stack);
  set_value(L, value);
  oracle_native_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(reject_oracle_native);
  errno = 0;
  status = call_driver(L, oracle, producer, consumer, RUNS);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert_result(L, status);
  assert(oracle_native_finishes == 0 && errno == EDOM);
  assert(getter1(L, "__callm_num_producer_count", family) == RUNS);
  assert(getter1(L, "__callm_num_consumer_count", kind) == RUNS);
  assert(getter1(L, "__callm_num_producer_first_errno", family) == E2BIG);
  assert(getter1(L, "__callm_num_producer_last_errno", family) == EDOM);
  assert(getter1(L, "__callm_num_consumer_first_errno", kind) == ERANGE);
  assert(getter1(L, "__callm_num_consumer_last_errno", kind) == ERANGE);
  run_lua(L, "__callm_num_save_expected(); __callm_num_expect_enable(true)");
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
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == expected_callback_stopreq);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(lj_tg_ffi_call_func_acq(tg) == expected_ffi_func);
  if (T != NULL) assert(trace_native_pins_acq(T) == 0);
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

static const BCIns *only_call(GCproto *pt, BCOp op)
{
  const BCIns *bc = proto_bc(pt), *found = NULL;
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == op) {
      assert(found == NULL);
      found = &bc[i];
    }
  }
  assert(found != NULL);
  return found;
}

static void assert_callm_pair(GCproto *pt)
{
  const BCIns *producer = only_call(pt, BC_CALL);
  const BCIns *consumer = only_call(pt, BC_CALLM);
  BCIns producer_ins = (BCIns)la_load32_acq((const uint32_t *)producer);
  BCIns consumer_ins = (BCIns)la_load32_acq((const uint32_t *)consumer);
  assert(bc_b(producer_ins) == 0 && consumer == producer + 1 &&
         (unsigned)bc_a(producer_ins) == (unsigned)bc_a(consumer_ins) +
           (unsigned)bc_c(consumer_ins) + 1u + LJ_FR2);
}

static void assert_postcall_exit(jit_State *J, GCtrace *T, GCproto *pt,
                                 BCOp op)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  GCtrace *exited = traceref_safe(J, parent);
  const SnapShot *snap;
  const BCIns *target = only_call(pt, op), *pc;
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
  assert((uint8_t)pcbase == 0 && pc == target + 1 &&
         bc_op((BCIns)la_load32_acq((const uint32_t *)target)) == op);
}

static void assert_expected(lua_State *L, unsigned family, unsigned kind)
{
  assert(getter0(L, "__callm_num_match_expected") == 1);
  assert(getter0(L, "__callm_num_mismatch_count") == 0);
  assert(getter1(L, "__callm_num_producer_count", family) == RUNS);
  assert(getter1(L, "__callm_num_consumer_count", kind) == RUNS);
  assert(getter1(L, "__callm_num_producer_first_errno", family) == E2BIG);
  assert(getter1(L, "__callm_num_producer_last_errno", family) == EDOM);
  assert(getter1(L, "__callm_num_consumer_first_errno", kind) == ERANGE);
  assert(getter1(L, "__callm_num_consumer_last_errno", kind) == ERANGE);
}

static GCtrace *run_native(lua_State *L, jit_State *J, TGState *tg,
                           unsigned family, unsigned kind, int stack,
                           double value, GCtrace *reuse, int force)
{
  char runner[80], producer[64], consumer[64];
  GCproto *pt;
  GCtrace *T;
  int status, saved_errno;
  unsigned attempt;
  make_names(runner, sizeof(runner), producer, sizeof(producer),
             consumer, sizeof(consumer), family, kind, stack);
  pt = global_proto(L, runner);
  assert(pt == runner_protos[family][stack][kind]);
  for (attempt = 0; ; attempt++) {
    if (reuse == NULL) run_lua(L, "jit.opt.start('hotloop=1')");
    configure(L, family, kind, stack);
    set_value(L, value);
    capture_cfunc(L, producer, &expected_producer);
    capture_cfunc(L, consumer, &expected_consumer);
    expected_L = L;
    expected_tg = tg;
    expected_trace = reuse;
    native_finishes = producer_finishes = consumer_finishes = 0;
    force_func = force == 1 ? expected_producer :
                 force == 2 ? expected_consumer : NULL;
    forced_finishes = 0;
    old_epoch = UINT64_MAX;
    assert_callm_pair(pt);
    lj_trace_test_reset_exit_stats();
    lj_ffi_native_trace_test_set_finish_hook(force ? finish_or_force : observe_finish);
    errno = 0;
    status = call_driver(L, runner, producer, consumer, RUNS);
    saved_errno = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_result(L, status);
    assert(saved_errno == EDOM);
    T = find_trace(J, pt);
    if (reuse != NULL || native_finishes != 0 || attempt == 7u) break;
    /* A GC phase or handshake can abort cold recording without a Lua error.
    ** Retrying does not waive the bit/count oracle or any native-run checks. */
    assert_expected(L, family, kind);
    assert_clean(tg, T);
    recording_retries++;
  }
  if (T == NULL || native_finishes == 0 || expected_trace != T)
    fprintf(stderr, "numeric CALLM trace missing: family=%s kind=%s stack=%d "
            "value=%.17g native=%u trace=%p observed=%p\n",
            family_names[family], kind_names[kind], stack, value,
            native_finishes, (void *)T, (void *)expected_trace);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  if (force == 1) assert(producer_finishes > 0);
  else if (force == 2) assert(producer_finishes > 0 && consumer_finishes > 0);
  else assert(producer_finishes > 0 && consumer_finishes > 0);
  assert(trace_op_count(T, IR_CALLXS) == trace_op_count(T, IR_XSAVE));
  assert_expected(L, family, kind);
  if (force) {
    assert(forced_finishes == 1 && old_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() > 0);
    assert_postcall_exit(J, T, pt, force == 1 ? BC_CALL : BC_CALLM);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
  }
  assert_clean(tg, T);
  return T;
}

static void exercise_case(lua_State *L, jit_State *J, TGState *tg,
                          unsigned family, unsigned kind, int stack,
                          const double *values, size_t nvalues)
{
  const double base = -3.75;
  char runner[80], producer[64], consumer[64];
  GCtrace *T;
  size_t i;
  make_names(runner, sizeof(runner), producer, sizeof(producer),
             consumer, sizeof(consumer), family, kind, stack);
  save_oracle(L, family, kind, stack, base);
  set_jit(L, 1);
  T = run_native(L, J, tg, family, kind, stack, base, NULL, 0);
  for (i = 0; i < nvalues; i++) {
    save_oracle(L, family, kind, stack, values[i]);
    set_jit(L, 1);
    T = run_native(L, J, tg, family, kind, stack, values[i], T, 0);
  }
  save_oracle(L, family, kind, stack, base);
  set_jit(L, 1);
  T = run_native(L, J, tg, family, kind, stack, base, T, 1);
  T = run_native(L, J, tg, family, kind, stack, base, T, 0);
  T = run_native(L, J, tg, family, kind, stack, base, T, 2);
  run_native(L, J, tg, family, kind, stack, base, T, 0);
}

static void install_lua(lua_State *L)
{
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callm_numeric_configure(int32_t,int32_t,int32_t);\n"
    "void lj_callm_numeric_expected_enable(int32_t);\n"
    "void lj_callm_numeric_save_expected(void);\n"
    "int32_t lj_callm_numeric_mismatch_count(void);\n"
    "int32_t lj_callm_numeric_match_expected(void);\n"
    "int32_t lj_callm_numeric_producer_count(int32_t);\n"
    "int32_t lj_callm_numeric_consumer_count(int32_t);\n"
    "int32_t lj_callm_numeric_producer_first_errno(int32_t);\n"
    "int32_t lj_callm_numeric_producer_last_errno(int32_t);\n"
    "int32_t lj_callm_numeric_consumer_first_errno(int32_t);\n"
    "int32_t lj_callm_numeric_consumer_last_errno(int32_t);\n"
    "double lj_callm_numeric_producer_double(double);\n"
    "float lj_callm_numeric_producer_float(float);\n"
    "uint32_t lj_callm_numeric_producer_u32(uint32_t);\n"
    "double lj_callm_numeric_i8(int8_t); double lj_callm_numeric_u8(uint8_t);\n"
    "double lj_callm_numeric_i16(int16_t); double lj_callm_numeric_u16(uint16_t);\n"
    "double lj_callm_numeric_i32(int32_t); double lj_callm_numeric_u32(uint32_t);\n"
    "double lj_callm_numeric_i64(int64_t); double lj_callm_numeric_u64(uint64_t);\n"
    "double lj_callm_numeric_stack_i8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int8_t);\n"
    "double lj_callm_numeric_stack_u8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint8_t);\n"
    "double lj_callm_numeric_stack_i16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int16_t);\n"
    "double lj_callm_numeric_stack_u16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint16_t);\n"
    "double lj_callm_numeric_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t);\n"
    "double lj_callm_numeric_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t);\n"
    "double lj_callm_numeric_stack_i64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int64_t);\n"
    "double lj_callm_numeric_stack_u64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint64_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLM_NUMERIC_SO')))\n"
    "local families={'double','float','u32'}\n"
    "local kinds={'i8','u8','i16','u16','i32','u32','i64','u64'}\n"
    "for _,family in ipairs(families) do\n"
    "  _G['__callm_num_produce_'..family]=lib['lj_callm_numeric_producer_'..family]\n"
    "  for _,kind in ipairs(kinds) do\n"
    "    local consumer=lib['lj_callm_numeric_'..kind]\n"
    "    local stackconsumer=lib['lj_callm_numeric_stack_'..kind]\n"
    "    _G['__callm_num_'..kind]=consumer; _G['__callm_num_stack_'..kind]=stackconsumer\n"
    "    _G['__callm_num_run_'..family..'_'..kind]=assert(loadstring('return function(t,produce,consume,n) local s=.5; for i=1,n do s=s+consume(produce(t.value)) end; return s-.5 end'))()\n"
    "    _G['__callm_num_run_stack_'..family..'_'..kind]=assert(loadstring('return function(t,produce,consume,n) local s=.5; for i=1,n do s=s+consume(101,102,103,104,105,106,107,108,31,-505,1985229328,produce(t.value)) end; return s-.5 end'))()\n"
    "  end\n"
    "end\n"
    "function _G.__callm_num_configure(f,k,s) lib.lj_callm_numeric_configure(f,k,s and 1 or 0) end\n"
    "function _G.__callm_num_expect_enable(v) lib.lj_callm_numeric_expected_enable(v and 1 or 0) end\n"
    "function _G.__callm_num_save_expected() lib.lj_callm_numeric_save_expected() end\n"
    "function _G.__callm_num_mismatch_count() return lib.lj_callm_numeric_mismatch_count() end\n"
    "function _G.__callm_num_match_expected() return lib.lj_callm_numeric_match_expected() end\n"
    "function _G.__callm_num_producer_count(f) return lib.lj_callm_numeric_producer_count(f) end\n"
    "function _G.__callm_num_consumer_count(k) return lib.lj_callm_numeric_consumer_count(k) end\n"
    "function _G.__callm_num_producer_first_errno(f) return lib.lj_callm_numeric_producer_first_errno(f) end\n"
    "function _G.__callm_num_producer_last_errno(f) return lib.lj_callm_numeric_producer_last_errno(f) end\n"
    "function _G.__callm_num_consumer_first_errno(k) return lib.lj_callm_numeric_consumer_first_errno(k) end\n"
    "function _G.__callm_num_consumer_last_errno(k) return lib.lj_callm_numeric_consumer_last_errno(k) end\n"
    "_G.__callm_num_table={value=-3.75}\n"
    "local function oracle_unary(t,produce,consume,n) local s=.5; for i=1,n do s=s+consume(produce(t.value)) end; return s-.5 end\n"
    "local function oracle_stack(t,produce,consume,n) local s=.5; for i=1,n do s=s+consume(101,102,103,104,105,106,107,108,31,-505,1985229328,produce(t.value)) end; return s-.5 end\n"
    "jit.off(oracle_unary,true); jit.off(oracle_stack,true)\n"
    "_G.__callm_num_oracle_unary=oracle_unary; _G.__callm_num_oracle_stack=oracle_stack\n"
    "function _G.__callm_num_driver(run,t,produce,consume,n) __callm_num_set_errno(); local result=run(t,produce,consume,n); return result end\n"
    "local threading=require('threading'); _G.__callm_num_ready=threading.channel(1); _G.__callm_num_release=threading.channel(1)\n"
    "_G.__callm_num_peer=assert(threading.spawn(function(ready,release) assert(ready:send(true,5)); assert(select(2,release:recv(60))); return true end,__callm_num_ready,__callm_num_release))\n"
    "assert(select(2,__callm_num_ready:recv(5)))\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=256')\n");
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
  const double *sets[CALLM_NUM_KINDS] = {
    values8, values8, values16, values16, values32, values32,
    values64, values64
  };
  const size_t counts[CALLM_NUM_KINDS] = {
    sizeof(values8)/sizeof(values8[0]), sizeof(values8)/sizeof(values8[0]),
    sizeof(values16)/sizeof(values16[0]), sizeof(values16)/sizeof(values16[0]),
    sizeof(values32)/sizeof(values32[0]), sizeof(values32)/sizeof(values32[0]),
    sizeof(values64)/sizeof(values64[0]), sizeof(values64)/sizeof(values64[0])
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  unsigned family, kind;
  expected_callback_slot = ccallback_slot_acq(&tg->cb);
  expected_callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  expected_ffi_func = lj_tg_ffi_call_func_acq(tg);
  expected_L = L;
  expected_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__callm_num_set_errno");
  install_lua(L);
  save_runner_protos(L);
  assert(mt_active_acq(g) != 0);
  set_jit(L, 0);
  for (family = 0; family < CALLM_NUM_FAMILIES; family++)
    for (kind = 0; kind < CALLM_NUM_KINDS; kind++) {
      exercise_case(L, J, tg, family, kind, 0, sets[kind], counts[kind]);
      exercise_case(L, J, tg, family, kind, 1, sets[kind], counts[kind]);
    }
  run_lua(L, "assert(__callm_num_release:send(true,5)); local ok,v=__callm_num_peer:join(5); assert(ok and v)");
  assert(mt_active_acq(g) != 0);
  lua_close(L);
  printf("numeric CALLM cold recording retries: %u\n", recording_retries);
  puts("t-arm64-jit-callm-numeric OK: NUM/FLOAT/U32 producer to fixed integer "
       "CALLM, stack args, oracle bits, postcall exits and cleanup");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callm-numeric SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
