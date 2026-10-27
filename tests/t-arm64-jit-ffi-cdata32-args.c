/* Native, rooted int32_t/uint32_t CData CALLXS arguments and POSTCALL exits. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
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

enum { KINDS = 2, FORMS = 2, SOURCE_KINDS = 2, RUNS = 24 };
static const char *const kind_name[KINDS] = {"i32", "u32"};
static TGState *want_tg;
static lua_State *want_L;
static GCtrace *want_trace;
static GCobj *want_object;
static GCobj *want_table_object;
static void *want_func, *force_func;
static uint32_t native_finishes, forced_finishes, oracle_finishes;
static uint32_t successful_native_runs, successful_reuse_runs;
static uint32_t forced_exit_cases, cold_retry_attempts;
static uint64_t old_epoch = UINT64_MAX;
static MSize original_callback_slot;
static uint8_t original_stopreq;
static void *original_ffi_func;

static void run_lua(lua_State *L, const char *src)
{
  int status = luaL_dostring(L, src);
  if (status != LUA_OK) {
    fprintf(stderr, "CData32 CALLXS Lua error: %s\n", lua_tostring(L, -1));
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
    if (ir_load_acq(&ir[r]).o == op) n++;
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

static void assert_argument_root(const LJFFINativeFrame *f)
{
  uint64_t start = lj_ffi_native_frame_root_offset_acq(f);
  uint64_t end = lj_ffi_native_frame_top_offset_acq(f);
  TValue *stack = tvref(want_L->stack), *tv, *last;
  unsigned found = 0;
  assert(start <= end && start % sizeof(TValue) == 0 &&
         end % sizeof(TValue) == 0);
  assert(end <=
         (uintptr_t)tvref(want_L->maxstack) - (uintptr_t)stack);
  tv = (TValue *)((char *)stack + start);
  last = (TValue *)((char *)stack + end);
  /* This proves the exact CData object is in the published frame root span.
  ** The frame API does not expose a narrower per-argument slot range. */
  for (; tv < last; tv++) {
    if (tviscdata(tv) && gcV(tv) == want_object) found |= 1u;
    if (want_table_object && tvistab(tv) && gcV(tv) == want_table_object)
      found |= 2u;
  }
  assert(found == (want_table_object ? 3u : 1u));
}

static const BCIns *runner_call(GCproto *pt)
{
  const BCIns *bc = proto_bc(pt), *found = NULL;
  MSize i;
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_CALL) {
      assert(found == NULL);
      found = &bc[i];
    }
  }
  assert(found != NULL);
  return found;
}

static void assert_exit_after_call(jit_State *J, GCtrace *T, GCproto *pt)
{
  TraceNo parent = lj_trace_test_first_exit_parent();
  ExitNo ex = lj_trace_test_first_exitno();
  GCtrace *out = traceref_safe(J, parent);
  const SnapShot *s;
  MSize ofs, ne;
  const BCIns *pc;
  assert(out == T && ex < trace_nsnap_acq(T));
  s = &trace_snap_acq(T)[ex];
  ofs = snap_mapofs_acq(s);
  ne = snap_nent_acq(s);
  assert(ofs <= trace_nsnapmap_acq(T) &&
         ne + 1u + LJ_FR2 <= trace_nsnapmap_acq(T) - ofs);
  pc = snap_pc_acq(&trace_snapmap_acq(T)[ofs + ne]);
  assert(pc == runner_call(pt) + 1);
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
  assert(lj_ffi_native_frame_flags_acq(f) &
         LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED);
  assert(lj_ffi_native_frame_L_acq(f) == want_L);
  assert(lj_ffi_native_frame_result_root_acq(f) == NULL);
  if (want_trace == NULL) want_trace = lj_ffi_native_frame_trace_acq(f);
  assert(want_trace && lj_ffi_native_frame_trace_acq(f) == want_trace);
  assert(trace_native_pins_acq(want_trace) != 0);
  assert(lj_ffi_native_frame_func_acq(f) == want_func);
  assert_argument_root(f);
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

static GCobj *get_source_object(lua_State *L, const char *name, int table)
{
  GCobj *obj;
  lua_getglobal(L, name);
  assert(table ? tvistab(L->top - 1) : tviscdata(L->top - 1));
  obj = gcV(L->top - 1);
  lua_pop(L, 1);
  return obj;
}

static void set_case_value(lua_State *L, int kind, lua_Integer value)
{
  lua_getglobal(L, "__cdata32_set_value");
  lua_pushinteger(L, kind);
  lua_pushinteger(L, value);
  assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
  lua_getglobal(L, "__cdata32_table");
  lua_getfield(L, -1, "value");
  assert(tviscdata(L->top - 1));
  lua_pop(L, 2);
}

static void call_configure(lua_State *L, int kind, int stack)
{
  lua_getglobal(L, "__cdata32_configure");
  lua_pushinteger(L, kind);
  lua_pushboolean(L, stack);
  assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
}

static lua_Integer get_int(lua_State *L, const char *name, int arg)
{
  lua_Integer v;
  lua_getglobal(L, name);
  if (arg >= 0) lua_pushinteger(L, arg);
  assert(lua_pcall(L, arg >= 0 ? 1 : 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  v = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}

static void assert_helper(lua_State *L, int kind)
{
  assert(get_int(L, "__cdata32_count", kind) == RUNS);
  assert(get_int(L, "__cdata32_mismatch_count", -1) == 0);
  assert(get_int(L, "__cdata32_first_errno", kind) == E2BIG);
  assert(get_int(L, "__cdata32_last_errno", kind) == EDOM);
}

static int driver(lua_State *L, const char *driver_name, const char *runner,
                  int table_source, const char *consumer)
{
  lua_getglobal(L, driver_name);
  lua_getglobal(L, runner);
  lua_getglobal(L, table_source ? "__cdata32_table" : "__cdata32_table_value");
  lua_getglobal(L, consumer);
  lua_pushinteger(L, RUNS);
  return lua_pcall(L, 4, 1, 0);
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
  if (T) assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    LJFFINativeFrame *f = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(f) == NULL &&
           lj_ffi_native_frame_L_acq(f) == NULL);
    assert(lj_ffi_native_frame_func_acq(f) == NULL &&
           lj_ffi_native_frame_flags_acq(f) == 0);
  }
}

static void assert_return(lua_State *L, int status)
{
  assert(status == LUA_OK && lua_isnumber(L, -1) &&
         lua_tonumber(L, -1) == 30.0);
  lua_pop(L, 1);
}

static void invoke(lua_State *L, jit_State *J, TGState *tg, int kind,
                   int table_source, int stack, const char *id,
                   lua_Integer input, int force)
{
  char candidate[96], oracle[96], consumer[64];
  GCproto *pt;
  GCtrace *T = NULL;
  int status, attempt, limit, saved_errno;
  snprintf(candidate, sizeof(candidate), "__cdata32_run_%s", id);
  snprintf(oracle, sizeof(oracle), "__cdata32_oracle_%s", id);
  snprintf(consumer, sizeof(consumer), "__cdata32_%s%s",
           stack ? "stack_" : "", kind_name[kind]);
  pt = global_proto(L, candidate);
  set_case_value(L, kind, input);
  want_object = get_source_object(L, "__cdata32_table_value", 0);
  want_table_object = table_source
                          ? get_source_object(L, "__cdata32_table", 1)
                          : NULL;
  call_configure(L, kind, stack);
  run_lua(L, "__cdata32_expect_enable(false)");
  set_jit(L, 0);
  oracle_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(reject_oracle);
  errno = 0;
  status = driver(L, "__cdata32_driver", oracle, table_source, consumer);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert_return(L, status);
  assert(oracle_finishes == 0 && saved_errno == EDOM);
  assert(find_loop_trace(J, global_proto(L, oracle)) == NULL);
  assert_helper(L, kind);
  run_lua(L, "__cdata32_save_expected(); __cdata32_expect_enable(true)");
  set_jit(L, 1);

  T = find_loop_trace(J, pt);
  limit = T ? 1 : 8;
  for (attempt = 0; attempt < limit; attempt++) {
    if (find_loop_trace(J, pt) == NULL)
      run_lua(L, "jit.opt.start('hotloop=1','hotexit=1000')");
    call_configure(L, kind, stack);
    set_case_value(L, kind, input);
    want_tg = tg;
    want_L = L;
    want_object = get_source_object(L, "__cdata32_table_value", 0);
    want_table_object = table_source
                            ? get_source_object(L, "__cdata32_table", 1)
                            : NULL;
    want_trace = T;
    force_func = NULL;
    native_finishes = forced_finishes = 0;
    old_epoch = UINT64_MAX;
    capture_func(L, consumer, &want_func);
    lj_trace_test_reset_exit_stats();
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__cdata32_driver", candidate, table_source, consumer);
    saved_errno = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(saved_errno == EDOM);
    assert_helper(L, kind);
    {
      GCtrace *published = find_loop_trace(J, pt);
      assert_clean(tg, published);
      if (T == NULL) T = published;
      else assert(published == T);
    }
    if (T && native_finishes) {
      successful_native_runs++;
      break;
    }
    if (attempt + 1 < limit) cold_retry_attempts++;
  }
  if (T == NULL || native_finishes == 0)
    fprintf(stderr, "no native CData32 argument trace: %s\n", id);
  assert(T && native_finishes > 0 && want_trace == T);
  assert_clean(tg, T);

  if (force) {
    call_configure(L, kind, stack);
    set_case_value(L, kind, input);
    want_object = get_source_object(L, "__cdata32_table_value", 0);
    want_table_object = table_source
                            ? get_source_object(L, "__cdata32_table", 1)
                            : NULL;
    want_trace = T;
    native_finishes = forced_finishes = 0;
    old_epoch = UINT64_MAX;
    force_func = want_func;
    lj_trace_test_reset_exit_stats();
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__cdata32_driver", candidate, table_source, consumer);
    saved_errno = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(saved_errno == EDOM && native_finishes > 0 && forced_finishes == 1);
    assert_helper(L, kind);
    assert(lj_trace_test_exit_calls() > 0);
    assert_exit_after_call(J, T, pt);
    forced_exit_cases++;
    lj_tg_hs_epoch_ack_rel(tg, old_epoch);
    assert_clean(tg, T);
    /* Reuse the exact trace after its POSTCALL continuation was reconstructed. */
    call_configure(L, kind, stack);
    want_trace = T;
    force_func = NULL;
    native_finishes = 0;
    lj_ffi_native_trace_test_set_finish_hook(observe_finish);
    errno = 0;
    status = driver(L, "__cdata32_driver", candidate, table_source, consumer);
    saved_errno = errno;
    lj_ffi_native_trace_test_set_finish_hook(NULL);
    assert_return(L, status);
    assert(saved_errno == EDOM && native_finishes > 0);
    successful_reuse_runs++;
    assert_helper(L, kind);
    assert_clean(tg, T);
  }
}

int main(void)
{
  static const lua_Integer values[KINDS][6] = {
    {INT32_MIN, INT32_MIN + 1, -1, 0, 1, INT32_MAX},
    {0, 1, INT32_MAX, INT32_MAX + (lua_Integer)1,
     UINT32_MAX - 1u, UINT32_MAX}
  };
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  int k, src, stack;
  char id[48];
  original_callback_slot = ccallback_slot_acq(&tg->cb);
  original_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  original_ffi_func = lj_tg_ffi_call_func_acq(tg);
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__cdata32_set_errno");
  run_lua(L, "jit.off()");
  run_lua(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[void lj_ffi_cdata32_configure(int32_t,int32_t); "
    "void lj_ffi_cdata32_expect_enable(int32_t); "
    "void lj_ffi_cdata32_save_expected(int32_t,uint32_t); "
    "int32_t lj_ffi_cdata32_mismatch_count(void); "
    "int32_t lj_ffi_cdata32_count(int32_t); "
    "int32_t lj_ffi_cdata32_first_errno(int32_t); "
    "int32_t lj_ffi_cdata32_last_errno(int32_t); "
    "uint32_t lj_ffi_cdata32_captured(int32_t,int32_t); "
    "double lj_ffi_cdata32_i32(int32_t); double lj_ffi_cdata32_u32(uint32_t); "
    "double lj_ffi_cdata32_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,"
    "int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t); "
    "double lj_ffi_cdata32_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,"
    "int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CDATA32_ARGS_SO')))\n"
    "_G.__cdata32_i32=lib.lj_ffi_cdata32_i32; "
    "_G.__cdata32_u32=lib.lj_ffi_cdata32_u32; "
    "_G.__cdata32_stack_i32=lib.lj_ffi_cdata32_stack_i32; "
    "_G.__cdata32_stack_u32=lib.lj_ffi_cdata32_stack_u32\n"
    "_G.__cdata32_expected={}\n"
    "function __cdata32_configure(k,s) lib.lj_ffi_cdata32_configure(k,s and 1 or 0); "
    "local e=__cdata32_expected[k]; if e~=nil then lib.lj_ffi_cdata32_save_expected(k,e) end end\n"
    "function __cdata32_expect_enable(v) lib.lj_ffi_cdata32_expect_enable(v and 1 or 0) end\n"
    "function __cdata32_save_expected() local k=__cdata32_kind; "
    "local e=lib.lj_ffi_cdata32_captured(k,0); __cdata32_expected[k]=e; "
    "lib.lj_ffi_cdata32_save_expected(k,e) end\n"
    "function __cdata32_mismatch_count() return lib.lj_ffi_cdata32_mismatch_count() end\n"
    "function __cdata32_count(k) return lib.lj_ffi_cdata32_count(k) end\n"
    "function __cdata32_first_errno(k) return lib.lj_ffi_cdata32_first_errno(k) end\n"
    "function __cdata32_last_errno(k) return lib.lj_ffi_cdata32_last_errno(k) end\n"
    "function __cdata32_captured(k,i) return lib.lj_ffi_cdata32_captured(k,i) end\n"
    "_G.__cdata32_table={}; _G.__cdata32_table_value=nil; "
    "function __cdata32_set_value(k,v) __cdata32_kind=k; local ty=k==0 and 'int32_t' or 'uint32_t'; "
    "local x=ffi.new(ty,v); assert(ffi.istype(ty,x)); "
    "__cdata32_table.value=x; __cdata32_table_value=x; "
    "assert(rawequal(__cdata32_table.value,__cdata32_table_value)) end\n"
    "function __cdata32_install(id,kind,table_source,stack)\n"
    " local x=table_source and 'arg.value' or 'arg'; local a=stack and "
    "('101,102,103,104,105,106,107,108,31,-505,1985229328,'..x) or x\n"
    " local fn='c('..a..')'; local body='local sum=.5; for i=1,n do sum=sum+'..fn..' end; return sum-.5'\n"
    " local src='return function(arg,c,n) '..body..' end'; "
    "_G['__cdata32_run_'..id]=assert(loadstring(src,'@cdata32-'..id))()\n"
    " _G['__cdata32_oracle_'..id]=assert(loadstring(src,'@cdata32-oracle-'..id))(); "
    "jit.off(_G['__cdata32_oracle_'..id],true)\n"
    "end\n"
    "function __cdata32_driver(run,arg,c,n) __cdata32_set_errno(); "
    "local r=run(arg,c,n); return r end\n"
    "local th=require('threading'); __cdata32_ready=th.channel(1); __cdata32_release=th.channel(1); "
    "__cdata32_peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
    "local _,ok=s:recv(60000); assert(ok); return true end,__cdata32_ready,__cdata32_release)); "
    "assert(select(2,__cdata32_ready:recv(5))); "
    "jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')\n");
  assert(mt_active_acq(g) != 0);
  for (k = 0; k < KINDS; k++)
    for (src = 0; src < SOURCE_KINDS; src++)
      for (stack = 0; stack < FORMS; stack++) {
        static const char *const labels[] = {"unary", "stack"};
        unsigned vi;
        snprintf(id, sizeof(id), "k%d_src%d_%s", k, src, labels[stack]);
        lua_getglobal(L, "__cdata32_install");
        lua_pushstring(L, id);
        lua_pushinteger(L, k);
        lua_pushboolean(L, src);
        lua_pushboolean(L, stack);
        assert(lua_pcall(L, 4, 0, 0) == LUA_OK);
        for (vi = 0; vi < 6; vi++)
          invoke(L, J, tg, k, src, stack, id, values[k][vi], vi == 1);
      }
  assert(successful_native_runs == KINDS * SOURCE_KINDS * FORMS * 6u);
  assert(forced_exit_cases == KINDS * SOURCE_KINDS * FORMS);
  assert(successful_reuse_runs == KINDS * SOURCE_KINDS * FORMS);
  assert(successful_native_runs + forced_exit_cases + successful_reuse_runs == 64u);
  assert(cold_retry_attempts <= KINDS * SOURCE_KINDS * FORMS * 7u);
  run_lua(L, "assert(__cdata32_release:send(true,5)); local ok,v=__cdata32_peer:join(5); assert(ok and v)");
  assert(mt_active_acq(g) != 0);
  lua_close(L);
  printf("t-arm64-jit-ffi-cdata32-args OK: native-runs=%u normal=%u forced=%u reuse=%u cold-retries=%u\n",
         successful_native_runs + forced_exit_cases + successful_reuse_runs,
         successful_native_runs, forced_exit_cases, successful_reuse_runs,
         cold_retry_attempts);
  return 0;
}
#else
int main(void)
{
  puts("t-arm64-jit-ffi-cdata32-args SKIP");
  return 0;
}
#endif
