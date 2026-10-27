/* Native lifecycle coverage for table-loaded owned I64/U64 CALLXS inputs. */

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
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-table-i64-args requires ARM64 CALLXS admission"
#endif

#define FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static lua_State *expected_L;
static TGState *expected_tg;
static GCtrace *observed_trace;
static void *expected_func;
static GCcdata *expected_args[9];
static unsigned expected_arg_count;
static uint32_t native_finishes;
static uint32_t force_postcall;
static uint32_t forced_finishes;
static uint64_t forced_epoch = UINT64_MAX;

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

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

static int proto_has_op(GCproto *pt, BCOp op)
{
  const BCIns *bc = proto_bc(pt);
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == op) return 1;
  }
  return 0;
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

static void capture_table_roots(lua_State *L,
                                const char *const *fields,
                                const unsigned *indices, unsigned count)
{
  unsigned i;
  lua_getglobal(L, "__table_i64_registry");
  assert(lua_istable(L, -1));
  for (i = 0; i < count; i++) {
    char key[8];
    int n = snprintf(key, sizeof(key), "a%u", indices[i]);
    assert(n > 0 && (size_t)n < sizeof(key));
    lua_getfield(L, -1, fields[i]);
    assert(lua_istable(L, -1));
    lua_getfield(L, -1, key);
    assert(tviscdata(L->top - 1));
    expected_args[i] = cdataV(L->top - 1);
    lua_pop(L, 2);
  }
  lua_pop(L, 1);
  expected_arg_count = count;
}

static void assert_argument_roots(const LJFFINativeFrame *frame)
{
  uint64_t root_offset = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t top_offset = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(expected_L->stack);
  TValue *maxstack = tvref(expected_L->maxstack);
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
  for (i = 0; i < expected_arg_count; i++) {
    int found = 0;
    for (tv = root; tv <= top; tv++) {
      if (tviscdata(tv) && cdataV(tv) == expected_args[i]) {
        found = 1;
        break;
      }
    }
    if (!found)
      fprintf(stderr, "table I64 argument root %u missing\n", i);
    assert(found);
  }
}

static void observe_native(TGState *tg)
{
  const LJFFINativeFrame *frame;
  assert(tg == expected_tg && expected_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) & FRAME_STATE) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func);
  if (observed_trace == NULL)
    observed_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(observed_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == observed_trace);
  assert(trace_native_pins_acq(observed_trace) != 0);
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  assert_argument_roots(frame);
  native_finishes++;
}

static void observe_finish(TGState *tg)
{ observe_native(tg); }

static void force_finish(TGState *tg)
{
  uint64_t epoch;
  observe_native(tg);
  if (!force_postcall || forced_finishes != 0) return;
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
    fprintf(stderr, "table I64 CALLXS Lua error: %s\n",
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
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

static void configure(lua_State *L, unsigned mode)
{
  lua_getglobal(L, "__table_i64_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
}

static void replace_values(lua_State *L)
{
  run_lua(L, "assert(__table_i64_go:send('replace',5)); "
             "local _,ok=__table_i64_done:recv(5); assert(ok)");
}

static void assert_forced_postcall_snapshot(jit_State *J, GCtrace *expected,
                                            GCproto *pt)
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
  assert(bc_op(pc[-1]) == BC_CALL);
}

static GCtrace *run_case(lua_State *L, jit_State *J, TGState *tg,
                         const char *runner, const char *callable,
                         const char *const *root_fields,
                         const unsigned *root_indices,
                         unsigned root_count, const char *counter_name,
                         unsigned mode, lua_Integer n, BCOp table_op,
                         int flush, int force, GCtrace *expected_existing)
{
  GCtrace *T;
  int status, saved_errno;
  configure(L, mode);
  capture_table_roots(L, root_fields, root_indices, root_count);
  expected_func = capture_func(L, callable);
  observed_trace = NULL;
  native_finishes = 0;
  force_postcall = (uint32_t)force;
  forced_finishes = 0;
  forced_epoch = UINT64_MAX;
  if (flush) run_lua(L, "jit.flush()");
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force ? force_finish : observe_finish);
  errno = 0;
  lua_getglobal(L, "__table_i64_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, callable);
  lua_getglobal(L, "__table_i64_registry");
  lua_pushstring(L, "a0");
  lua_pushinteger(L, n);
  status = lua_pcall(L, 5, 1, 0);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == (double)n);
  lua_pop(L, 1);
  assert(saved_errno == EDOM);
  assert(counter(L, counter_name) == n);
  assert(counter(L, "__table_i64_total_count") == n);
  if (native_finishes == 0)
    fprintf(stderr, "no native finish: runner=%s mode=%u op=%u\n",
            runner, mode, (unsigned)table_op);
  assert(native_finishes != 0);
  if (force) {
    assert(forced_finishes == 1u && forced_epoch != UINT64_MAX);
    assert(lj_trace_test_exit_calls() >= 1u);
    assert_forced_postcall_snapshot(J, observed_trace,
                                    global_proto(L, runner));
    lj_tg_hs_epoch_ack_rel(tg, forced_epoch);
  }
  T = find_loop_trace(J, global_proto(L, runner));
  assert(T != NULL && T == observed_trace);
  if (expected_existing != NULL) assert(T == expected_existing);
  assert(proto_has_op(global_proto(L, runner), table_op));
  assert_clean(tg, T);
  return T;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;
  static const unsigned all9[9] = { 0,1,2,3,4,5,6,7,8 };
  static const unsigned mixed9[9] = { 0,0,1,2,3,4,5,6,7 };
  static const unsigned one[1] = { 0 };
  static const char *const signed_fields[9] = {
    "s","s","s","s","s","s","s","s","s" };
  static const char *const unsigned_fields[9] = {
    "u","u","u","u","u","u","u","u","u" };
  static const char *const alias_fields[9] = {
    "a","a","a","a","a","a","a","a","a" };
  static const char *const const_fields[9] = {
    "c","c","c","c","c","c","c","c","c" };
  static const char *const mixed_fields[9] = {
    "s","u","s","s","s","s","s","s","s" };
  static const char *const fixed_field[1] = { "s" };
  uint8_t stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  MSize callback_slot = ccallback_slot_acq(&tg->cb);
  void *old_ffi_func = lj_tg_ffi_call_func_acq(tg);
  const lua_Integer n = 32;

  expected_L = L;
  expected_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__table_i64_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[\n"
    "typedef int64_t lj_table_i64_alias;\n"
    "void lj_callxs_table_i64_configure(int32_t);\n"
    "int32_t lj_callxs_table_i64_signed_count(void);\n"
    "int32_t lj_callxs_table_i64_unsigned_count(void);\n"
    "int32_t lj_callxs_table_i64_mixed_count(void);\n"
    "int32_t lj_callxs_table_i64_alias_count(void);\n"
    "int32_t lj_callxs_table_i64_const_count(void);\n"
    "int32_t lj_callxs_table_i64_vararg_count(void);\n"
    "int32_t lj_callxs_table_i64_pair_count(void);\n"
    "int32_t lj_callxs_table_i64_total_count(void);\n"
    "int32_t lj_callxs_table_i64_signed(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_table_i64_unsigned(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n"
    "int32_t lj_callxs_table_i64_mixed(int64_t,uint64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
    "int32_t lj_callxs_table_i64_alias(lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias,lj_table_i64_alias);\n"
    "int32_t lj_callxs_table_i64_const(const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t,const int64_t);\n"
    "int32_t lj_callxs_table_i64_vararg(int64_t,...);\n"
    "int32_t lj_callxs_table_i64_pair(int64_t,int64_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_I64_ARGS_SO')))\n"
    "local S0={ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',0x7fffffffffffffffLL),ffi.new('int64_t',-1),ffi.new('int64_t',0x0123456789abcdefLL),ffi.new('int64_t',-81985529216486896LL),ffi.new('int64_t',-9223372036854775807LL),ffi.new('int64_t',0x7fffffff),ffi.new('int64_t',0x80000000),ffi.new('int64_t',0x100000000)}\n"
    "local S1={ffi.new('int64_t',-6148914691236517206LL),ffi.new('int64_t',6148914691236517205LL),ffi.new('int64_t',-2147483648),ffi.new('int64_t',-2147483649LL),ffi.new('int64_t',4294967295ULL),ffi.new('int64_t',4294967297LL),ffi.new('int64_t',-9223372036854775807LL-1),ffi.new('int64_t',0x7fffffffffffffffLL),ffi.new('int64_t',0x0123456789abcdefLL)}\n"
    "local U0={ffi.new('uint64_t',0ULL),ffi.new('uint64_t',0xffffffffffffffffULL),ffi.new('uint64_t',0x8000000000000000ULL),ffi.new('uint64_t',0x0123456789abcdefULL),ffi.new('uint64_t',0xfedcba9876543210ULL),ffi.new('uint64_t',0x100000001ULL),ffi.new('uint64_t',0x7fffffffULL),ffi.new('uint64_t',0x80000000ULL),ffi.new('uint64_t',0xffffffffULL)}\n"
    "local U1={ffi.new('uint64_t',0xaaaaaaaaaaaaaaaaULL),ffi.new('uint64_t',0x5555555555555555ULL),ffi.new('uint64_t',0x100000000ULL),ffi.new('uint64_t',0xffffffffULL),ffi.new('uint64_t',0x8000000000000001ULL),ffi.new('uint64_t',0x7fffffffffffffffULL),ffi.new('uint64_t',0xffffffffffffffffULL),ffi.new('uint64_t',1ULL),ffi.new('uint64_t',0xfedcba9876543210ULL)}\n"
    "local function named(src) local t={}; for i=1,9 do t['a'..(i-1)]=src[i] end; return t end\n"
    "local function alias_values(src) local t={}; for i=1,9 do t['a'..(i-1)]=ffi.cast('lj_table_i64_alias',src[i]) end; return t end\n"
    "local function const_values(src) local t={}; for i=1,9 do t['a'..(i-1)]=ffi.cast('const int64_t',src[i]) end; return t end\n"
    "local A0,A1=alias_values(S0),alias_values(S1); local C0,C1=const_values(S0),const_values(S1)\n"
    "_G.__table_i64_registry={s=named(S0),u=named(U0),a=A0,c=C0}\n"
    "_G.__table_i64_ready=threading.channel(1); _G.__table_i64_go=threading.channel(1); _G.__table_i64_done=threading.channel(1); _G.__table_i64_release=threading.channel(1)\n"
    "_G.__table_i64_peer=threading.spawn(function(reg,s,u,a,c,ready,go,done,release)\n"
    " assert(ready:send(true,5)); while true do local cmd,valid=go:recv(30); assert(valid)\n"
    " if cmd=='replace' then for i=0,8 do reg.s['a'..i]=s[i+1]; reg.u['a'..i]=u[i+1]; reg.a['a'..i]=a['a'..i]; reg.c['a'..i]=c['a'..i] end; collectgarbage('collect')\n"
    " elseif cmd=='stop' then assert(done:send(true,5)); break end; assert(done:send(true,5)) end\n"
    " local _,ok=release:recv(30); assert(ok); return true end,__table_i64_registry,S1,U1,A1,C1,__table_i64_ready,__table_i64_go,__table_i64_done,__table_i64_release)\n"
    "local _,ready_ok=__table_i64_ready:recv(5); assert(ready_ok)\n"
    "_G.__table_i64_signed=lib.lj_callxs_table_i64_signed; _G.__table_i64_unsigned=lib.lj_callxs_table_i64_unsigned; _G.__table_i64_pair=lib.lj_callxs_table_i64_pair\n"
    "_G.__table_i64_mixed=lib.lj_callxs_table_i64_mixed; _G.__table_i64_alias=lib.lj_callxs_table_i64_alias; _G.__table_i64_const=lib.lj_callxs_table_i64_const; _G.__table_i64_vararg=lib.lj_callxs_table_i64_vararg\n"
    "_G.__table_i64_signed_count=lib.lj_callxs_table_i64_signed_count; _G.__table_i64_unsigned_count=lib.lj_callxs_table_i64_unsigned_count; _G.__table_i64_mixed_count=lib.lj_callxs_table_i64_mixed_count; _G.__table_i64_alias_count=lib.lj_callxs_table_i64_alias_count; _G.__table_i64_const_count=lib.lj_callxs_table_i64_const_count; _G.__table_i64_vararg_count=lib.lj_callxs_table_i64_vararg_count; _G.__table_i64_pair_count=lib.lj_callxs_table_i64_pair_count; _G.__table_i64_total_count=lib.lj_callxs_table_i64_total_count\n"
    "_G.__table_i64_configure=function(m) lib.lj_callxs_table_i64_configure(m) end\n"
    "function _G.__table_i64_run_s(fn,reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do sum=sum+fn(s.a0,s.a1,s.a2,s.a3,s.a4,s.a5,s.a6,s.a7,s.a8) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_v(fn,reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do local a=s[key]; sum=sum+fn(a,s.a1,s.a2,s.a3,s.a4,s.a5,s.a6,s.a7,s.a8) end; return sum-0.5 end\n"
    "function _G.__table_u64_run_s(fn,reg,key,n) local u=reg.u; local sum=0.5; for i=1,n do sum=sum+fn(u.a0,u.a1,u.a2,u.a3,u.a4,u.a5,u.a6,u.a7,u.a8) end; return sum-0.5 end\n"
    "function _G.__table_u64_run_v(fn,reg,key,n) local u=reg.u; local sum=0.5; for i=1,n do local a=u[key]; sum=sum+fn(a,u.a1,u.a2,u.a3,u.a4,u.a5,u.a6,u.a7,u.a8) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_mixed(fn,reg,key,n) local s,u=reg.s,reg.u; local sum=0.5; for i=1,n do local a=s[key]; local b=u.a0; sum=sum+fn(a,b,s.a1,s.a2,s.a3,s.a4,s.a5,s.a6,s.a7) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_alias(fn,reg,key,n) local a=reg.a; local sum=0.5; for i=1,n do sum=sum+fn(a.a0,a.a1,a.a2,a.a3,a.a4,a.a5,a.a6,a.a7,a.a8) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_const(fn,reg,key,n) local c=reg.c; local sum=0.5; for i=1,n do sum=sum+fn(c.a0,c.a1,c.a2,c.a3,c.a4,c.a5,c.a6,c.a7,c.a8) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_vararg(fn,reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do local fixed=s.a0; sum=sum+fn(fixed,i+0.5,true,'table-i64-vararg') end; return sum-0.5 end\n"
    "local set_errno=__table_i64_set_errno\n"
    "function _G.__table_i64_driver(run,fn,reg,key,n) set_errno(); local r=run(fn,reg,key,n); return r end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  assert(mt_active_acq(g) != 0);
  run_lua(L, "__table_i64_configure(0)");
  T = run_case(L, J, tg, "__table_i64_run_s", "__table_i64_signed",
      signed_fields, all9, 9, "__table_i64_signed_count", 0, n, BC_TGETS, 1, 0, NULL);
  replace_values(L);
  T = run_case(L, J, tg, "__table_i64_run_s", "__table_i64_signed",
      signed_fields, all9, 9, "__table_i64_signed_count", 1, n, BC_TGETS, 0, 0, T);

  run_case(L, J, tg, "__table_i64_run_v", "__table_i64_signed",
      signed_fields, all9, 9, "__table_i64_signed_count", 1, n, BC_TGETV, 1, 0, NULL);
  run_case(L, J, tg, "__table_u64_run_s", "__table_i64_unsigned",
      unsigned_fields, all9, 9, "__table_i64_unsigned_count", 1, n, BC_TGETS, 1, 0, NULL);
  run_case(L, J, tg, "__table_u64_run_v", "__table_i64_unsigned",
      unsigned_fields, all9, 9, "__table_i64_unsigned_count", 1, n, BC_TGETV, 1, 0, NULL);
  run_case(L, J, tg, "__table_i64_run_mixed", "__table_i64_mixed",
      mixed_fields, mixed9, 9, "__table_i64_mixed_count", 1, n, BC_TGETV, 1, 0, NULL);
  run_case(L, J, tg, "__table_i64_run_alias", "__table_i64_alias",
      alias_fields, all9, 9, "__table_i64_alias_count", 1, n, BC_TGETS, 1, 0, NULL);
  run_case(L, J, tg, "__table_i64_run_const", "__table_i64_const",
      const_fields, all9, 9, "__table_i64_const_count", 1, n, BC_TGETS, 1, 0, NULL);
  T = run_case(L, J, tg, "__table_i64_run_vararg", "__table_i64_vararg",
      fixed_field, one, 1, "__table_i64_vararg_count", 1, n, BC_TGETS, 1, 0, NULL);

  /* The completed foreign effect must not be replayed after POSTCALL exit. */
  T = run_case(L, J, tg, "__table_i64_run_s", "__table_i64_signed",
      signed_fields, all9, 9, "__table_i64_signed_count", 1, 24, BC_TGETS, 1, 1, NULL);
  assert(trace_runnable_acq(T, trace_traceno_acq(T)));

  run_lua(L, "assert(__table_i64_go:send('stop',5)); "
             "local _,ok=__table_i64_done:recv(5); assert(ok); "
             "assert(__table_i64_release:send(true,5)); "
             "local ok2,result=__table_i64_peer:join(5); assert(ok2 and result)");
  assert(ccallback_slot_acq(&tg->cb) == callback_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == stopreq);
  assert(lj_tg_ffi_call_func_acq(tg) == old_ffi_func);
  expected_L = NULL;
  expected_tg = NULL;
  lua_close(L);
  puts("t-arm64-jit-table-i64-args OK: rooted table I64/U64 arguments, "
       "TGETS/TGETV, ninth stack arg, aliases, variadic prefix, "
       "POSTCALL no-replay and cleanup");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-i64-args SKIP: requires experimental Darwin ARM64 "
       "CALLXS and trace/XSAVE test hooks");
  return 0;
}

#endif
