/* Native fixture for Lua INT -> fixed ARM64 CALLXS integer arguments.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) &&      \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) &&                                      \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) &&                               \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lib/lua_fixture_helpers.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

enum { KINDS = 8, FORMS = 3, RUNS = 24 };
static const char *const suffix[KINDS] = {"i8",  "u8",  "i16", "u16",
                                          "i32", "u32", "i64", "u64"};
static TGState *want_tg;
static lua_State *want_L;
static GCtrace *want_trace;
static void *want_func, *force_func, *wanted_consumer, *wanted_producer;
static int wanted_form;
static uint32_t native_finishes, native_consumers, native_producers;
static uint32_t forced_finishes, oracle_finishes;
static unsigned required_runs, forced_runs, cold_retries;
static uint64_t old_epoch = UINT64_MAX;
static MSize original_callback_slot;
static uint8_t original_stopreq;
static void *original_ffi_func;

static void run_lua(lua_State *L, const char *src)
{
  int status = luaL_dostring(L, src);
  if (status != LUA_OK) {
    fprintf(stderr, "int CALLXS Lua error: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}
static int set_errno_caller(lua_State *L)
{
  UNUSED(L);
  errno = E2BIG;
  return 0;
}

static void capture_func(lua_State *L, const char *name, void **out)
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
static unsigned op_count(GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r;
  unsigned n = 0;
  for (r = REF_FIRST; r < trace_nins_acq(T); r++)
    if (ir_load_acq(&ir[r]).o == op)
      n++;
  return n;
}
static GCtrace *find_loop_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) > 0 &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}
static void reject_oracle(TGState *tg)
{
  UNUSED(tg);
  oracle_finishes++;
}
static void observe_finish(TGState *tg)
{
  LJFFINativeFrame *f;
  assert(tg == want_tg && want_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  f = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(f) &
          (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
           LJ_FFI_NATIVE_FRAME_F_POSTCALL)) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert(lj_ffi_native_frame_flags_acq(f) & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED);
  assert(lj_ffi_native_frame_L_acq(f) == want_L);
  assert(lj_ffi_native_frame_result_root_acq(f) == NULL);
  if (want_trace == NULL)
    want_trace = lj_ffi_native_frame_trace_acq(f);
  assert(want_trace && lj_ffi_native_frame_trace_acq(f) == want_trace);
  assert(trace_native_pins_acq(want_trace) != 0);
  if (wanted_consumer) {
    void *actual = lj_ffi_native_frame_func_acq(f);
    assert(actual == wanted_consumer ||
           (wanted_form == 2 && actual == wanted_producer));
    if (actual == wanted_consumer)
      native_consumers++;
    if (wanted_form == 2 && actual == wanted_producer)
      native_producers++;
  }
  if (want_func &&
      (!force_func || lj_ffi_native_frame_func_acq(f) == force_func))
    assert(lj_ffi_native_frame_func_acq(f) == want_func);
  native_finishes++;
  if (force_func && lj_ffi_native_frame_func_acq(f) == force_func &&
      forced_finishes == 0) {
    old_epoch = lj_tg_hs_epoch_ack_acq(tg);
    assert(old_epoch != UINT64_MAX);
    forced_finishes++;
    lj_tg_hs_epoch_ack_rel(tg, old_epoch + 1u);
  }
}
static void set_jit(lua_State *L, int on)
{
  run_lua(L, on ? "jit.on()" : "jit.off()");
}
static void set_table_int(lua_State *L, lua_Integer v)
{
  lua_getglobal(L, "__int_arg_table");
  assert(lua_istable(L, -1));
  lua_pushinteger(L, v);
  assert(tvisint(L->top - 1));
  lua_setfield(L, -2, "value");
  lua_getfield(L, -1, "value");
  assert(tvisint(L->top - 1));
  lua_pop(L, 1);
  lua_pop(L, 1);
}
static void call_configure(lua_State *L, unsigned kind, int stack)
{
  lua_getglobal(L, "__int_arg_configure");
  lua_pushinteger(L, kind);
  lua_pushboolean(L, stack);
  assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
}
static lua_Integer get_int(lua_State *L, const char *name, int arg)
{
  lua_Integer v;
  lua_getglobal(L, name);
  lua_pushinteger(L, arg);
  assert(lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  v = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}
static void assert_helper_state(lua_State *L, unsigned kind, int form)
{
  assert(get_int(L, "__int_arg_count", kind) == RUNS);
  assert(get_int(L, "__int_arg_mismatch_count", 0) == 0);
  assert(get_int(L, "__int_arg_first_errno", kind) ==
         (form == 2 ? ERANGE : E2BIG));
  assert(get_int(L, "__int_arg_last_errno", kind) ==
         (form == 2 ? ERANGE : EDOM));
  if (form == 2) {
    assert(get_int(L, "__int_arg_producer_count", 0) == RUNS);
    assert(get_int(L, "__int_arg_producer_first_errno", 0) == E2BIG);
    assert(get_int(L, "__int_arg_producer_last_errno", 0) == EDOM);
  } else
    assert(get_int(L, "__int_arg_producer_count", 0) == 0);
}
static int driver(lua_State *L, const char *driver_name, const char *runner,
                  lua_Integer value, const char *consumer, int n)
{
  lua_getglobal(L, driver_name);
  lua_getglobal(L, runner);
  lua_getglobal(L, "__int_arg_table");
  lua_pushinteger(L, value);
  assert(tvisint(L->top - 1));
  lua_getglobal(L, consumer);
  lua_getglobal(L, "__int_arg_producer");
  lua_pushinteger(L, n);
  return lua_pcall(L, 6, 1, 0);
}
static void assert_return(lua_State *L, int status)
{
  assert(status == LUA_OK && lua_isnumber(L, -1) &&
         lua_tonumber(L, -1) == 30.0);
  lua_pop(L, 1);
}
static void assert_clean(TGState *tg, GCtrace *T)
{
  unsigned i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0 &&
         lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0 && lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
         tg->ffi_xsave_nslots == 0);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == original_callback_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == original_stopreq);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0 &&
         ccallback_L_acq(&tg->cb) == NULL);
  assert(lj_tg_ffi_call_func_acq(tg) == original_ffi_func);
  if (T)
    assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    LJFFINativeFrame *f = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(f) == NULL &&
           lj_ffi_native_frame_L_acq(f) == NULL);
    assert(lj_ffi_native_frame_func_acq(f) == NULL &&
           lj_ffi_native_frame_flags_acq(f) == 0);
  }
}
static const BCIns *call_ins(GCproto *pt, int form, int producer)
{
  const BCIns *bc = proto_bc(pt), *found = NULL;
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    int match = producer    ? bc_op(ins) == BC_CALL
                : form == 2 ? bc_op(ins) == BC_CALLM
                            : bc_op(ins) == BC_CALL;
    if (match) {
      assert(found == NULL);
      found = &bc[i];
    }
  }
  assert(found);
  return found;
}
static void assert_runner_calls(GCproto *pt, int form)
{
  if (form == 2) {
    const BCIns *producer = call_ins(pt, form, 1);
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)producer);
    assert(bc_b(ins) == 0);
    assert(bc_op((BCIns)la_load32_acq((const uint32_t *)(producer + 1))) ==
           BC_CALLM);
    (void)call_ins(pt, form, 0);
  } else {
    (void)call_ins(pt, form, 0);
  }
}
static void assert_exit_after_call(jit_State *J, GCtrace *T, GCproto *pt,
                                   int form, int producer)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo ex = lj_trace_test_first_exitno();
  GCtrace *out = traceref_safe(J, parent);
  const SnapShot *s;
  MSize ofs, ne;
  uint64_t pcbase;
  const BCIns *at = call_ins(pt, form, producer), *pc;
  assert(out == T && ex < trace_nsnap_acq(T));
  s = &trace_snap_acq(T)[ex];
  ofs = snap_mapofs_acq(s);
  ne = snap_nent_acq(s);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         ne + 1u + LJ_FR2 <= trace_nsnapmap_acq(T) - ofs);
  pc = snap_pc_acq(&trace_snapmap_acq(T)[ofs + ne]);
#if LJ_FR2
  memcpy(&pcbase, &trace_snapmap_acq(T)[ofs + ne], sizeof(pcbase));
  assert((pcbase & UINT64_C(0xff)) == 0);
#else
  UNUSED(pcbase);
#endif
  assert(pc == at + 1);
}
static void set_case_global(lua_State *L, const char *id, const char *which)
{
  char name[96];
  snprintf(name, sizeof(name), "__int_arg_%s_%s", which, id);
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  lua_pop(L, 1);
}

static void invoke_case(lua_State *L, jit_State *J, TGState *tg, unsigned kind,
                        int form, int stack, const char *id, lua_Integer value,
                        int force_target)
{
  char candidate[96], oracle[96], consumer[64];
  GCproto *pt;
  GCtrace *T = NULL;
  int status, attempt, attempt_limit, saved_errno;
  lua_Integer n;
  snprintf(candidate, sizeof(candidate), "__int_arg_run_%s", id);
  snprintf(oracle, sizeof(oracle), "__int_arg_oracle_%s", id);
  snprintf(consumer, sizeof(consumer), "__int_arg_%s%s", stack ? "stack_" : "",
           suffix[kind]);
  set_case_global(L, id, "run");
  set_case_global(L, id, "oracle");
  pt = global_proto(L, candidate);
  assert_runner_calls(pt, form);
  call_configure(L, kind, stack);
  set_table_int(L, value);
  assert(pt != global_proto(L, oracle));
  capture_func(L, consumer, &wanted_consumer);
  capture_func(L, "__int_arg_producer", &wanted_producer);
  wanted_form = form;
  set_jit(L, 0);
  run_lua(L, "__int_arg_expect_enable(false)");
  oracle_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(reject_oracle);
  errno = 0;
  status = driver(L, "__int_arg_driver", oracle, value, consumer, RUNS);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert_return(L, status);
  assert(oracle_finishes == 0 && saved_errno == EDOM);
  assert(find_loop_trace(J, global_proto(L, oracle)) == NULL);
  assert_helper_state(L, kind, form);
  run_lua(L, "__int_arg_save_expected(); __int_arg_expect_enable(true)");
  set_jit(L, 1);
  T = find_loop_trace(J, pt);
  attempt_limit = T ? 1 : 8;
  for (attempt = 0; attempt < attempt_limit; attempt++) {
    if (find_loop_trace(J, pt) == NULL)
      run_lua(L, "jit.opt.start('hotloop=1','hotexit=1000')");
    call_configure(L, kind, stack);
    set_table_int(L, value);
    want_tg = tg;
    want_L = L;
    want_trace = T;
    want_func = NULL;
    force_func = NULL;
    native_finishes = native_consumers = native_producers = 0;
    forced_finishes = 0;
    old_epoch = UINT64_MAX;
    lj_trace_test_reset_exit_stats();
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__int_arg_driver", candidate, value, consumer, RUNS);
    n = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(n == EDOM);
    assert_helper_state(L, kind, form);
    {
      GCtrace *published = find_loop_trace(J, pt);
      assert_clean(tg, published);
      if (T == NULL)
        T = published;
      else
        assert(published == T);
    }
    if (T && native_finishes)
      break;
  }
  if (T == NULL || native_finishes == 0)
    fprintf(stderr, "no native INT argument trace: %s\n", id);
  assert(T && native_finishes > 0 && want_trace == T &&
         bc_op(trace_startins_acq(T)) == BC_FORL);
  assert(native_consumers > 0);
  if (form == 2)
    assert(native_producers > 0);
  assert_clean(tg, T);
  required_runs++;
  cold_retries += (unsigned)attempt;
  if (force_target) {
    /* The exact-call POSTCALL case is performed after the candidate source
    ** and prototype have published; one epoch change forces a resumable exit.
    * */
    call_configure(L, kind, stack);
    set_table_int(L, value);
    capture_func(L, consumer, &want_func);
    if (force_target == 2)
      capture_func(L, "__int_arg_producer", &force_func);
    else
      force_func = want_func;
    want_func = force_func;
    want_trace = T;
    native_finishes = native_consumers = native_producers = 0;
    forced_finishes = 0;
    old_epoch = UINT64_MAX;
    lj_trace_test_reset_exit_stats();
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__int_arg_driver", candidate, value, consumer, RUNS);
    n = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(n == EDOM && native_finishes > 0 && forced_finishes == 1 &&
           old_epoch != UINT64_MAX);
    if (force_target == 2)
      assert(native_producers > 0);
    else
      assert(native_consumers > 0);
    assert_helper_state(L, kind, form);
    assert(lj_trace_test_exit_calls() > 0);
    assert_exit_after_call(J, T, pt, form, force_target == 2);
    required_runs++;
    forced_runs++;
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert_clean(tg, T);
    /* Reusing the same published trace proves the reconstructed continuation
    ** does not replay either foreign side effect. */
    call_configure(L, kind, stack);
    want_trace = T;
    want_func = NULL;
    force_func = NULL;
    native_finishes = native_consumers = native_producers = 0;
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__int_arg_driver", candidate, value, consumer, RUNS);
    n = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(n == EDOM && native_finishes > 0);
    assert_helper_state(L, kind, form);
    assert(native_consumers > 0);
    if (form == 2)
      assert(native_producers > 0);
    assert_clean(tg, T);
    required_runs++;
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  unsigned k, f, s;
  char id[48];
  GCproto *seen[KINDS * FORMS * 2];
  GCproto *seen_oracle[KINDS * FORMS * 2];
  unsigned nseen = 0, noracle = 0, si;
  original_callback_slot = ccallback_slot_acq(&tg->cb);
  original_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  original_ffi_func = lj_tg_ffi_call_func_acq(tg);
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__int_arg_set_errno");
  run_lua(L, "jit.off()");
  run_lua(
      L,
      "local ffi=require('ffi')\n"
      "ffi.cdef[[void lj_ffi_int_configure(int32_t,int32_t); void "
      "lj_ffi_int_expect_enable(int32_t); void lj_ffi_int_save_expected(void); "
      "int32_t lj_ffi_int_mismatch_count(void); int32_t "
      "lj_ffi_int_count(int32_t); int32_t lj_ffi_int_producer_count(void); "
      "int32_t lj_ffi_int_first_errno(int32_t); int32_t "
      "lj_ffi_int_last_errno(int32_t); int32_t "
      "lj_ffi_int_producer_first_errno(void); int32_t "
      "lj_ffi_int_producer_last_errno(void); double lj_ffi_int_i8(int8_t); "
      "double lj_ffi_int_u8(uint8_t); double lj_ffi_int_i16(int16_t); double "
      "lj_ffi_int_u16(uint16_t); double lj_ffi_int_i32(int32_t); double "
      "lj_ffi_int_u32(uint32_t); double lj_ffi_int_i64(int64_t); double "
      "lj_ffi_int_u64(uint64_t); double "
      "lj_ffi_int_stack_i8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int8_t); double "
      "lj_ffi_int_stack_u8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint8_t); double "
      "lj_ffi_int_stack_i16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int16_t); double "
      "lj_ffi_int_stack_u16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint16_t); double "
      "lj_ffi_int_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t); double "
      "lj_ffi_int_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t); double "
      "lj_ffi_int_stack_i64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int64_t); double "
      "lj_ffi_int_stack_u64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint64_t); int32_t "
      "lj_ffi_int_producer(int32_t);]]\n"
      "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_INT_ARGS_SO')))\n"
      "local names={'i8','u8','i16','u16','i32','u32','i64','u64'}\n"
      "for i,n in ipairs(names) do _G['__int_arg_'..n]=lib['lj_ffi_int_'..n]; "
      "_G['__int_arg_stack_'..n]=lib['lj_ffi_int_stack_'..n] end\n"
      "_G.__int_arg_producer=lib.lj_ffi_int_producer; "
      "_G.__int_arg_table={value=-1}\n"
      "function __int_arg_configure(k,s) lib.lj_ffi_int_configure(k,s and 1 or "
      "0) end\n"
      "function __int_arg_expect_enable(v) lib.lj_ffi_int_expect_enable(v and "
      "1 or 0) end\n"
      "function __int_arg_save_expected() lib.lj_ffi_int_save_expected() end\n"
      "function __int_arg_mismatch_count() return "
      "lib.lj_ffi_int_mismatch_count() end\n"
      "function __int_arg_count(k) return lib.lj_ffi_int_count(k) end\n"
      "function __int_arg_first_errno(k) return lib.lj_ffi_int_first_errno(k) "
      "end\n"
      "function __int_arg_last_errno(k) return lib.lj_ffi_int_last_errno(k) "
      "end\n"
      "function __int_arg_producer_count() return "
      "lib.lj_ffi_int_producer_count() end\n"
      "function __int_arg_producer_first_errno() return "
      "lib.lj_ffi_int_producer_first_errno() end\n"
      "function __int_arg_producer_last_errno() return "
      "lib.lj_ffi_int_producer_last_errno() end\n"
      "local function mk(params,body,name) return assert(loadstring('return "
      "function('..params..') '..body..' end','@'..name))() end\n"
      "function __int_arg_install(id,form,stack)\n"
      " local v=(form==0 and 't.value' or 'v'); if form==2 then v='p('..v..')' "
      "end\n"
      " local arg=stack and "
      "('101,102,103,104,105,106,107,108,31,-505,1985229328,'..v) or v\n"
      " local body='local sum=.5; for i=1,n do sum=sum+c('..arg..') end; "
      "return sum-.5'\n"
      " local params='t,v,c,p,n'; "
      "_G['__int_arg_run_'..id]=mk(params,body,'intarg-'..id)\n"
      " local oracle=mk(params,body,'intarg-oracle-'..id); "
      "jit.off(oracle,true); _G['__int_arg_oracle_'..id]=oracle\n"
      "end\n"
      "function __int_arg_driver(run,t,v,c,p,n) __int_arg_set_errno(); local "
      "r=run(t,v,c,p,n); return r end\n"
      "local th=require('threading'); __int_arg_ready=th.channel(1); "
      "__int_arg_release=th.channel(1)\n"
      "__int_arg_peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
      "local _,ok=s:recv(60); assert(ok); return true "
      "end,__int_arg_ready,__int_arg_release)); "
      "assert(select(2,__int_arg_ready:recv(5)))\n"
      "jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')\n");
  assert(mt_active_acq(g) != 0);
  for (k = 0; k < KINDS; k++)
    for (f = 0; f < FORMS; f++)
      for (s = 0; s < 2; s++) {
        static const lua_Integer vals[KINDS][6] = {
            {-1, -129, 127, 128, -128, 255},
            {-1, -129, 255, 256, 0, 127},
            {-1, -129, 32767, 32768, -32768, 65535},
            {-1, -129, 65535, 65536, 0, 32767},
            {-1, -129, INT32_MAX, INT32_MIN, 0, 1},
            {-1, 255, 65535, INT32_MIN, INT32_MAX, 0},
            {-1, -129, INT32_MAX, INT32_MIN, 0, 1},
            {-1, 255, 65535, INT32_MIN, INT32_MAX, 0}};
        unsigned vi;
        snprintf(id, sizeof(id), "k%u_f%u_s%u", k, f, s);
        {
          lua_getglobal(L, "__int_arg_install");
          lua_pushstring(L, id);
          lua_pushinteger(L, f);
          lua_pushboolean(L, s);
          assert(lua_pcall(L, 3, 0, 0) == LUA_OK);
        }
        {
          char nm[96];
          GCproto *pt;
          snprintf(nm, sizeof(nm), "__int_arg_run_%s", id);
          pt = global_proto(L, nm);
          for (si = 0; si < nseen; si++)
            assert(seen[si] != pt);
          for (si = 0; si < noracle; si++)
            assert(seen_oracle[si] != pt);
          assert(nseen < sizeof(seen) / sizeof(seen[0]));
          seen[nseen++] = pt;
          snprintf(nm, sizeof(nm), "__int_arg_oracle_%s", id);
          pt = global_proto(L, nm);
          for (si = 0; si < nseen; si++)
            assert(seen[si] != pt);
          for (si = 0; si < noracle; si++)
            assert(seen_oracle[si] != pt);
          assert(noracle < sizeof(seen_oracle) / sizeof(seen_oracle[0]));
          seen_oracle[noracle++] = pt;
        }
        for (vi = 0; vi < 6; vi++)
          invoke_case(L, J, tg, k, (int)f, (int)s, id, vals[k][vi],
                      vi == 0 ? 1 : 0);
        /* In every CALLM case, independently force the producer POSTCALL exit.
         */
        if (f == 2)
          invoke_case(L, J, tg, k, (int)f, (int)s, id, -1, 2);
      }
  assert(nseen == KINDS * FORMS * 2 && noracle == KINDS * FORMS * 2);
  run_lua(L, "assert(__int_arg_release:send(true,5)); local "
             "ok,v=__int_arg_peer:join(5); assert(ok and v)");
  assert(mt_active_acq(g) != 0);
  lua_close(L);
  assert(required_runs == 432 && forced_runs == 64);
  printf("t-arm64-jit-ffi-int-args OK: 48 native shapes, %u runs, "
         "%u forced exits, %u cold retries\n",
         required_runs, forced_runs, cold_retries);
  return 0;
}
#else
int main(void)
{
  puts("t-arm64-jit-ffi-int-args SKIP");
  return 0;
}
#endif
