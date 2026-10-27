/* A traced nested table value stays rooted across a C-to-Lua callback. */

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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS) && \
    defined(LJ_TAB_TEST_HELPERS)

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-table-chains-callback requires ARM64 CALLXS admission"
#endif

#define FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

static TGState *owner_tg;
static lua_State *owner_L;
static GCtrace *expected_trace;
static GCobj *target_object;
static GCobj *gate_object;
static GCcdata *pair_args[2];
typedef void (*VoidFunc)(void);

static void *gate_func;
static VoidFunc gate_arm_func;
static void *warm_target_func;
static void *target_func;
static void *pair_func;
static uint32_t target_root_seen;
static uint32_t callback_armed_seen;
static uint32_t callback_checks;
static uint32_t native_finishes;
static uint32_t gate_finishes;
static uint32_t warm_target_finishes;
static uint32_t target_finishes;
static uint32_t pair_finishes;

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

static VoidFunc capture_void_func(lua_State *L, const char *name)
{
  GCcdata *cd;
  VoidFunc func;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(&func, cdataptr(cd), sizeof(func));
  lua_pop(L, 1);
  assert(func != NULL);
  return func;
}

static GCobj *capture_object(lua_State *L, const char *name)
{
  GCobj *obj;
  lua_getglobal(L, name);
  assert(tvisgcv(L->top - 1));
  obj = gcV(L->top - 1);
  lua_pop(L, 1);
  assert(obj != NULL);
  return obj;
}

static void capture_pair_args(lua_State *L)
{
  lua_getglobal(L, "__chains_cb_registry");
  lua_getfield(L, -1, "root");
  lua_getfield(L, -1, "branch");
  lua_getfield(L, -1, "leaf");
  lua_getfield(L, -1, "args");
  lua_rawgeti(L, -1, 1);
  assert(tviscdata(L->top - 1));
  pair_args[0] = cdataV(L->top - 1);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 2);
  assert(tviscdata(L->top - 1));
  pair_args[1] = cdataV(L->top - 1);
  lua_pop(L, 6);
}

static int frame_has_target_root(const LJFFINativeFrame *frame)
{
  uint64_t root_offset = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t top_offset = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(owner_L->stack);
  TValue *maxstack = tvref(owner_L->maxstack);
  TValue *root, *top, *tv;
  assert(root_offset <= top_offset &&
         root_offset % sizeof(TValue) == 0 &&
         top_offset % sizeof(TValue) == 0);
  assert((uintptr_t)maxstack >= (uintptr_t)stack &&
         top_offset + sizeof(TValue) <=
           (uint64_t)((uintptr_t)maxstack - (uintptr_t)stack));
  root = (TValue *)((char *)stack + root_offset);
  top = (TValue *)((char *)stack + top_offset);
  for (tv = root; tv <= top; tv++)
    if (tvisgcv(tv) && gcV(tv) == target_object)
      return 1;
  return 0;
}

static void rooted_read_hook(lua_State *L, cTValue *outroot)
{
  if (L != owner_L || lj_tg_load_jit_base(owner_tg) == NULL)
    return;
  if (tvisgcv(outroot) && gcV(outroot) == target_object)
    la_store32_rel(&target_root_seen, 1);
  if (tviscdata(outroot) && gcV(outroot) == gate_object) {
    assert(la_load32_acq(&target_root_seen) == 1);
    gate_arm_func();
    la_store32_rel(&callback_armed_seen, 1);
  }
}

static void assert_suspended_callback(lua_State *L)
{
  TGState *tg = L2TG(L);
  const LJFFINativeFrame *frame;
  uint32_t flags;
  assert(L == owner_L && tg == owner_tg);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert((flags & FRAME_STATE) == LJ_FFI_NATIVE_FRAME_F_SUSPENDED);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN) != 0);
  assert(lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(frame_has_target_root(frame));
  la_add32_acqrel(&callback_checks, 1);
}

static int callback_probe(lua_State *L)
{
  assert_suspended_callback(L);
  lua_pushboolean(L, 1);
  return 1;
}

static void assert_pair_roots(const LJFFINativeFrame *frame)
{
  uint64_t root_offset = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t top_offset = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(owner_L->stack);
  TValue *root, *top, *tv;
  unsigned i;
  assert(root_offset <= top_offset && root_offset % sizeof(TValue) == 0 &&
         top_offset % sizeof(TValue) == 0);
  assert((uintptr_t)tvref(owner_L->maxstack) >= (uintptr_t)stack &&
         top_offset + sizeof(TValue) <=
           (uint64_t)((uintptr_t)tvref(owner_L->maxstack) -
                     (uintptr_t)stack));
  root = (TValue *)((char *)stack + root_offset);
  top = (TValue *)((char *)stack + top_offset);
  for (i = 0; i < 2; i++) {
    int found = 0;
    for (tv = root; tv <= top; tv++)
      if (tviscdata(tv) && cdataV(tv) == pair_args[i]) {
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
  assert(tg == owner_tg && owner_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) & FRAME_STATE) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == owner_L);
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  func = lj_ffi_native_frame_func_acq(frame);
  if (func == gate_func) gate_finishes++;
  else if (func == warm_target_func) warm_target_finishes++;
  else if (func == target_func) {
    assert(la_load32_acq(&callback_checks) >= 2u ||
           la_load32_acq(&callback_armed_seen) == 0);
    target_finishes++;
  } else {
    assert(func == pair_func);
    assert_pair_roots(frame);
    pair_finishes++;
  }
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
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

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) >= 3u &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
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
    fprintf(stderr, "table-chain callback fixture: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer counter(lua_State *L, const char *name)
{
  lua_Integer result;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static int run_driver(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__chains_cb_driver");
  lua_getglobal(L, "__chains_cb_run");
  lua_getglobal(L, "__chains_cb_registry");
  lua_pushinteger(L, n);
  return lua_pcall(L, 3, 1, 0);
}

static void assert_clean(TGState *tg, GCtrace *T)
{
  uint32_t i;
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0 && lj_tg_load_jit_base(tg) == NULL);
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

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;
  int status, saved_errno;

  owner_L = L;
  owner_tg = tg;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__chains_cb_set_errno");
  lua_pushcfunction(L, callback_probe);
  lua_setglobal(L, "__chains_cb_probe");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[int32_t lj_callxs_table_first(int32_t); int32_t lj_callxs_table_second(int32_t); void lj_callxs_table_configure(void); int32_t lj_callxs_table_first_count(void); int32_t lj_callxs_table_second_count(void);]]\n"
    "ffi.cdef[[int32_t lj_callxs_table_i64_pair(int64_t,int64_t); int32_t lj_callxs_table_i64_pair_count(void); int32_t lj_callxs_table_i64_total_count(void); void lj_callxs_table_i64_configure(int32_t);]]\n"
    "ffi.cdef[[typedef int32_t (*lj_chains_cb_t)(int32_t); void lj_table_chains_callback_set(lj_chains_cb_t); void lj_table_chains_callback_configure(void); void lj_table_chains_callback_arm(void); int32_t lj_table_chains_callback_gate(int32_t); int32_t lj_table_chains_callback_gate_count(void); int32_t lj_table_chains_callback_count(void);]]\n"
    "local l1=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_TABLE_SO'))); local l2=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_I64_ARGS_SO'))); local l3=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_CHAINS_CALLBACK_SO')))\n"
    "local ctype=ffi.typeof('int32_t(*)(int32_t)'); local ptype=ffi.typeof('int32_t(*)(int64_t,int64_t)')\n"
    "local warm=ffi.cast(ctype,l1.lj_callxs_table_first); local gate=ffi.cast(ctype,l3.lj_table_chains_callback_gate); local pair=ffi.cast(ptype,l2.lj_callxs_table_i64_pair)\n"
    "local args={ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',-1)}\n"
    "local warmleaf={child={call=warm},args=args,pair=pair}; local registry={root={branch={leaf=warmleaf}},callback=gate,callback_root=false}\n"
    "_G.__chains_cb_registry=registry; _G.__chains_cb_ctype=ctype; _G.__chains_cb_gate=gate; _G.__chains_cb_gate_arm=l3.lj_table_chains_callback_arm; _G.__chains_cb_second=ffi.cast(ctype,l1.lj_callxs_table_second); _G.__chains_cb_pair=pair; _G.__chains_cb_warm_target=warm; _G.__chains_cb_weak=false; _G.__chains_cb_target_capture=false; _G.__chains_cb_closure_capture=false\n"
    "_G.__chains_cb_first_count=l1.lj_callxs_table_first_count; _G.__chains_cb_second_count=l1.lj_callxs_table_second_count; _G.__chains_cb_pair_count=l2.lj_callxs_table_i64_pair_count; _G.__chains_cb_total_count=l2.lj_callxs_table_i64_total_count; _G.__chains_cb_gate_count=l3.lj_table_chains_callback_gate_count; _G.__chains_cb_calls=l3.lj_table_chains_callback_count\n"
    "_G.__chains_cb_configure=function() l1.lj_callxs_table_configure(); l2.lj_callxs_table_i64_configure(0); l3.lj_table_chains_callback_configure() end\n"
    "local ready=threading.channel(1); local release=threading.channel(1); _G.__chains_cb_release=release; _G.__chains_cb_peer=threading.spawn(function(r,d) assert(r:send(true,5)); local _,ok=d:recv(30); assert(ok); return true end,ready,release); local _,ok=ready:recv(5); assert(ok)\n"
    "function _G.__chains_cb_run(reg,n) local root=reg.root; local sum=0.5; for i=1,n do local target=root.branch.leaf; local gate=reg.callback; sum=sum+gate(i); sum=sum+target.child.call(i); local a=target.args[1]; local b=target.args[2]; sum=sum+target.pair(a,b) end; return sum-0.5 end\n"
    "local set_errno=__chains_cb_set_errno; function _G.__chains_cb_driver(run,reg,n) set_errno(); local r=run(reg,n); return r end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=32')\n");

  gate_func = capture_func(L, "__chains_cb_gate");
  gate_arm_func = capture_void_func(L, "__chains_cb_gate_arm");
  warm_target_func = capture_func(L, "__chains_cb_warm_target");
  target_func = capture_func(L, "__chains_cb_second");
  pair_func = capture_func(L, "__chains_cb_pair");
  capture_pair_args(L);

  expected_trace = NULL;
  native_finishes = gate_finishes = warm_target_finishes = target_finishes =
    pair_finishes = 0;
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = run_driver(L, 32);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_tonumber(L, -1) ==
         32.0 * 33.0 / 2.0 + 17.0 * 32.0 + 32.0);
  lua_pop(L, 1);
  assert(saved_errno == EDOM && native_finishes != 0 && gate_finishes != 0 &&
         warm_target_finishes != 0 && pair_finishes != 0);
  T = find_trace(J, global_proto(L, "__chains_cb_run"));
  assert(T != NULL && T == expected_trace && op_count(T, IR_CALLXS) >= 3u);
  assert_clean(tg, T);

  /* Install a callback and a unique target after the trace is published.
  ** The callback function captures only registry and weak witness, never the
  ** target itself. */
  run_lua(L,
    "local ffi=require('ffi'); local reg=__chains_cb_registry\n"
    "local ctype=__chains_cb_ctype; local target={child={call=ffi.cast(ctype,__chains_cb_second)},args={ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',-1)},pair=__chains_cb_pair}\n"
    "local weak=setmetatable({target},{__mode='v'})\n"
    "local cb=ffi.cast(ctype,function(value) if value==2 then assert(__chains_cb_probe()); reg.root.branch.leaf=nil; collectgarbage('collect'); assert(weak[1]~=nil); assert(__chains_cb_probe()) end; return 0 end)\n"
    "reg.root.branch.leaf=target; reg.callback_root=cb; local l3=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_CHAINS_CALLBACK_SO'))); l3.lj_table_chains_callback_set(cb)\n"
    "_G.__chains_cb_weak=weak; _G.__chains_cb_target_capture=target; _G.__chains_cb_closure_capture=cb\n");
  target_object = capture_object(L, "__chains_cb_target_capture");
  lua_getglobal(L, "__chains_cb_gate");
  assert(tviscdata(L->top - 1));
  gate_object = obj2gco(cdataV(L->top - 1));
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "__chains_cb_target_capture");
  lua_pushnil(L);
  lua_setglobal(L, "__chains_cb_closure_capture");
  capture_pair_args(L);
  run_lua(L, "__chains_cb_configure()");
  la_store32_rel(&target_root_seen, 0);
  la_store32_rel(&callback_armed_seen, 0);
  la_store32_rel(&callback_checks, 0);
  native_finishes = gate_finishes = target_finishes = pair_finishes = 0;
  lj_tab_test_set_rooted_gc_read_hook(rooted_read_hook);
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = run_driver(L, 2);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  lj_tab_test_set_rooted_gc_read_hook(NULL);
  assert(status == LUA_OK && lua_tonumber(L, -1) == 213.0);
  lua_pop(L, 1);
  assert(saved_errno == EDOM);
  assert(la_load32_acq(&target_root_seen) == 1);
  assert(la_load32_acq(&callback_armed_seen) == 1);
  assert(la_load32_acq(&callback_checks) >= 2u);
  assert(counter(L, "__chains_cb_calls") == 1);
  assert(counter(L, "__chains_cb_gate_count") == 2);
  assert(counter(L, "__chains_cb_second_count") == 2);
  assert(counter(L, "__chains_cb_pair_count") == 2);
  assert(counter(L, "__chains_cb_total_count") == 2);
  assert(counter(L, "__chains_cb_first_count") == 0);
  assert(native_finishes != 0 && gate_finishes != 0);
  assert(T == find_trace(J, global_proto(L, "__chains_cb_run")));
  assert_clean(tg, T);

  run_lua(L,
    "assert(__chains_cb_registry.root.branch.leaf==nil)\n"
    "__chains_cb_registry.callback_root=nil\n"
    "jit.flush(); collectgarbage('collect'); collectgarbage('collect')\n"
    "assert(__chains_cb_weak[1]==nil,'nested target remained strongly reachable')\n"
    "assert(__chains_cb_release:send(true,5)); local joined,res=__chains_cb_peer:join(5); assert(joined and res)\n");
  target_object = NULL;
  gate_object = NULL;
  expected_trace = NULL;
  owner_L = NULL;
  owner_tg = NULL;
  lua_close(L);
  puts("t-arm64-jit-table-chains-callback OK: suspended nested root and callback GC");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-chains-callback SKIP: requires ARM64 CALLXS and table hooks");
  return 0;
}

#endif
