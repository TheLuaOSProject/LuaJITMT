/* Native lifecycle coverage for a shared-table TGETS -> CALLXS loop. */

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

#include "lj_bc.h"
#include "lj_ccall.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "t-arm64-jit-callxs-table requires ARM64 CALLXS admission"
#endif

static TGState *expected_tg;
static lua_State *expected_L;
static GCtrace *expected_trace;
static void *first_func;
static void *second_func;
static void *force_func;
static uint32_t first_native;
static uint32_t second_native;
static uint32_t native_finishes;
static uint32_t forced_finishes;
static uint64_t old_epoch = UINT64_MAX;
static MSize expected_callback_slot;
static uint8_t expected_callback_stopreq;
static void *expected_ffi_func;
static uint32_t active_pause_armed;
static uint32_t active_pause_seen;
static uint32_t active_release;
static uint32_t active_mutated;

static void observe_active_table_read(lua_State *L, cTValue *outroot)
{
  uint32_t expected = 1;
  uint32_t i;
  TGState *tg;
  if (L != expected_L || lj_tg_load_jit_base(expected_tg) == NULL ||
      !tviscdata(outroot) ||
      !la_cas32(&active_pause_armed, &expected, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  tg = L2TG(L);
  assert(tg == expected_tg && lj_tg_load_jit_base(tg) != NULL);
  la_store32_rel(&active_pause_seen, 1);
  for (i = 0; i < 50000000u && !la_load32_acq(&active_release); i++)
    (void)sched_yield();
  assert(la_load32_acq(&active_release) == 1);
  assert(la_load32_acq(&active_mutated) == 1);
}

static int active_pause_seen_fn(lua_State *L)
{
  UNUSED(L);
  lua_pushboolean(L, la_load32_acq(&active_pause_seen) != 0);
  return 1;
}

static int active_mark_mutated_fn(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&active_mutated, 1);
  return 0;
}

static int active_release_fn(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&active_release, 1);
  return 0;
}

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

static GCtrace *find_call_trace(jit_State *J, GCproto *pt)
{
  TraceNo traceno;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL && T == expected_trace && trace_runnable_acq(T, traceno) &&
        trace_root_acq(T) == 0 && trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) != 0 &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE))
      return T;
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
  if (expected_trace == NULL)
    expected_trace = lj_ffi_native_frame_trace_acq(frame);
  assert(expected_trace != NULL &&
         lj_ffi_native_frame_trace_acq(frame) == expected_trace);
  assert(trace_native_pins_acq(expected_trace) != 0);
  assert(lj_ffi_native_frame_L_acq(frame) == expected_L);
  func = lj_ffi_native_frame_func_acq(frame);
  if (func == first_func)
    first_native++;
  else {
    assert(func == second_func);
    second_native++;
  }
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  native_finishes++;
}

static void force_postcall_epoch(TGState *tg)
{
  void *func = lj_ffi_native_frame_func_acq(&tg->ffi_native_frame[0]);
  uint64_t epoch;
  observe_finish(tg);
  if (func != force_func || forced_finishes != 0)
    return;
  epoch = lj_tg_hs_epoch_ack_acq(tg);
  assert(epoch != UINT64_MAX);
  old_epoch = epoch;
  forced_finishes++;
  lj_tg_hs_epoch_ack_rel(tg, epoch + 1u);
}

static int set_errno_caller(lua_State *L)
{
  (void)L;
  errno = E2BIG;
  return 0;
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

static int proto_has_tgets(lua_State *L, const char *name)
{
  GCproto *pt;
  const BCIns *bc;
  MSize i;
  int found = 0;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  bc = proto_bc(pt);
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_TGETS) {
      found = 1;
      break;
    }
  }
  lua_pop(L, 1);
  return found;
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "table CALLXS fixture Lua error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int call_runner(lua_State *L, lua_Integer n)
{
  lua_getglobal(L, "__table_driver");
  lua_getglobal(L, "__table_call_s");
  lua_pushinteger(L, n);
  return lua_pcall(L, 2, 1, 0);
}

static int call_runner_error(lua_State *L, const char *text)
{
  int status;
  lua_getglobal(L, "__table_driver");
  lua_getglobal(L, "__table_call_s");
  lua_pushinteger(L, 8);
  status = lua_pcall(L, 2, 1, 0);
  assert(status == LUA_ERRRUN);
  assert(lua_tostring(L, -1) != NULL && strstr(lua_tostring(L, -1), text));
  lua_pop(L, 1);
  return status;
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
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(tg->ffi_xsave_root == NULL && tg->ffi_xsave_baseslot == 0 &&
         tg->ffi_xsave_nslots == 0);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == expected_callback_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) ==
         expected_callback_stopreq);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == expected_ffi_func);
  assert(trace_native_pins_acq(T) == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

static GCtrace *run_success(lua_State *L, jit_State *J, TGState *tg,
                            uint32_t which, lua_Integer n)
{
  GCtrace *T;
  double expected;
  int status, saved_errno;
  expected_trace = NULL;
  native_finishes = first_native = second_native = 0;
  force_func = NULL;
  forced_finishes = 0;
  old_epoch = UINT64_MAX;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  errno = 0;
  status = call_runner(L, n);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_isnumber(L, -1));
  expected = (which == 1 ? 1.0 : 3.0) *
             (double)n * ((double)n + 1.0) / 2.0 +
             (which == 1 ? 17.0 : 101.0) * (double)n;
  assert(lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  assert(saved_errno == EDOM);
  T = find_call_trace(J, global_proto(L, "__table_call_s"));
  if (T == NULL || native_finishes == 0)
    fprintf(stderr, "table call identity %u: native=%u trace=%p\n",
            which, native_finishes, (void *)expected_trace);
  assert(T != NULL && expected_trace == T && native_finishes != 0);
  assert(bc_op(trace_startins_acq(T)) == BC_FORL);
  if (which == 1) assert(first_native != 0);
  else assert(second_native != 0);
  assert_frame_clean(tg, T);
  return T;
}

static void replace_from_peer(lua_State *L, const char *command)
{
  char chunk[128];
  int n = snprintf(chunk, sizeof(chunk),
                   "assert(__table_go:send('%s', 5)); "
                   "local _, ok=__table_done:recv(5); assert(ok)", command);
  assert(n > 0 && (size_t)n < sizeof(chunk));
  run_lua(L, chunk);
}

static void start_active_replacement(lua_State *L, const char *command)
{
  char chunk[128];
  int n = snprintf(chunk, sizeof(chunk),
                   "assert(__table_go:send('%s', 5))", command);
  assert(n > 0 && (size_t)n < sizeof(chunk));
  run_lua(L, chunk);
}

static void finish_active_replacement(lua_State *L)
{
  run_lua(L, "local _,ok=__table_done:recv(5); assert(ok)");
}

static void run_active_bad_entry(lua_State *L, jit_State *J, TGState *tg,
                                 const char *command)
{
  GCtrace *T = expected_trace;
  int status, saved_errno;
  assert(T != NULL && trace_runnable_acq(T, trace_traceno_acq(T)));
  la_store32_rel(&active_pause_seen, 0);
  la_store32_rel(&active_release, 0);
  la_store32_rel(&active_mutated, 0);
  la_store32_rel(&active_pause_armed, 1);
  native_finishes = first_native = second_native = 0;
  lj_trace_test_reset_exit_stats();
  lj_tab_test_set_rooted_gc_read_hook(observe_active_table_read);
  lj_ffi_native_trace_test_set_finish_hook(observe_finish);
  start_active_replacement(L, command);
  errno = 0;
  status = call_runner_error(L, "call");
  saved_errno = errno;
  finish_active_replacement(L);
  lj_tab_test_set_rooted_gc_read_hook(NULL);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_ERRRUN && saved_errno == EDOM);
  assert(la_load32_acq(&active_pause_seen) == 1);
  assert(la_load32_acq(&active_mutated) == 1);
  assert(native_finishes == 1u && second_native == 1u && first_native == 0);
  assert(lj_trace_test_exit_calls() != 0);
  T = find_call_trace(J, global_proto(L, "__table_call_s"));
  assert(T != NULL && T == expected_trace);
  assert_frame_clean(tg, T);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  GCtrace *T;
  lua_Integer before_first, before_second;
  int saved_errno, status;

  expected_tg = tg;
  expected_L = L;
  expected_callback_slot = ccallback_slot_acq(&tg->cb);
  expected_callback_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  expected_ffi_func = lj_tg_ffi_call_func_acq(tg);
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__table_set_errno");
  lua_pushcfunction(L, active_pause_seen_fn);
  lua_setglobal(L, "__table_active_pause_seen");
  lua_pushcfunction(L, active_mark_mutated_fn);
  lua_setglobal(L, "__table_active_mark_mutated");
  lua_pushcfunction(L, active_release_fn);
  lua_setglobal(L, "__table_active_release");
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "local threading=require('threading')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_table_configure(void);\n"
    "int32_t lj_callxs_table_first_count(void);\n"
    "int32_t lj_callxs_table_second_count(void);\n"
    "int32_t lj_callxs_table_first(int32_t);\n"
    "int32_t lj_callxs_table_second(int32_t);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_TABLE_SO')))\n"
    "local calltype=ffi.typeof('int32_t(*)(int32_t)')\n"
    "local first=ffi.cast(calltype,lib.lj_callxs_table_first)\n"
    "local second=ffi.cast(calltype,lib.lj_callxs_table_second)\n"
    "_G.__table_library=lib\n"
    "_G.__table_first,_G.__table_second=first,second\n"
    "_G.__table_first_count=lib.lj_callxs_table_first_count\n"
    "_G.__table_second_count=lib.lj_callxs_table_second_count\n"
    "lib.lj_callxs_table_configure()\n"
    "_G.__table_registry={call=first}\n"
    "_G.__table_ready=threading.channel(1); _G.__table_go=threading.channel(1)\n"
    "_G.__table_done=threading.channel(1); _G.__table_release=threading.channel(1)\n"
    "_G.__table_peer=threading.spawn(function(reg, a, b, ready, go, done, release)\n"
    "  assert(ready:send(true,5))\n"
    "  while true do\n"
    "    local cmd, valid=go:recv(30); assert(valid)\n"
    "    if cmd=='second' then reg.call=b; collectgarbage('collect')\n"
    "    elseif cmd=='first' then reg.call=a\n"
    "    elseif cmd=='active_number' or cmd=='active_missing' then\n"
    "      local seen=false\n"
    "      for i=1,30000 do\n"
    "        if __table_active_pause_seen() then seen=true; break end\n"
    "        threading.sleep(0.001)\n"
    "      end\n"
    "      assert(seen,'active table-call pause timeout')\n"
    "      if cmd=='active_number' then reg.call=17 else reg.call=nil end\n"
    "      __table_active_mark_mutated(); __table_active_release()\n"
    "    elseif cmd=='stop' then assert(done:send(true,5)); break end\n"
    "    assert(done:send(true,5))\n"
    "  end\n"
    "  local _,rok=release:recv(30); assert(rok); return true\n"
    "end,__table_registry,first,second,__table_ready,__table_go,__table_done,__table_release)\n"
    "local _,ready_ok=__table_ready:recv(5); assert(ready_ok)\n"
    "function _G.__table_call_s(reg,n)\n"
    "  local sum=0.5\n"
    "  for i=1,n do sum=sum+reg.call(i) end\n"
    "  return sum-0.5\n"
    "end\n"
    "local set_errno=__table_set_errno\n"
    "function _G.__table_driver(run,n) set_errno(); local result=run(__table_registry,n); return result end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=32')\n");

  run_lua(L, "if os.getenv('LJ_ARM64_TABLE_CALL_DUMP') then "
             "require('jit.dump').on('isaT') end");

  capture_cfunc(L, "__table_first", &first_func);
  capture_cfunc(L, "__table_second", &second_func);
  assert(proto_has_tgets(L, "__table_call_s"));
  assert(mt_active_acq(g) != 0);

  T = run_success(L, J, tg, 1, 32);
  assert(counter(L, "__table_first_count") == 32);
  assert(counter(L, "__table_second_count") == 0);
  assert_frame_clean(tg, T);

  /* The peer replaces the shared callable and collects while the main TG
  ** still owns the compiled reader. The next pass must call the new identity. */
  replace_from_peer(L, "second");
  run_lua(L, "collectgarbage('collect')");
  T = run_success(L, J, tg, 2, 32);
  assert(counter(L, "__table_first_count") == 32);
  assert(counter(L, "__table_second_count") == 32);
  assert_frame_clean(tg, T);

  /* Mutate only after the second iteration entered the trace and rooted its
  ** old callable. The completed call is allowed; the next lookup must leave
  ** native code before another C effect, then raise in the interpreter. */
  before_first = counter(L, "__table_first_count");
  before_second = counter(L, "__table_second_count");
  run_active_bad_entry(L, J, tg, "active_number");
  assert(counter(L, "__table_first_count") == before_first);
  assert(counter(L, "__table_second_count") == before_second + 2);

  replace_from_peer(L, "second");
  before_second = counter(L, "__table_second_count");
  run_active_bad_entry(L, J, tg, "active_missing");
  assert(counter(L, "__table_first_count") == before_first);
  assert(counter(L, "__table_second_count") == before_second + 2);

  /* The second identity also survives a post-call exit without replay. */
  replace_from_peer(L, "second");
  before_second = counter(L, "__table_second_count");
  T = find_call_trace(J, global_proto(L, "__table_call_s"));
  assert(T != NULL);
  expected_trace = NULL;
  native_finishes = first_native = second_native = 0;
  forced_finishes = 0;
  old_epoch = UINT64_MAX;
  force_func = second_func;
  lj_trace_test_reset_exit_stats();
  lj_ffi_native_trace_test_set_finish_hook(force_postcall_epoch);
  errno = 0;
  status = call_runner(L, 24);
  saved_errno = errno;
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(status == LUA_OK && lua_tonumber(L, -1) ==
         3.0 * 24.0 * 25.0 / 2.0 + 101.0 * 24.0);
  lua_pop(L, 1);
  assert(saved_errno == EDOM && forced_finishes == 1u);
  assert(old_epoch != UINT64_MAX);
  lj_tg_hs_epoch_ack_rel(tg, old_epoch);
  assert(lj_trace_test_exit_calls() >= forced_finishes);
  {
    TraceNo parent = lj_trace_test_first_exit_parent();
    ExitNo exitno = lj_trace_test_first_exitno();
    GCtrace *exited = traceref_safe(J, parent);
    const SnapShot *snap;
    GCproto *pt = global_proto(L, "__table_call_s");
    MSize ofs, nent;
    uint64_t pcbase;
    const BCIns *pc;
    assert(exited == expected_trace && exitno < trace_nsnap_acq(exited));
    snap = &trace_snap_acq(exited)[exitno];
    ofs = snap_mapofs_acq(snap);
    nent = snap_nent_acq(snap);
    assert(ofs <= trace_nsnapmap_acq(exited) &&
           nent+1u+LJ_FR2 <= trace_nsnapmap_acq(exited)-ofs);
    memcpy(&pcbase, &trace_snapmap_acq(exited)[ofs+nent], sizeof(pcbase));
    pc = (const BCIns *)(uintptr_t)(pcbase >> 8);
    assert((uint8_t)pcbase == 0 && pc > proto_bc(pt) &&
           pc < proto_bc(pt)+pt->sizebc &&
           bc_op((BCIns)la_load32_acq((const uint32_t *)(pc-1))) == BC_CALL);
  }
  assert(counter(L, "__table_first_count") == before_first);
  assert(counter(L, "__table_second_count") == before_second + 24);
  T = find_call_trace(J, global_proto(L, "__table_call_s"));
  assert(T != NULL && native_finishes != 0 && second_native != 0);
  assert_frame_clean(tg, T);

  replace_from_peer(L, "stop");
  run_lua(L, "assert(__table_release:send(true,5)); "
             "local ok,result=__table_peer:join(5); assert(ok and result)");
  expected_tg = NULL;
  expected_L = NULL;
  expected_trace = NULL;
  lua_close(L);
  puts("t-arm64-jit-callxs-table OK: shared TGETS, identity replacement, "
       "active invalidation exits, POSTCALL no-replay, errno and cleanup");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-table SKIP: requires ARM64 experimental CALLXS");
  return 0;
}

#endif
