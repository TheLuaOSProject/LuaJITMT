/*
** Native macOS ARM64 entry contract for admitted integer BC_FORL roots.
** Entry is permitted only from a taken integer JFORL edge, after the VM has
** incremented, tested and stored both IDX and EXT. The shared FP JFORL path
** must keep using branch-only stale-startins recovery.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-native-forl requires native integer JFORL entry"
#endif

typedef struct FullWordRace {
  BCIns *pc;
  BCIns replacement;
  uint32_t saw_pause;
  uint32_t worker_done;
} FullWordRace;

typedef struct ProfileRejectRace {
  TGState *tg;
  uint32_t saw_pause;
  uint32_t worker_done;
} ProfileRejectRace;

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 native-FORL chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static lua_Number call_forl_integer(lua_State *L, lua_Integer n,
	lua_Integer seed)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_native_forl");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, seed);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 native-FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Number call_forl_number_stop(lua_State *L, lua_Number n,
	lua_Integer seed)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_native_forl");
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, n);
  lua_pushinteger(L, seed);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric native-FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static lua_Integer call_edge(lua_State *L, const char *name, lua_Integer first,
	lua_Integer stop)
{
  void *saved_cframe = L->cframe;
  lua_Integer result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, first);
  lua_pushinteger(L, stop);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 JFORL edge call %s failed: %s\n", name,
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static GCtrace *expect_forl_trace(lua_State *L, GCproto *pt)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *bc = proto_bc(pt);
  const BCIns *pc;
  BCIns startins, live, fori;
  int64_t pos, bodypos, foripos, exitpos;
  TraceNo traceno;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_FORL_ADMITTED) != 0);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) == 0);
  assert((la_load8_acq(&T->unused1) & TRACE_ENTRY_GATED) == 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0);
  assert(trace_mcloop_acq(T) < trace_szmcode_acq(T));
  assert((trace_mcloop_acq(T) & (sizeof(MCode)-1u)) == 0);

  pc = trace_startpc_acq(T);
  startins = trace_startins_acq(T);
  assert(pc >= bc && pc < bc+pt->sizebc);
  assert(bc_op(startins) == BC_FORL);
  assert(bc_j(startins) < 0);
  assert((MSize)bc_a(startins)+FORL_EXT < (MSize)pt->framesize);
  pos = (int64_t)proto_bcpos(pt, pc);
  bodypos = pos+1+(int64_t)bc_j(startins);
  foripos = bodypos-1;
  assert(bodypos > 0 && bodypos <= pos);
  assert(foripos >= 0 && foripos < (int64_t)pt->sizebc);
  fori = (BCIns)la_load32_acq((const uint32_t *)&bc[(BCPos)foripos]);
  assert(bc_op(fori) == BC_FORI);
  assert(bc_a(fori) == bc_a(startins));
  assert(bc_j(fori) > 0);
  exitpos = foripos+1+(int64_t)bc_j(fori);
  assert(exitpos == pos+1);

  live = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(live == BCINS_AD(BC_JFORL, bc_a(startins), 1));
  assert(proto_jit_startins_acq(pt, pc) == startins);
  assert(proto_trace_acq(pt) == 1);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
  return T;
}

static void expect_no_native_activity(lua_State *L)
{
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
}

static void expect_one_native_loop(lua_State *L)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 5);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == 5);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
}

static void wait_for_postmetadata(uint32_t *saw_pause)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA) {
      la_store32_rel(saw_pause, 1);
      return;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 JFORL helper did not reach post-metadata pause");
}

static void *mutate_full_word(void *arg)
{
  FullWordRace *race = (FullWordRace *)arg;
  wait_for_postmetadata(&race->saw_pause);
  bc_publish(race->pc, race->replacement);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void *publish_profile_rejection(void *arg)
{
  ProfileRejectRace *race = (ProfileRejectRace *)arg;
  wait_for_postmetadata(&race->saw_pause);
  assert(lj_tg_profile_request_acq(race->tg) == 0);
  lj_tg_profile_request_rel(race->tg, 1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void test_vm_branch_only_rejection(lua_State *L)
{
  TGState *tg = L2TG(L);
  ProfileRejectRace race;
  pthread_t worker;

  memset(&race, 0, sizeof(race));
  race.tg = tg;
  assert(lj_tg_profile_request_acq(tg) == 0);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA);
  assert(pthread_create(&worker, NULL, publish_profile_rejection, &race) == 0);
  assert(call_forl_integer(L, 2, 0) == 3);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&race.saw_pause) == 1);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  assert(lj_trace_test_root_entry_startins_calls() == 1);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

static void test_full_word_recheck(lua_State *L, GCfunc *fn, GCtrace *T)
{
  jit_State *J = L2J(L);
  TGState *tg = L2TG(L);
  BCIns *pc = (BCIns *)trace_startpc_acq(T);
  BCIns startins = trace_startins_acq(T);
  BCIns live = (BCIns)la_load32_acq((const uint32_t *)pc);
  uint8_t wronga = (uint8_t)(bc_a(startins)+1u);
  TValue saved_func;
  int32_t saved_vmstate;
  FullWordRace race;
  LJTraceRootEntry entry;
  pthread_t worker;

  assert(wronga != bc_a(startins));
  assert(live == BCINS_AD(BC_JFORL, bc_a(startins), 1));
  memset(&race, 0, sizeof(race));
  race.pc = pc;
  race.replacement = BCINS_AD(BC_JFORL, wronga, 1);

  copyTV(L, &saved_func, L->base-2);
  setfuncV(L, L->base-2, fn);
  assert(curr_func(L) == fn);
  saved_vmstate = lj_tg_vmstate_load_acq(tg);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);

  /* First prove that this exact real trace/frame pair is admissible. */
  lj_trace_test_root_entry_reset();
  entry = lj_trace_enter_root(J, pc, 1, L, L->base, live);
  assert(entry.trace == T && entry.target != NULL);
  assert((uintptr_t)lj_ptr_strip(entry.target) ==
	 (uintptr_t)(void *)trace_mcode_acq(T));
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_tg_load_jit_base(tg) == L->base);
  lj_tg_store_jit_base(tg, NULL);

  /* Change only A after both metadata views. The final acquire must compare
  ** the complete JFORL A/D word, reject the target and clear its TG lease. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA);
  assert(pthread_create(&worker, NULL, mutate_full_word, &race) == 0);
  entry = lj_trace_enter_root(J, pc, 1, L, L->base, live);
  assert(entry.trace == NULL && entry.target == NULL);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&race.saw_pause) == 1);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert((BCIns)la_load32_acq((const uint32_t *)pc) == race.replacement);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  assert(lj_tg_load_jit_base(tg) == NULL);
  bc_publish(pc, live);

  lj_tg_vmstate_store_rel(tg, saved_vmstate);
  copyTV(L, L->base-2, &saved_func);
  assert((BCIns)la_load32_acq((const uint32_t *)pc) == live);
}

static void test_signed_step_edges(int negative)
{
  const char *name = negative ? "__arm64_native_forl_neg_edge" :
	"__arm64_native_forl_pos_edge";
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCfunc *fn;
  GCproto *pt;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  assert(J != NULL && L2TG(L) != NULL);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=2','hotexit=1','maxtrace=2'); "
    "function __arm64_native_forl_pos_edge(first, stop) "
      "local c=0 "
      "for i=first,stop,1 do c=c+1 end "
      "return c "
    "end "
    "function __arm64_native_forl_neg_edge(first, stop) "
      "local c=0 "
      "for i=first,stop,-1 do c=c+1 end "
      "return c "
    "end");
  fn = global_lfunc(L, name);
  pt = funcproto(fn);

  /* Record only with a stop that satisfies the signed overflow guard. */
  if (negative)
    assert(call_edge(L, name, 40, 1) == 40);
  else
    assert(call_edge(L, name, 1, 40) == 40);
  (void)expect_forl_trace(L, pt);

  /* Both signs share the post-update admission label. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  if (negative)
    assert(call_edge(L, name, 10, 1) == 10);
  else
    assert(call_edge(L, name, 1, 10) == 10);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);

  /* Signed overflow in the interpreter's JFORL increment terminates the loop
  ** before IDX/EXT publication and before the native-entry helper is called. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  if (negative)
    assert(call_edge(L, name, (lua_Integer)INT32_MIN,
		     (lua_Integer)INT32_MIN) == 1);
  else
    assert(call_edge(L, name, (lua_Integer)INT32_MAX,
		     (lua_Integer)INT32_MAX) == 1);
  expect_no_native_activity(L);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(trace_runnable_acq(traceref_safe(J, 1), 1));
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  GCfunc *fn;
  GCproto *pt;
  GCtrace *T;
  const BCIns *firstpc;
  BCIns firstins;
  void *saved_cframe;
  int32_t saved_vmstate;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  tg = L2TG(L);
  assert(J != NULL && tg != NULL);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=2','hotexit=1','maxtrace=2'); "
    "function __arm64_native_forl(n, seed) "
      "local s=seed "
      "for i=1,n do s=s+i end "
      "return s "
    "end");
  fn = global_lfunc(L, "__arm64_native_forl");
  pt = funcproto(fn);

  /* Record and independently prove the narrow FORL publication. */
  assert(call_forl_integer(L, 20, 0) == 210);
  T = expect_forl_trace(L, pt);
  firstpc = trace_startpc_acq(T);
  firstins = trace_startins_acq(T);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);

  /* A taken integer JFORL enters once, runs the native loop and exits at its
  ** terminal comparison snapshot. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_integer(L, 20, 0) == 210);
  expect_one_native_loop(L);

  /* A late request rejects the helper after both metadata views. The VM must
  ** restore the consumed JFORL/trace number and branch without incrementing a
  ** second time; n=2 therefore remains exactly 1+2. */
  test_vm_branch_only_rejection(L);

  /* The update happens, but its false edge must never invoke admission. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_integer(L, 1, 9) == 10);
  expect_no_native_activity(L);
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  /* The same patched JFORL executes its number path for a fractional stop.
  ** Both taken FP edges remain branch-only and cannot publish native intent. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_number_stop(L, 3.5, 0) == 6.0);
  expect_no_native_activity(L);
  assert(lj_trace_test_root_entry_startins_calls() == 2);

  /* i=1 leaves s at INT32_MAX. The native preheader addition overflows on i=2
  ** and exits; correct completion proves JFORL stored the incremented IDX and
  ** EXT before entering. A second entry rejects the number accumulator. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_integer(L, 3, 2147483646) == 2147483652.0);
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 1);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);

  test_full_word_recheck(L, fn, T);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);

  /* Flush must restore the exact FORL word and sidecar generation. Reusing
  ** trace slot 1 must publish an equivalent root that still enters natively. */
  run_lua(L, "jit.flush()");
  assert((BCIns)la_load32_acq((const uint32_t *)firstpc) == firstins);
  assert(proto_jit_startins_acq(pt, firstpc) == firstins);
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(J, 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
  assert(lj_tg_load_jit_base(tg) == NULL);

  assert(call_forl_integer(L, 20, 0) == 210);
  T = expect_forl_trace(L, pt);
  assert(trace_startpc_acq(T) == firstpc);
  assert(trace_startins_acq(T) == firstins);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_integer(L, 20, 0) == 210);
  expect_one_native_loop(L);

  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(lj_tg_load_jit_base(tg) == NULL);
  lua_close(L);

  test_signed_step_edges(0);
  test_signed_step_edges(1);
  puts("t-arm64-jit-native-forl OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-native-forl SKIP");
  return 0;
}

#endif
