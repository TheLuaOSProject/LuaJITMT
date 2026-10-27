/* A live JIT lease preserves a table-loaded FFI CData through a later table
** hit which overwrites the helper scratch root, up to native CALLXS entry. */

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
#error "t-arm64-jit-callxs-table-lifecycle requires ARM64 CALLXS admission"
#endif

static TGState *owner_tg;
static lua_State *owner_L;
static GCtrace *native_trace;
static GCobj *target_object;
static GCobj *sentinel_object;
static void *target_func;
static void *sentinel_func;
static uint32_t pause_armed;
static uint32_t pause_seen;
static uint32_t release_native;
static uint32_t removed_edge;
static uint32_t phase_attempted;
static uint32_t target_native;
static uint32_t sentinel_native;
static uint32_t native_finishes;

static void capture(lua_State *L, const char *name, void **func_out,
                   GCobj **object_out)
{
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  memcpy(func_out, cdataptr(cd), sizeof(*func_out));
  *object_out = obj2gco(cd);
  assert(*func_out != NULL && *object_out != NULL);
  lua_pop(L, 1);
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
        op_count(T, IR_CALLXS) >= 2u &&
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
  void *func;
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
  func = lj_ffi_native_frame_func_acq(frame);
  if (func == target_func) {
    assert(la_load32_acq(&removed_edge) == 1);
    assert(la_load32_acq(&phase_attempted) == 1);
    target_native++;
  } else {
    assert(func == sentinel_func);
    sentinel_native++;
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
    fprintf(stderr, "table CALLXS lifecycle Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int call_runner(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__table_lifecycle_driver");
  lua_getglobal(L, "__table_lifecycle_run");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
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
  double expected;

  owner_tg = tg;
  owner_L = L;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__table_lifecycle_set_errno");
  lua_pushcfunction(L, pause_seen_fn);
  lua_setglobal(L, "__table_lifecycle_pause_seen");
  lua_pushcfunction(L, mark_removed_fn);
  lua_setglobal(L, "__table_lifecycle_mark_removed");
  lua_pushcfunction(L, attempt_gc_fn);
  lua_setglobal(L, "__table_lifecycle_attempt_gc");
  lua_pushcfunction(L, release_native_fn);
  lua_setglobal(L, "__table_lifecycle_release_native");
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_table_configure(void);\n"
    "int32_t lj_callxs_table_first_count(void);\n"
    "int32_t lj_callxs_table_second_count(void);\n"
    "int32_t lj_callxs_table_first(int32_t);\n"
    "int32_t lj_callxs_table_second(int32_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_TABLE_SO')))\n"
    "_G.__table_lifecycle_library=lib\n"
    "local calltype=ffi.typeof('int32_t(*)(int32_t)')\n"
    "local warm=ffi.cast(calltype,lib.lj_callxs_table_second)\n"
    "local keep=ffi.cast(calltype, warm)\n"
    "local target=ffi.cast(calltype, lib.lj_callxs_table_first)\n"
    "_G.__table_lifecycle_target=target; _G.__table_lifecycle_keep=keep\n"
    "_G.__table_lifecycle_first_count=lib.lj_callxs_table_first_count\n"
    "_G.__table_lifecycle_second_count=lib.lj_callxs_table_second_count\n"
    "_G.__table_lifecycle_configure=lib.lj_callxs_table_configure\n"
    "_G.__table_lifecycle_registry={call=warm,keep=keep}\n"
    "_G.__table_lifecycle_weak=setmetatable({target},{__mode='v'})\n"
    "_G.__table_lifecycle_ready=threading.channel(1)\n"
    "_G.__table_lifecycle_done=threading.channel(1)\n"
    "_G.__table_lifecycle_peer=threading.spawn(function(reg,weak,ready,done)\n"
    "  assert(ready:send(true,5))\n"
    "  for i=1,30000 do\n"
    "    if __table_lifecycle_pause_seen() then break end\n"
    "    threading.sleep(0.001)\n"
    "  end\n"
    "  assert(__table_lifecycle_pause_seen(),'rooted table read pause timeout')\n"
    "  reg.call=nil\n"
    "  __table_lifecycle_mark_removed()\n"
    "  assert(__table_lifecycle_attempt_gc())\n"
    "  assert(weak[1]~=nil,'target vanished before native call')\n"
    "  __table_lifecycle_release_native()\n"
    "  assert(done:send(true,5))\n"
    "  return true\n"
    "end,__table_lifecycle_registry,__table_lifecycle_weak,\n"
    "  __table_lifecycle_ready,__table_lifecycle_done)\n"
    "local _,ready_ok=__table_lifecycle_ready:recv(5); assert(ready_ok)\n"
    "function _G.__table_lifecycle_run(reg,n)\n"
    "  local sum=0.5\n"
    "  for i=1,n do\n"
    "    local fn=reg.call; local witness=reg.keep\n"
    "    sum=sum+fn(i)+witness(0)\n"
    "  end\n"
    "  return sum-0.5\n"
    "end\n"
    "local set_errno=__table_lifecycle_set_errno\n"
    "function _G.__table_lifecycle_driver(run,n)\n"
    "  set_errno(); local result=run(__table_lifecycle_registry,n); return result\n"
    "end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=16')\n");

  assert(mt_active_acq(g) != 0);
  capture(L, "__table_lifecycle_target", &target_func, &target_object);
  capture(L, "__table_lifecycle_keep", &sentinel_func, &sentinel_object);
  assert(target_object != sentinel_object);

  /* Compile the loop against a different callable. The unique target is only
  ** installed in the table after this trace exists, and is held by a weak
  ** witness plus that one table edge. */
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  errno = 0;
  status = call_runner(L, 32);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  expected = 3.0 * 32.0 * 33.0 / 2.0 + 202.0 * 32.0;
  assert(lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(errno == EDOM && native_finishes != 0 && sentinel_native != 0);
  T = find_trace(J, global_proto(L, "__table_lifecycle_run"));
  assert(T != NULL && T == native_trace);
  assert_clean(tg, T);

  /* Make the target the sole strong table value, then remove its global root.
  ** For n=2 the first iteration is interpreted and the FORL trace runs once. */
  run_lua(L,
    "__table_lifecycle_registry.call=__table_lifecycle_target\n"
    "__table_lifecycle_target=nil\n"
    "collectgarbage('collect')\n"
    "assert(__table_lifecycle_weak[1]~=nil)\n"
    "__table_lifecycle_configure()\n");
  assert(tvisnil(&tg->tmptv3));
  la_store32_rel(&pause_armed, 1);
  la_store32_rel(&pause_seen, 0);
  la_store32_rel(&release_native, 0);
  la_store32_rel(&removed_edge, 0);
  la_store32_rel(&phase_attempted, 0);
  target_native = sentinel_native = native_finishes = 0;
  native_trace = NULL;
  lj_tab_test_set_rooted_gc_read_hook(rooted_read_hook);
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  errno = 0;
  status = call_runner(L, 2);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  assert(lua_tonumber(L, -1) == 239.0);
  lua_pop(L, 1);
  assert(errno == EDOM);
  assert(la_load32_acq(&pause_seen) == 1);
  assert(la_load32_acq(&removed_edge) == 1);
  assert(la_load32_acq(&phase_attempted) == 1);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(target_native == 1u && sentinel_native == 1u);
  assert(native_finishes == 2u);
  assert(counter(L, "__table_lifecycle_first_count") == 2);
  assert(counter(L, "__table_lifecycle_second_count") == 2);
  T = find_trace(J, global_proto(L, "__table_lifecycle_run"));
  assert(T != NULL && T == native_trace);
  assert_clean(tg, T);

  run_lua(L,
    "local ok,result=__table_lifecycle_peer:join(5)\n"
    "assert(ok and result and __table_lifecycle_registry.call==nil)\n"
    "jit.flush(); collectgarbage('collect')\n"
    "assert(__table_lifecycle_weak[1]==nil, 'target CData retained after trace flush')\n");
  lj_tab_test_set_rooted_gc_read_hook(NULL);
  native_trace = NULL;
  target_object = NULL;
  sentinel_object = NULL;
  owner_L = NULL;
  owner_tg = NULL;
  lua_close(L);
  puts("t-arm64-jit-callxs-table-lifecycle OK: pre-frame GC deferral, "
       "table-edge removal, target identity, and eventual reclamation");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-table-lifecycle SKIP: requires ARM64 experimental "
       "CALLXS and rooted table hooks");
  return 0;
}

#endif
