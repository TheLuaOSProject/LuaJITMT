/* Native lifecycle coverage for nested shared-table CALLXS chains. */

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
#error "t-arm64-jit-table-chains requires ARM64 CALLXS admission"
#endif

#define FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *observed_trace;
static void *call_first_func;
static void *call_second_func;
static void *pair_func;
static void *force_func;
static GCcdata *expected_cdata[2];
static unsigned expected_cdata_count;
static uint32_t native_finishes;
static uint32_t first_finishes;
static uint32_t second_finishes;
static uint32_t pair_finishes;
static uint32_t forced_finishes;
static uint64_t forced_epoch = UINT64_MAX;

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

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

static int proto_has_op(GCproto *pt, BCOp op)
{
  const BCIns *bc = proto_bc(pt);
  MSize i;
  for (i = 0; i < pt->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == op)
      return 1;
  return 0;
}

static GCtrace *find_loop_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && T == observed_trace && trace_runnable_acq(T, no) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) != 0 &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}

static void assert_arg_roots(const LJFFINativeFrame *frame)
{
  uint64_t ro = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t to = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(expected_L->stack);
  TValue *maxstack = tvref(expected_L->maxstack);
  TValue *root, *top, *tv;
  unsigned i;
  assert(ro <= to && ro % sizeof(TValue) == 0 && to % sizeof(TValue) == 0);
  assert((uintptr_t)maxstack >= (uintptr_t)stack &&
         to + sizeof(TValue) <=
           (uint64_t)((uintptr_t)maxstack - (uintptr_t)stack));
  root = (TValue *)((char *)stack + ro);
  top = (TValue *)((char *)stack + to);
  for (i = 0; i < expected_cdata_count; i++) {
    int found = 0;
    for (tv = root; tv <= top; tv++)
      if (tviscdata(tv) && cdataV(tv) == expected_cdata[i]) {
        found = 1;
        break;
      }
    assert(found);
  }
}

static void observe_finish(TGState *tg)
{
  const LJFFINativeFrame *frame;
  void *func;
  assert(tg == expected_tg && expected_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) & FRAME_STATE) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  if (observed_trace == NULL)
    observed_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(observed_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == observed_trace);
  assert(trace_native_pins_acq(observed_trace) != 0);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  func = lj_ffi_native_frame_func_acq(frame);
  if (func == call_first_func) first_finishes++;
  else if (func == call_second_func) second_finishes++;
  else {
    assert(func == pair_func);
    pair_finishes++;
  }
  if (func == pair_func) assert_arg_roots(frame);
  native_finishes++;
}

static void force_postcall(TGState *tg)
{
  void *func = lj_ffi_native_frame_func_acq(&tg->ffi_native_frame[0]);
  uint64_t epoch;
  observe_finish(tg);
  if (func != force_func || forced_finishes != 0) return;
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  assert(epoch != UINT64_MAX);
  forced_epoch = epoch;
  forced_finishes++;
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
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
    fprintf(stderr, "nested table CALLXS fixture: %s\n", lua_tostring(L, -1));
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

static void assert_frame_clean(TGState *tg, GCtrace *T)
{
  uint32_t i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0 && lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
         tg->ffi_xsave_nslots == 0);
  assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

static GCtrace *run_case(lua_State *L, jit_State *J, TGState *tg,
                         const char *runner, const char *counter_name,
                         lua_Integer n, double expected,
                         unsigned cdata_count, int force)
{
  GCtrace *T;
  GCproto *pt;
  int status, saved_errno;
  run_lua(L, "__chains_configure()");
  expected_cdata_count = cdata_count;
  observed_trace = NULL;
  native_finishes = first_finishes = second_finishes = pair_finishes = 0;
  forced_finishes = 0;
  forced_epoch = UINT64_MAX;
  force_func = force ? pair_func : NULL;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force ? force_postcall : observe_finish);
  errno = 0;
  lua_getglobal(L, "__chains_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, "__chains_registry");
  lua_pushinteger(L, n);
  status = lua_pcall(L, 3, 1, 0);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  assert(saved_errno == EDOM && counter(L, counter_name) == n);
  if (native_finishes == 0)
    fprintf(stderr, "no native nested call: runner=%s\n", runner);
  assert(native_finishes != 0);
  pt = global_proto(L, runner);
  T = find_loop_trace(J, pt);
  if (T == NULL || observed_trace == NULL)
    fprintf(stderr, "no nested CALLXS trace: runner=%s native=%u\n",
            runner, native_finishes);
  assert(T != NULL && T == observed_trace);
  if (force) {
    assert(forced_finishes == 1u && forced_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() >= 1u);
    lj_tg_hs_epoch_ack_rel(tg, forced_epoch);
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
      assert(exited == T && exitno < trace_nsnap_acq(exited));
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
      assert(bc_op(pc[-1]) == BC_CALL);
    }
  }
  assert_frame_clean(tg, T);
  return T;
}

static void capture_pair_roots(lua_State *L)
{
  lua_getglobal(L, "__chains_registry");
  lua_getfield(L, -1, "root");
  lua_getfield(L, -1, "branch");
  lua_getfield(L, -1, "leaf");
  lua_getfield(L, -1, "args");
  lua_rawgeti(L, -1, 1);
  assert(tviscdata(L->top - 1));
  expected_cdata[0] = cdataV(L->top - 1);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  assert(tviscdata(L->top - 1));
  expected_cdata[1] = cdataV(L->top - 1);
  lua_pop(L, 6);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;

  expected_L = L;
  expected_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__chains_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_callxs_table_configure(void); int32_t lj_callxs_table_first_count(void); int32_t lj_callxs_table_second_count(void); int32_t lj_callxs_table_first(int32_t); int32_t lj_callxs_table_second(int32_t);]]\n"
    "ffi.cdef[[void lj_callxs_table_i64_configure(int32_t); int32_t lj_callxs_table_i64_pair_count(void); int32_t lj_callxs_table_i64_total_count(void); int32_t lj_callxs_table_i64_pair(int64_t,int64_t);]]\n"
    "local l1=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_TABLE_SO'))); local l2=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_I64_ARGS_SO')))\n"
    "local ctype=ffi.typeof('int32_t(*)(int32_t)'); local ptype=ffi.typeof('int32_t(*)(int64_t,int64_t)')\n"
    "local first=ffi.cast(ctype,l1.lj_callxs_table_first); local second=ffi.cast(ctype,l1.lj_callxs_table_second); local pair=ffi.cast(ptype,l2.lj_callxs_table_i64_pair)\n"
    "local function args() return {ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',-1)} end\n"
    "local oldleaf={call=first,args=args()}; local newleaf={call=second,args=args()}\n"
    "local registry={s={inner=oldleaf}, v={}, list={}, root={branch={leaf=oldleaf}}, pair=pair}\n"
    "registry.v[1]={inner=oldleaf}; registry.list[1]={inner=oldleaf}\n"
    "_G.__chains_registry=registry; _G.__chains_first=first; _G.__chains_second=second; _G.__chains_pair=pair\n"
    "_G.__chains_first_count=l1.lj_callxs_table_first_count; _G.__chains_second_count=l1.lj_callxs_table_second_count; _G.__chains_pair_count=l2.lj_callxs_table_i64_pair_count; _G.__chains_total_count=l2.lj_callxs_table_i64_total_count\n"
    "l1.lj_callxs_table_configure(); l2.lj_callxs_table_i64_configure(0)\n"
    "_G.__chains_ready=threading.channel(1); _G.__chains_go=threading.channel(1); _G.__chains_done=threading.channel(1); _G.__chains_release=threading.channel(1)\n"
    "_G.__chains_peer=threading.spawn(function(reg,leaf,ready,go,done,release) assert(ready:send(true,5)); while true do local cmd,ok=go:recv(30); assert(ok); if cmd=='replace' then reg.s.inner=leaf; reg.v[1].inner=leaf; reg.list[1].inner=leaf; reg.root.branch.leaf=leaf; collectgarbage('collect') elseif cmd=='stop' then assert(done:send(true,5)); break end; assert(done:send(true,5)) end; local _,ok=release:recv(30); assert(ok); return true end,registry,newleaf,__chains_ready,__chains_go,__chains_done,__chains_release)\n"
    "local _,ok=__chains_ready:recv(5); assert(ok)\n"
    "function _G.__chains_configure() l1.lj_callxs_table_configure(); l2.lj_callxs_table_i64_configure(0) end\n"
    "function _G.__chains_s(reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do sum=sum+s.inner.call(i) end; return sum-0.5 end\n"
    "function _G.__chains_v(reg,key,n) local v=reg.v; local sum=0.5; for i=1,n do sum=sum+v[key].inner.call(i) end; return sum-0.5 end\n"
    "function _G.__chains_b(reg,key,n) local list=reg.list; local sum=0.5; for i=1,n do sum=sum+list[1].inner.call(i) end; return sum-0.5 end\n"
    "function _G.__chains_arg(reg,key,n) local root=reg.root; local pair=reg.pair; local sum=0.5; for i=1,n do local leaf=root.branch.leaf; sum=sum+leaf.call(i); local a=leaf.args[1]; local b=leaf.args[2]; sum=sum+pair(a,b) end; return sum-0.5 end\n"
    "local set_errno=__chains_set_errno; function _G.__chains_driver(run,reg,n) set_errno(); local r=run(reg,1,n); return r end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')\n");

  call_first_func = capture_func(L, "__chains_first");
  call_second_func = capture_func(L, "__chains_second");
  pair_func = capture_func(L, "__chains_pair");
  capture_pair_roots(L);
  assert(mt_active_acq(g) != 0);

  T = run_case(L, J, tg, "__chains_s", "__chains_first_count", 32,
               32.0 * 33.0 / 2.0 + 17.0 * 32.0, 0, 0);
  assert(first_finishes != 0 && counter(L, "__chains_second_count") == 0);
  assert(proto_has_op(global_proto(L, "__chains_s"), BC_TGETS));
  assert(op_count(T, IR_CALLXS) != 0);

  {
    GCtrace *same_trace = T;
    run_lua(L, "assert(__chains_go:send('replace',5)); local _,ok=__chains_done:recv(5); assert(ok); collectgarbage('collect')");
    T = run_case(L, J, tg, "__chains_s", "__chains_second_count", 32,
                 3.0 * 32.0 * 33.0 / 2.0 + 101.0 * 32.0, 0, 0);
    assert(T == same_trace);
  }
  assert(second_finishes != 0);

  T = run_case(L, J, tg, "__chains_v", "__chains_second_count", 24,
               3.0 * 24.0 * 25.0 / 2.0 + 101.0 * 24.0, 0, 0);
  assert(proto_has_op(global_proto(L, "__chains_v"), BC_TGETV));
  assert(second_finishes != 0 && op_count(T, IR_CALLXS) != 0);
  T = run_case(L, J, tg, "__chains_b", "__chains_second_count", 24,
               3.0 * 24.0 * 25.0 / 2.0 + 101.0 * 24.0, 0, 0);
  assert(proto_has_op(global_proto(L, "__chains_b"), BC_TGETB));
  assert(second_finishes != 0);

  /* Keep the nested leaf across one finished CALLXS, then use its later
  ** nested cdata values as roots for a second call. */
  capture_pair_roots(L);
  T = run_case(L, J, tg, "__chains_arg", "__chains_pair_count", 24,
               (3.0 * 24.0 * 25.0 / 2.0 + 101.0 * 24.0) + 24.0,
               2, 1);
  assert(second_finishes != 0 && pair_finishes != 0);
  assert(counter(L, "__chains_first_count") == 0);
  assert(counter(L, "__chains_second_count") == 24);
  assert(counter(L, "__chains_total_count") == 24);

  run_lua(L, "assert(__chains_go:send('stop',5)); local _,ok=__chains_done:recv(5); assert(ok); assert(__chains_release:send(true,5)); local joined,res=__chains_peer:join(5); assert(joined and res)");
  assert_frame_clean(tg, T);
  expected_L = NULL;
  expected_tg = NULL;
  observed_trace = NULL;
  lua_close(L);
  puts("t-arm64-jit-table-chains OK: nested table CALLXS and CData chains");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-chains SKIP: requires ARM64 experimental CALLXS");
  return 0;
}

#endif
