/* A live JIT lease preserves an owned I64 argument after a second table
** hit overwrites the helper scratch root and a peer removes its table edge. */

#include <assert.h>
#include <errno.h>
#include <sched.h>
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
#error "t-arm64-jit-table-i64-args-lifecycle requires ARM64 CALLXS admission"
#endif

static TGState *owner_tg;
static lua_State *owner_L;
static GCtrace *native_trace;
static GCobj *target_object;
static GCobj *sentinel_object;
static void *sink_func;
static GCobj *warm_object;
static GCobj *argument_object;
static uint32_t pause_armed;
static uint32_t pause_seen;
static uint32_t release_native;
static uint32_t removed_edge;
static uint32_t phase_attempted;
static uint32_t native_finishes;

static GCobj *capture_object(lua_State *L, const char *name, uint64_t want)
{
  GCcdata *cd;
  uint64_t bits;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(&bits, cdataptr(cd), sizeof(bits));
  assert(bits == want);
  lua_pop(L, 1);
  return obj2gco(cd);
}

static void assert_argument_roots(const LJFFINativeFrame *frame)
{
  uint64_t start = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t end = lj_ffi_native_frame_top_offset_acq(frame);
  TValue *stack = tvref(owner_L->stack), *tv, *last;
  unsigned found = 0;
  assert(start <= end && start % sizeof(TValue) == 0 &&
         end % sizeof(TValue) == 0);
  assert(end + sizeof(TValue) <=
         (uintptr_t)tvref(owner_L->maxstack) - (uintptr_t)stack);
  tv = (TValue *)((char *)stack + start);
  last = (TValue *)((char *)stack + end);
  for (; tv <= last; tv++) {
    if (tviscdata(tv) && gcV(tv) == argument_object) found |= 1u;
    if (tviscdata(tv) && gcV(tv) == sentinel_object) found |= 2u;
  }
  assert(found == 3u);
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
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && trace_runnable_acq(T, traceno) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) != 0u &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
  }
  return NULL;
}

static void rooted_read_hook(lua_State *L, cTValue *outroot)
{
  uint32_t expected = 1;
  uint32_t i;
  if (L != owner_L || lj_tg_load_jit_base(owner_tg) == NULL ||
      !tviscdata(outroot) ||
      gcV(outroot) != sentinel_object ||
      !la_cas32(&pause_armed, &expected, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  /* This is the second rooted hit: the target CData was already loaded, and
  ** this later result now occupies the table helper's shared output root. */
  assert(outroot == &owner_tg->tmptv);
  assert(lj_tg_load_jit_base(owner_tg) != NULL);
  assert(lj_ffi_native_frame_depth_acq(owner_tg) == 0);
  la_store32_rel(&pause_seen, 1);
  for (i = 0; i < 50000000u && !la_load32_acq(&release_native); i++)
    (void)sched_yield();
  assert(la_load32_acq(&release_native) == 1);
  assert(la_load32_acq(&removed_edge) == 1);
  assert(la_load32_acq(&phase_attempted) == 1);
}

static void observe_native_finish(TGState *tg)
{
  const LJFFINativeFrame *frame;
  assert(tg == owner_tg && owner_L != NULL);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) &
          (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
           LJ_FFI_NATIVE_FRAME_F_POSTCALL)) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert(lj_ffi_native_frame_L_acq(frame) == owner_L);
  if (native_trace == NULL)
    native_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(native_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == native_trace);
  assert(trace_native_pins_acq(native_trace) != 0);
  assert(lj_ffi_native_frame_func_acq(frame) == sink_func);
  assert((lj_ffi_native_frame_flags_acq(frame) &
          LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert_argument_roots(frame);
  if (argument_object == target_object) {
    assert(la_load32_acq(&removed_edge) == 1);
    assert(la_load32_acq(&phase_attempted) == 1);
  }
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
}

static int pause_seen_fn(lua_State *L)
{
  lua_pushboolean(L, la_load32_acq(&pause_seen) != 0);
  return 1;
}

static int mark_removed_fn(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&removed_edge, 1);
  return 0;
}

static int attempt_gc_fn(lua_State *L)
{
  global_State *g = G(L);
  TGState *peer_tg = L2TG(L);
  assert(peer_tg != NULL && peer_tg != owner_tg);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_force_major(g);
  assert(lj_gc2_request_cycle_explicit(g, peer_tg));
  /* The owner is between its table-hit helper and native-frame publication.
  ** The collector closes admission but must defer the phase transition. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(!lj_gc2_jit_entry_open(g));
  assert(lj_tg_load_jit_base(owner_tg) != NULL);
  la_store32_rel(&phase_attempted, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int release_native_fn(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&release_native, 1);
  return 0;
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
    fprintf(stderr, "table I64 lifecycle Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int call_runner(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__table_i64_lifecycle_driver");
  lua_getglobal(L, "__table_i64_lifecycle_run");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
}

static void drain_gc_to_idle(lua_State *L)
{
  global_State *g = G(L);
  uint32_t i;
  for (i = 0; i < 2000000u &&
       (gc2_phase_acq(g) != LJ_GC2_IDLE || !lj_gc2_jit_entry_open(g)); i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE && lj_gc2_jit_entry_open(g));
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
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

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;
  int status;
  unsigned attempts;

  owner_tg = tg;
  owner_L = L;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__table_i64_lifecycle_set_errno");
  lua_pushcfunction(L, pause_seen_fn);
  lua_setglobal(L, "__table_i64_lifecycle_pause_seen");
  lua_pushcfunction(L, mark_removed_fn);
  lua_setglobal(L, "__table_i64_lifecycle_mark_removed");
  lua_pushcfunction(L, attempt_gc_fn);
  lua_setglobal(L, "__table_i64_lifecycle_attempt_gc");
  lua_pushcfunction(L, release_native_fn);
  lua_setglobal(L, "__table_i64_lifecycle_release_native");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_table_i64_configure(int32_t);\n"
    "int32_t lj_callxs_table_i64_pair_count(void);\n"
    "int32_t lj_callxs_table_i64_pair(int64_t,int64_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_I64_ARGS_SO')))\n"
    "_G.__table_i64_lifecycle_library=lib\n"
    "local warm=ffi.new('int64_t',0x8000000000000000ULL)\n"
    "local keep=ffi.new('int64_t',-1LL)\n"
    "local target=ffi.new('int64_t',0x8000000000000000ULL)\n"
    "_G.__table_i64_lifecycle_target=target; _G.__table_i64_lifecycle_keep=keep\n"
    "_G.__table_i64_lifecycle_warm=warm\n"
    "_G.__table_i64_lifecycle_count=lib.lj_callxs_table_i64_pair_count\n"
    "_G.__table_i64_lifecycle_sink=lib.lj_callxs_table_i64_pair\n"
    "_G.__table_i64_lifecycle_configure=lib.lj_callxs_table_i64_configure\n"
    "_G.__table_i64_lifecycle_registry={value=warm,keep=keep}\n"
    "_G.__table_i64_lifecycle_weak=setmetatable({target},{__mode='v'})\n"
    "_G.__table_i64_lifecycle_ready=threading.channel(1)\n"
    "_G.__table_i64_lifecycle_done=threading.channel(1)\n"
    "_G.__table_i64_lifecycle_peer=threading.spawn(function(reg,weak,ready,done)\n"
    "  assert(ready:send(true,5))\n"
    "  for i=1,30000 do\n"
    "    if __table_i64_lifecycle_pause_seen() then break end\n"
    "    threading.sleep(0.001)\n"
    "  end\n"
    "  assert(__table_i64_lifecycle_pause_seen(),'rooted table read pause timeout')\n"
    "  reg.value=nil\n"
    "  __table_i64_lifecycle_mark_removed()\n"
    "  assert(__table_i64_lifecycle_attempt_gc())\n"
    "  assert(weak[1]~=nil,'target vanished before native call')\n"
    "  __table_i64_lifecycle_release_native()\n"
    "  assert(done:send(true,5))\n"
    "  return true\n"
    "end,__table_i64_lifecycle_registry,__table_i64_lifecycle_weak,\n"
    "  __table_i64_lifecycle_ready,__table_i64_lifecycle_done)\n"
    "local _,ready_ok=__table_i64_lifecycle_ready:recv(5); assert(ready_ok)\n"
    "function _G.__table_i64_lifecycle_run(reg,fn,n)\n"
    "  local sum=0.5\n"
    "  for i=1,n do\n"
    "    local value=reg.value; local witness=reg.keep\n"
    "    sum=sum+fn(value,witness)\n"
    "  end\n"
    "  return sum-0.5\n"
    "end\n"
    "local set_errno=__table_i64_lifecycle_set_errno\n"
    "function _G.__table_i64_lifecycle_driver(run,n)\n"
    "  set_errno(); local result=run(__table_i64_lifecycle_registry,__table_i64_lifecycle_sink,n); return result\n"
    "end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=16')\n");

  assert(mt_active_acq(g) != 0);
  target_object = capture_object(L, "__table_i64_lifecycle_target",
                                 UINT64_C(0x8000000000000000));
  sentinel_object = capture_object(L, "__table_i64_lifecycle_keep", UINT64_MAX);
  warm_object = capture_object(L, "__table_i64_lifecycle_warm",
                               UINT64_C(0x8000000000000000));
  assert(target_object != sentinel_object && target_object != warm_object);
  argument_object = warm_object;
  lua_getglobal(L, "__table_i64_lifecycle_sink");
  assert(tviscdata(L->top - 1));
  memcpy(&sink_func, cdataptr(cdataV(L->top - 1)), sizeof(sink_func));
  assert(sink_func != NULL);
  lua_pop(L, 1);
  run_lua(L, "__table_i64_lifecycle_warm=nil");

  /* Compile against a different object with the same CType and bits. The
  ** unique target is only installed after the trace exists, so its address
  ** cannot accidentally become a trace constant that keeps it alive. */
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  errno = 0;
  status = call_runner(L, 32);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == 32.0);
  lua_pop(L, 1);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(errno == EDOM && native_finishes != 0);
  T = find_trace(J, global_proto(L, "__table_i64_lifecycle_run"));
  assert(T != NULL && T == native_trace);
  assert_clean(tg, T);

  /* Make the target the sole strong table value, then remove its global root.
  ** For n=2 the first iteration is interpreted and the FORL trace runs once. */
  run_lua(L,
    "__table_i64_lifecycle_registry.value=__table_i64_lifecycle_target\n"
    "__table_i64_lifecycle_target=nil\n"
    "collectgarbage('collect')\n"
    "assert(__table_i64_lifecycle_weak[1]~=nil)\n"
    "__table_i64_lifecycle_configure(0)\n");
  /* A requested full collection may still have deferred cycle work. Require
  ** native entry to be open before expecting the two-iteration run to enter. */
  drain_gc_to_idle(L);
  assert_clean(tg, T);
  assert(tvisnil(&tg->tmptv3));
  la_store32_rel(&pause_armed, 1);
  la_store32_rel(&pause_seen, 0);
  la_store32_rel(&release_native, 0);
  la_store32_rel(&removed_edge, 0);
  la_store32_rel(&phase_attempted, 0);
  native_finishes = 0;
  argument_object = target_object;
  native_trace = NULL;
  lj_tab_test_set_rooted_gc_read_hook(rooted_read_hook);
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  /* Bounded table-hit helpers may refuse entry during concurrent activity.
  ** Retry a whole two-iteration run only while no pause/native effect occurred;
  ** the final assertions require the actual lease-to-frame handoff. */
  for (attempts = 1; attempts <= 32u; attempts++) {
    drain_gc_to_idle(L);
    errno = 0;
    status = call_runner(L, 2);
    assert(status == LUA_OK && lua_isnumber(L, -1));
    assert(lua_tonumber(L, -1) == 2.0);
    lua_pop(L, 1);
    assert(errno == EDOM);
    if (la_load32_acq(&pause_seen)) break;
    assert(native_finishes == 0 && la_load32_acq(&removed_edge) == 0 &&
           la_load32_acq(&phase_attempted) == 0);
    assert_clean(tg, T);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(attempts <= 32u);
  assert(la_load32_acq(&pause_seen) == 1);
  assert(la_load32_acq(&removed_edge) == 1);
  assert(la_load32_acq(&phase_attempted) == 1);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(native_finishes == 1u);
  assert(counter(L, "__table_i64_lifecycle_count") == 2*(lua_Integer)attempts);
  T = find_trace(J, global_proto(L, "__table_i64_lifecycle_run"));
  assert(T != NULL && T == native_trace);
  assert_clean(tg, T);

  run_lua(L,
    "local ok,result=__table_i64_lifecycle_peer:join(5)\n"
    "assert(ok and result and __table_i64_lifecycle_registry.value==nil)\n"
    "jit.flush(); collectgarbage('collect')\n"
    "assert(__table_i64_lifecycle_weak[1]==nil, 'target CData retained after trace flush')\n");
  lj_tab_test_set_rooted_gc_read_hook(NULL);
  native_trace = NULL;
  target_object = warm_object = argument_object = NULL;
  sentinel_object = NULL;
  owner_L = NULL;
  owner_tg = NULL;
  lua_close(L);
  puts("t-arm64-jit-table-i64-args-lifecycle OK: pre-frame GC deferral, "
       "table-edge removal, exact argument roots and payload, and reclamation");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-i64-args-lifecycle SKIP: requires ARM64 experimental "
       "CALLXS and rooted table hooks");
  return 0;
}

#endif
