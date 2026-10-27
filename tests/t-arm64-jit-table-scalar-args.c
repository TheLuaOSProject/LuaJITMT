/* Native coverage for shared table scalar values passed to CALLXS. */

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

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-table-scalar-args requires ARM64 CALLXS admission"
#endif

#define FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static lua_State *owner_L;
static TGState *owner_tg;
static GCtrace *expected_trace;
static void *expected_func;
static uint32_t native_finishes;
static uint32_t force_postcall;
static uint32_t forced_finishes;
static uint64_t forced_epoch = UINT64_MAX;
static GCproto *abort_recording_pt;
static uint32_t injected_aborts;
static uint32_t recording_retries;

static void *capture_func(lua_State *L, const char *name)
{
  GCcdata *cd;
  void *func;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(&func, cdataptr(cd), sizeof(func));
  lua_pop(L, 1);
  assert(func != NULL);
  return func;
}

static unsigned op_count(const GCtrace *T, IROp wanted)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  unsigned count = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++)
    if (ir_load_acq(&ir[ref]).o == wanted) count++;
  return count;
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

static GCtrace *find_loop_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) != 0 &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}

static void interrupt_cold_recording(jit_State *J, const BCIns *pc)
{
  if (J->pt == abort_recording_pt && bc_op(*pc) == BC_TGETV) {
    assert(J2TG(J) == owner_tg);
    injected_aborts++;
    lj_trace_err(J, LJ_TRERR_RECERR);
  }
}

static void observe_native(TGState *tg)
{
  const LJFFINativeFrame *frame;
  assert(tg == owner_tg && owner_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) & FRAME_STATE) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == owner_L);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func);
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
}

static void observe_finish(TGState *tg)
{ observe_native(tg); }

static void force_finish(TGState *tg)
{
  observe_native(tg);
  if (!force_postcall || forced_finishes != 0) return;
  forced_epoch = lj_tg_hs_epoch_ack_acq(tg);
  assert(forced_epoch != UINT64_MAX);
  forced_finishes++;
  lj_tg_hs_epoch_ack_rel(tg, forced_epoch + 1u);
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
    fprintf(stderr, "table scalar CALLXS Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer counter(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static int call_driver(lua_State *L, const char *runner, const char *fn,
                       lua_Integer key, lua_Integer n)
{
  lua_getglobal(L, "__table_scalar_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__table_scalar_registry");
  lua_getglobal(L, fn);
  lua_pushinteger(L, key);
  lua_pushinteger(L, n);
  return lua_pcall(L, 5, 1, 0);
}

static void assert_clean(TGState *tg, GCtrace *T)
{
  uint32_t i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0 && lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
         tg->ffi_xsave_nslots == 0);
  if (T != NULL) assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

static void assert_first_exit_at(jit_State *J, GCtrace *expected,
                                 GCproto *pt, BCOp op, int after)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo exitno = lj_trace_test_first_exitno();
  GCtrace *exited = traceref_safe(J, parent);
  const SnapShot *snap;
  MSize ofs, nent;
  const BCIns *pc;
#if LJ_FR2
  uint64_t pcbase;
#endif
  assert(exited == expected && exitno < trace_nsnap_acq(exited));
  snap = &trace_snap_acq(exited)[exitno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  assert(ofs <= trace_nsnapmap_acq(exited) &&
         nent + 1u + LJ_FR2 <= trace_nsnapmap_acq(exited) - ofs);
  pc = snap_pc_acq(&trace_snapmap_acq(exited)[ofs + nent]);
#if LJ_FR2
  memcpy(&pcbase, &trace_snapmap_acq(exited)[ofs + nent], sizeof(pcbase));
  assert((pcbase & UINT64_C(0xff)) == 0);
#endif
  assert(pc > proto_bc(pt) && pc < proto_bc(pt) + pt->sizebc);
  assert(bc_op(pc[after ? -1 : 0]) == op);
}

static GCtrace *run_case(lua_State *L, jit_State *J, TGState *tg,
                         const char *runner, const char *fn_name,
                         int32_t mode, lua_Integer n, unsigned calls_per_iter,
                         lua_Number expected, void *func, const char *count_name,
                         int force)
{
  GCproto *pt = global_proto(L, runner);
  GCtrace *T = find_loop_trace(J, pt);
  int cold = T == NULL && !force;
  int status, saved_errno;
  unsigned attempt;
  uint32_t exits_before;
  for (attempt = 0; ; attempt++) {
    int inject = abort_recording_pt == pt && attempt == 0;
    if (cold) run_lua(L, "jit.opt.start('hotloop=1')");
    lua_getglobal(L, "__table_scalar_configure");
    lua_pushinteger(L, mode);
    assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
    lj_trace_test_reset_exit_stats();
    lj_trace_test_reset_exittab_stats();
    exits_before = lj_trace_test_exit_calls();
    expected_trace = T;
    expected_func = func;
    native_finishes = forced_finishes = 0;
    forced_epoch = UINT64_MAX;
    force_postcall = (uint32_t)force;
    lj_ffi_native_trace_test_set_finish_hook(force ? force_finish : observe_finish);
    if (inject) {
      assert(cold && T == NULL && injected_aborts == 0);
      lj_trace_test_set_recorded_hook(interrupt_cold_recording);
    }
    errno = 0;
    status = call_driver(L, runner, fn_name, 2, n);
    saved_errno = errno;
    lj_trace_test_set_recorded_hook(NULL);
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    force_postcall = 0;
    assert(status == LUA_OK && lua_isnumber(L, -1));
    if (lua_tonumber(L, -1) != expected) {
      fprintf(stderr, "%s returned %.17g, expected %.17g\n", runner,
              (double)lua_tonumber(L, -1), (double)expected);
      assert(0);
    }
    lua_pop(L, 1);
    assert(saved_errno == EDOM);
    assert(counter(L, "__table_scalar_total_count") == n * calls_per_iter);
    assert(counter(L, count_name) == n * calls_per_iter);
    T = find_loop_trace(J, pt);
    if (inject) {
      assert(injected_aborts != 0 && native_finishes == 0 && T == NULL);
      assert(lj_trace_test_abort_count() != 0 &&
             lj_trace_test_last_abort_error() == LJ_TRERR_RECERR);
    }
    if (force) {
      assert(forced_finishes == 1 && forced_epoch != UINT64_MAX);
      assert(lj_trace_test_exit_calls() > exits_before);
      assert_first_exit_at(J, expected_trace, pt, BC_CALL, 1);
      lj_tg_hs_epoch_ack_rel(tg, forced_epoch);
      forced_epoch = UINT64_MAX;
    }
    assert_clean(tg, T);
    if (native_finishes != 0) {
      assert(T != NULL && T == expected_trace);
      return T;
    }
    fprintf(stderr, "no native scalar CALLXS finish: runner=%s attempt=%u "
            "aborts=%u last=%u injected=%d\n", runner, attempt + 1u,
            lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(), inject);
    /* Only first-time recording may retry. Every attempt checks effects,
    ** errno and cleanup; published-trace reuse and forced exits get one try. */
    assert(cold && attempt < 7u);
    recording_retries++;
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  void *i32_func, *double_func, *float_func, *bool_func, *i32_9_func;
  void *mixed11_func;
  GCtrace *T, *same_trace;
  uint32_t exits_before;

  owner_L = L;
  owner_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__table_scalar_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_table_scalar_configure(int32_t); int32_t lj_table_scalar_i32_count(void); int32_t lj_table_scalar_double_count(void); int32_t lj_table_scalar_float_count(void); int32_t lj_table_scalar_bool_count(void); int32_t lj_table_scalar_i32_9_count(void); int32_t lj_table_scalar_mixed11_count(void); int32_t lj_table_scalar_total_count(void);]]\n"
    "ffi.cdef[[int32_t lj_table_scalar_i32(int32_t); int32_t lj_table_scalar_bool_i32(int32_t); double lj_table_scalar_double(double); float lj_table_scalar_float(float); int32_t lj_table_scalar_i32_9(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t); double lj_table_scalar_mixed11(int32_t,double,int32_t,float,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_SCALAR_ARGS_SO')))\n"
    "local i32type=ffi.typeof('int32_t(*)(int32_t)'); local dbtype=ffi.typeof('double(*)(double)'); local fltype=ffi.typeof('float(*)(float)')\n"
    "_G.__table_scalar_fn_i32=ffi.cast(i32type,lib.lj_table_scalar_i32); _G.__table_scalar_fn_double=ffi.cast(dbtype,lib.lj_table_scalar_double); _G.__table_scalar_fn_float=ffi.cast(fltype,lib.lj_table_scalar_float); _G.__table_scalar_fn_bool=ffi.cast(i32type,lib.lj_table_scalar_bool_i32); _G.__table_scalar_fn_i32_9=ffi.cast(ffi.typeof('int32_t(*)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t)'),lib.lj_table_scalar_i32_9); _G.__table_scalar_fn_mixed11=ffi.cast(ffi.typeof('double(*)(int32_t,double,int32_t,float,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t)'),lib.lj_table_scalar_mixed11)\n"
    "local identity={}; local registry={s={value=-17},v={[2]=2.5},list={1.5},bool={value=true,false_value=false},tablekey=identity,root={branch={leaf={}}}}; registry.v[identity]=5.5\n"
    "local leaf=registry.root.branch.leaf; for i=0,8 do leaf['a'..i]=i+1 end; leaf.d0=1.25; leaf.f0=2.5\n"
    "_G.__table_scalar_registry=registry; function _G.__table_scalar_configure(mode) lib.lj_table_scalar_configure(mode) end\n"
    "_G.__table_scalar_i32_count=lib.lj_table_scalar_i32_count; _G.__table_scalar_double_count=lib.lj_table_scalar_double_count; _G.__table_scalar_float_count=lib.lj_table_scalar_float_count; _G.__table_scalar_bool_count=lib.lj_table_scalar_bool_count; _G.__table_scalar_i32_9_count=lib.lj_table_scalar_i32_9_count; _G.__table_scalar_mixed11_count=lib.lj_table_scalar_mixed11_count; _G.__table_scalar_total_count=lib.lj_table_scalar_total_count\n"
    "_G.__table_scalar_ready=threading.channel(1); _G.__table_scalar_go=threading.channel(1); _G.__table_scalar_done=threading.channel(1); _G.__table_scalar_release=threading.channel(1)\n"
    "_G.__table_scalar_peer=threading.spawn(function(reg,ready,go,done,release) assert(ready:send(true,5)); while true do local cmd,ok=go:recv(30); assert(ok); if cmd=='replace' then reg.s.value=-19; reg.v[2]=4.25; reg.v[reg.tablekey]=5.5; reg.list[1]=2.75; reg.root.branch.leaf.a0=11; for i=1,8 do reg.root.branch.leaf['a'..i]=i+11 end; reg.root.branch.leaf.d0=2.5; reg.root.branch.leaf.f0=3.5 elseif cmd=='boolean' then reg.s.value=true elseif cmd=='stop' then assert(done:send(true,5)); break end; assert(done:send(true,5)) end; local _,ok=release:recv(30); assert(ok); return true end,registry,__table_scalar_ready,__table_scalar_go,__table_scalar_done,__table_scalar_release)\n"
    "local _,ready_ok=__table_scalar_ready:recv(5); assert(ready_ok)\n"
    "function _G.__table_scalar_run_s_i32(reg,fn,key,n) local t=reg.s; local sum=0.5; for i=1,n do sum=sum+fn(t.value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_read_after_call(reg,fn,key,n) local t=reg.s; local sum=0.5; for i=1,n do sum=sum+fn(t.value); sum=sum+fn(t.value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_v_double(reg,fn,key,n) local t=reg.v; local sum=0.5; for i=1,n do sum=sum+fn(t[key]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_b_float(reg,fn,key,n) local t=reg.list; local sum=0.5; for i=1,n do sum=sum+fn(t[1]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_bool_i32(reg,fn,key,n) local t=reg.bool; local sum=0.5; for i=1,n do sum=sum+fn(t.value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_bool_false_i32(reg,fn,key,n) local t=reg.bool; local sum=0.5; for i=1,n do sum=sum+fn(t.false_value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_v_tablekey_double(reg,fn,key,n) local t=reg.v; local k=reg.tablekey; local sum=0.5; for i=1,n do sum=sum+fn(t[k]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_nested9(reg,fn,key,n) local root=reg.root; local sum=0.5; for i=1,n do local t=root.branch.leaf; sum=sum+fn(t.a0,t.a1,t.a2,t.a3,t.a4,t.a5,t.a6,t.a7,t.a8) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_mixed11(reg,fn,key,n) local root=reg.root; local sum=0.5; for i=1,n do local t=root.branch.leaf; sum=sum+fn(t.a0,t.d0,t.a1,t.f0,t.a2,t.a3,t.a4,t.a5,t.a6,t.a7,t.a8) end; return sum-0.5 end\n"
    "local set_errno=__table_scalar_set_errno; function _G.__table_scalar_driver(run,reg,fn,key,n) set_errno(); local result=run(reg,fn,key,n); return result end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')\n");

  assert(mt_active_acq(g) != 0);
  i32_func = capture_func(L, "__table_scalar_fn_i32");
  double_func = capture_func(L, "__table_scalar_fn_double");
  float_func = capture_func(L, "__table_scalar_fn_float");
  bool_func = capture_func(L, "__table_scalar_fn_bool");
  i32_9_func = capture_func(L, "__table_scalar_fn_i32_9");
  mixed11_func = capture_func(L, "__table_scalar_fn_mixed11");

  T = run_case(L,J,tg,"__table_scalar_run_s_i32","__table_scalar_fn_i32",0,32,1,-544.0,i32_func,"__table_scalar_i32_count",0);
  assert(counter(L,"__table_scalar_i32_count")==32);
  same_trace = T;
  T = run_case(L,J,tg,"__table_scalar_run_read_after_call","__table_scalar_fn_i32",0,16,2,-544.0,i32_func,"__table_scalar_i32_count",0);
  assert(counter(L,"__table_scalar_i32_count")==32);
  run_lua(L,"assert(__table_scalar_go:send('replace',5)); local _,ok=__table_scalar_done:recv(5); assert(ok)");
  T = run_case(L,J,tg,"__table_scalar_run_s_i32","__table_scalar_fn_i32",1,32,1,-608.0,i32_func,"__table_scalar_i32_count",0);
  assert(T == same_trace && counter(L,"__table_scalar_i32_count")==32);

  /* Interrupt the first cold attempt before its C call. This synthetic
  ** RECERR verifies recovery, without attributing any intermittent failure
  ** to a particular runtime event. The next attempt must execute natively. */
  abort_recording_pt = global_proto(L, "__table_scalar_run_v_double");
  T = run_case(L,J,tg,"__table_scalar_run_v_double","__table_scalar_fn_double",8,32,1,136.0,double_func,"__table_scalar_double_count",0);
  assert(injected_aborts != 0 && recording_retries != 0);
  abort_recording_pt = NULL;
  assert(counter(L,"__table_scalar_double_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_v_tablekey_double","__table_scalar_fn_double",10,32,1,176.0,double_func,"__table_scalar_double_count",0);
  assert(counter(L,"__table_scalar_double_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_b_float","__table_scalar_fn_float",9,32,1,88.0,float_func,"__table_scalar_float_count",0);
  assert(counter(L,"__table_scalar_float_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_bool_i32","__table_scalar_fn_bool",2,32,1,32.0,bool_func,"__table_scalar_bool_count",0);
  assert(counter(L,"__table_scalar_bool_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_bool_false_i32","__table_scalar_fn_bool",11,32,1,0.0,bool_func,"__table_scalar_bool_count",0);
  assert(counter(L,"__table_scalar_bool_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_nested9","__table_scalar_fn_i32_9",5,32,1,4320.0,i32_9_func,"__table_scalar_i32_9_count",0);
  assert(counter(L,"__table_scalar_i32_9_count")==32);
  T = run_case(L,J,tg,"__table_scalar_run_mixed11","__table_scalar_fn_mixed11",7,32,1,4512.0,mixed11_func,"__table_scalar_mixed11_count",0);
  assert(counter(L,"__table_scalar_mixed11_count")==32);

  /* A changed table value of a different Lua type must guard-exit before the
  ** FFI call, then complete in the interpreter without duplicating effects. */
  run_lua(L,"assert(__table_scalar_go:send('boolean',5)); local _,ok=__table_scalar_done:recv(5); assert(ok)");
  run_lua(L,"__table_scalar_configure(2)");
  lj_trace_test_reset_exit_stats();
  exits_before = lj_trace_test_exit_calls();
  expected_trace = NULL; expected_func = i32_func; native_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  {
    int status = call_driver(L,"__table_scalar_run_s_i32","__table_scalar_fn_i32",2,2);
    assert(status == LUA_OK && lua_tonumber(L,-1) == 2.0);
    lua_pop(L,1);
  }
  assert(errno == EDOM && counter(L,"__table_scalar_i32_count") == 2);
  assert(counter(L,"__table_scalar_total_count") == 2);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(lj_trace_test_exit_calls() > exits_before);
  assert(native_finishes == 0);
  assert(lj_trace_test_first_exit_parent() == same_trace->traceno);
  assert_first_exit_at(J,same_trace,global_proto(L,"__table_scalar_run_s_i32"),BC_TGETS,0);
  assert_clean(tg,same_trace);

  /* Force one post-call exit after a committed scalar effect; the helper must
  ** still run exactly once per loop iteration. */
  run_lua(L,"assert(__table_scalar_go:send('replace',5)); local _,ok=__table_scalar_done:recv(5); assert(ok)");
  T = run_case(L,J,tg,"__table_scalar_run_s_i32","__table_scalar_fn_i32",1,16,1,-304.0,i32_func,"__table_scalar_i32_count",1);
  assert(counter(L,"__table_scalar_i32_count")==16);
  assert(forced_finishes == 1 && T == same_trace);

  run_lua(L,
    "assert(__table_scalar_go:send('stop',5)); local _,ok=__table_scalar_done:recv(5); assert(ok); assert(__table_scalar_release:send(true,5)); local joined,res=__table_scalar_peer:join(5); assert(joined and res)");
  assert_clean(tg,T);
  lua_close(L);
  printf("t-arm64-jit-table-scalar-args OK: native shared-table scalar arguments; "
         "injected aborts=%u cold retries=%u\n", injected_aborts, recording_retries);
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-scalar-args SKIP: requires ARM64 CALLXS");
  return 0;
}

#endif
