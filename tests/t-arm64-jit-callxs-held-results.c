/* Native coverage for scalar CALLXS results held across a later CALLXS. */

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

#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-held-results requires ARM64 CALLXS admission"
#endif

static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;
static void *expected_producer;
static void *expected_consumer;
static void *force_func;
static uint32_t native_finishes;
static uint32_t native_producer_finishes;
static uint32_t native_consumer_finishes;
static uint32_t forced_finishes;
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

static unsigned trace_op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == op)
      count++;
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
        trace_op_count(T, IR_CALLXS) == 4 &&
        trace_op_count(T, IR_XSAVE) == 4)
      return T;
  }
  return NULL;
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
  if (func == expected_producer)
    native_producer_finishes++;
  else {
    assert(func == expected_consumer);
    native_consumer_finishes++;
  }
  native_finishes++;
}

static void finish_or_force(TGState *tg)
{
  uint64_t epoch;
  void *func = lj_ffi_native_frame_func_acq(&tg->ffi_native_frame[0]);
  observe_finish(tg);
  if (func != force_func || forced_finishes != 0)
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

static lua_Integer getter(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == 0 && lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static void configure(lua_State *L, uint32_t mode)
{
  lua_getglobal(L, "__held_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == 0);
}

static double expected_sum(unsigned kind, uint32_t mode, uint32_t n)
{
  static const uint32_t values[] = {
    UINT32_C(0), UINT32_C(1), UINT32_C(0x7fffffff), UINT32_C(0x80000000),
    UINT32_C(0xffffffff), UINT32_C(0xaaaaaaaa), UINT32_C(0x55555555),
    UINT32_C(0xdeadbeef)
  };
  double sum = 0.0;
  uint32_t i;
  for (i = 1; i <= n; i++) {
    if (kind == 0) {
      double value = (double)((int32_t)i - 17);
      sum += (value * 3.0 + 1.0) + value;
    } else if (kind == 1) {
      double value = (double)i + 0.125;
      sum += (value * 2.0 + 0.25) + value;
    } else if (kind == 2) {
      float value = (float)i + 0.375f;
      float result = value * 0.5f + 0.25f;
      sum += (double)result + (double)value;
    } else {
      double value = (double)values[(mode + i) & 7u];
      sum += (value + 0.5) + value;
    }
  }
  return sum;
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

static unsigned call_ordinal(GCproto *pt, unsigned wanted, const BCIns **callpc)
{
  const BCIns *bc = proto_bc(pt);
  MSize i;
  unsigned found = 0;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALL) {
      if (found == wanted) {
        *callpc = &bc[i];
        return 1;
      }
      found++;
    }
  }
  return 0;
}

static void assert_postcall_exit(jit_State *J, GCtrace *T, GCproto *pt,
                                 unsigned which_call)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  GCtrace *exited = traceref_safe(J, parent);
  const SnapShot *snap;
  const BCIns *callpc, *pc;
  MSize ofs, nent;
  uint64_t pcbase;
  assert(exited == T && exitno < trace_nsnap_acq(T));
  assert(call_ordinal(pt, which_call, &callpc));
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

static int run_case(lua_State *L, jit_State *J, TGState *tg,
                    const char *runner, const char *producer,
                    const char *consumer, unsigned kind, uint32_t mode,
                    uint32_t n, int force_which, GCtrace **tracep)
{
  GCtrace *T;
  GCproto *pt;
  int status, saved_errno;
  configure(L, mode);
  capture_cfunc(L, producer, &expected_producer);
  capture_cfunc(L, consumer, &expected_consumer);
  expected_tg = tg;
  expected_L = L;
  expected_trace = NULL;
  native_finishes = native_producer_finishes = native_consumer_finishes = 0;
  force_func = force_which == 0 ? NULL :
               force_which == 1 ? expected_producer : expected_consumer;
  forced_finishes = 0;
  old_epoch = UINT64_MAX;
  pt = global_proto(L, runner);
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_which ? finish_or_force :
                                                   observe_finish);
  lua_getglobal(L, "__held_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, producer);
  lua_getglobal(L, consumer);
  lua_pushinteger(L, n);
  errno = 0;
  status = lua_pcall(L, 4, 1, 0);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == 0 && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == expected_sum(kind, mode, n));
  lua_pop(L, 1);
  assert(saved_errno == EDOM);
  T = find_trace(J, pt);
  if (T == NULL || (expected_trace != NULL && T != expected_trace))
    fprintf(stderr, "held CALLXS trace missing/mismatched: %s mode=%u "
            "native=%u published=%p observed=%p\n", runner,
            (unsigned)mode, (unsigned)native_finishes, (void *)T,
            (void *)expected_trace);
  assert(T != NULL && native_finishes > 0 && expected_trace == T);
  assert(native_producer_finishes > 0 && native_consumer_finishes > 0);
  assert(getter(L, "__held_producer_count") == (lua_Integer)n);
  assert(getter(L, "__held_consumer_count") == (lua_Integer)n);
  assert(getter(L, "__held_total_count") == (lua_Integer)(2u * n));
  assert(getter(L, "__held_first_errno") == E2BIG);
  assert(getter(L, "__held_last_errno") == EDOM);
  if (force_which) {
    assert(forced_finishes == 1 && old_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() > 0);
    assert_postcall_exit(J, T, pt, force_which == 1 ? 0u : 1u);
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
  }
  assert_clean(tg, T);
  *tracep = T;
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T = NULL, *traces[4] = { NULL, NULL, NULL, NULL };
  static const char *const runners[4] = {
    "__held_run_int", "__held_run_num", "__held_run_float",
    "__held_run_u32"
  };
  static const char *const producers[4] = {
    "__held_i32_producer", "__held_num_producer",
    "__held_float_producer", "__held_u32_producer"
  };
  static const char *const consumers[4] = {
    "__held_i32_consumer", "__held_num_consumer",
    "__held_float_consumer", "__held_u32_consumer"
  };
  expected_callback_slot = ccallback_slot_acq(&tg->cb);
  expected_callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  expected_ffi_func = lj_tg_ffi_call_func_acq(tg);
  expected_tg = tg;
  expected_L = L;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__held_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_held_configure(int32_t);\n"
    "int32_t lj_callxs_held_producer_count(void);\n"
    "int32_t lj_callxs_held_consumer_count(void);\n"
    "int32_t lj_callxs_held_total_count(void);\n"
    "int32_t lj_callxs_held_first_errno(void);\n"
    "int32_t lj_callxs_held_last_errno(void);\n"
    "int32_t lj_callxs_held_i32(int32_t);\n"
    "int32_t lj_callxs_held_i32_consume(int32_t);\n"
    "double lj_callxs_held_num(int32_t);\n"
    "double lj_callxs_held_num_consume(double);\n"
    "float lj_callxs_held_float(int32_t);\n"
    "float lj_callxs_held_float_consume(float);\n"
    "uint32_t lj_callxs_held_u32(int32_t);\n"
    "double lj_callxs_held_u32_consume(double);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_HELD_RESULTS_SO')))\n"
    "_G.__held_i32_producer=lib.lj_callxs_held_i32\n"
    "_G.__held_i32_consumer=lib.lj_callxs_held_i32_consume\n"
    "_G.__held_num_producer=lib.lj_callxs_held_num\n"
    "_G.__held_num_consumer=lib.lj_callxs_held_num_consume\n"
    "_G.__held_float_producer=lib.lj_callxs_held_float\n"
    "_G.__held_float_consumer=lib.lj_callxs_held_float_consume\n"
    "_G.__held_u32_producer=lib.lj_callxs_held_u32\n"
    "_G.__held_u32_consumer=lib.lj_callxs_held_u32_consume\n"
    "function _G.__held_configure(mode) lib.lj_callxs_held_configure(mode) end\n"
    "function _G.__held_producer_count() return lib.lj_callxs_held_producer_count() end\n"
    "function _G.__held_consumer_count() return lib.lj_callxs_held_consumer_count() end\n"
    "function _G.__held_total_count() return lib.lj_callxs_held_total_count() end\n"
    "function _G.__held_first_errno() return lib.lj_callxs_held_first_errno() end\n"
    "function _G.__held_last_errno() return lib.lj_callxs_held_last_errno() end\n"
    "function _G.__held_run_int(producer,consumer,n)\n"
    "  local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5\n"
    "end\n"
    "function _G.__held_run_num(producer,consumer,n)\n"
    "  local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5\n"
    "end\n"
    "function _G.__held_run_float(producer,consumer,n)\n"
    "  local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5\n"
    "end\n"
    "function _G.__held_run_u32(producer,consumer,n)\n"
    "  local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5\n"
    "end\n"
    "function _G.__held_driver(run,producer,consumer,n)\n"
    "  __held_set_errno(); local result=run(producer,consumer,n); return result\n"
    "end\n"
    "local threading=require('threading')\n"
    "_G.__held_ready=threading.channel(1); _G.__held_release=threading.channel(1)\n"
    "_G.__held_peer=assert(threading.spawn(function(ready,release)\n"
    "  assert(ready:send(true,5))\n"
    "  assert(select(2,release:recv(120))); return true\n"
    "end,__held_ready,__held_release))\n"
    "assert(select(2,__held_ready:recv(5)))\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  assert(mt_active_acq(g) != 0);
  run_case(L, J, tg, "__held_run_int", "__held_i32_producer",
           "__held_i32_consumer", 0, 0, 32, 0, &T);
  traces[0] = T;
  run_case(L, J, tg, "__held_run_num", "__held_num_producer",
           "__held_num_consumer", 1, 0, 32, 0, &T);
  traces[1] = T;
  run_case(L, J, tg, "__held_run_float", "__held_float_producer",
           "__held_float_consumer", 2, 0, 32, 0, &T);
  traces[2] = T;
  for (uint32_t mode = 0; mode < 8; mode++)
    run_case(L, J, tg, "__held_run_u32", "__held_u32_producer",
             "__held_u32_consumer", 3, mode, 32, 0, &T);
  traces[3] = T;

  /* Force one exit after the producer and another after the consumer. The
  ** callback targets the exact frame function and bumps the epoch only once.
  ** Re-run each same prototype after restoring the epoch to prove reuse. */
  for (unsigned kind = 0; kind < 4; kind++) {
    uint32_t mode = 0;
    assert(traces[kind] != NULL &&
           trace_runnable_acq(traces[kind], trace_traceno_acq(traces[kind])));
    run_case(L, J, tg, runners[kind], producers[kind], consumers[kind],
             kind, mode, 24, 1, &T);
    assert(T == traces[kind] &&
           trace_runnable_acq(traces[kind], trace_traceno_acq(traces[kind])));
    run_case(L, J, tg, runners[kind], producers[kind], consumers[kind],
             kind, mode, 24, 0, &T);
    assert(T == traces[kind]);
    run_case(L, J, tg, runners[kind], producers[kind], consumers[kind],
             kind, mode, 24, 2, &T);
    assert(T == traces[kind] &&
           trace_runnable_acq(traces[kind], trace_traceno_acq(traces[kind])));
    run_case(L, J, tg, runners[kind], producers[kind], consumers[kind],
             kind, mode, 24, 0, &T);
    assert(T == traces[kind]);
  }

  ljt_lua_dostring(L,
    "assert(__held_release:send(true,5))\n"
    "local ok,result=__held_peer:join(5); assert(ok and result)\n");
  /* Threading activation remains latched after the peer has joined. */
  assert(mt_active_acq(g) != 0);
  lua_close(L);
  puts("t-arm64-jit-callxs-held-results OK: held INT/NUM/FLOAT/U32 results, "
       "native frames, postcall exits, no replay and cleanup verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-held-results SKIP: requires experimental Darwin "
       "ARM64 CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
