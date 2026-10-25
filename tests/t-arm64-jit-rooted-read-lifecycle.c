/* Native ARM64 lifecycle coverage for the bounded rooted scalar table read. */

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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_TAB_TEST_HELPERS)

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "rooted-read lifecycle fixture requires the admitted ARM64 root gates"
#endif

static uint32_t scalar_read_calls;
static uint32_t leased_scalar_read_calls;
static uint32_t force_result_kind;
static uint32_t forced_result_hits;
static uint32_t force_root_retry;
static uint32_t forced_root_retry_hits;
static TValue retry_replacement;
static TValue gc_replacement;
static GCtab *read_table;

static void count_scalar_read(lua_State *L, GCtab *t, uint32_t stage)
{
  if (stage == LJ_TAB_SCALAR_TEST_SOURCE) {
    (void)la_add32_rlx(&scalar_read_calls, 1);
    /* jit_base is the immutable trace lease held across every native helper
    ** call; unlike in_native it remains published at the C-call boundary. */
    if (lj_tg_load_jit_base(L2TG(L)) != NULL)
      (void)la_add32_rlx(&leased_scalar_read_calls, 1);
  }
  if (stage == LJ_TAB_SCALAR_TEST_VALUE && t == read_table) {
    uint32_t kind = la_load32_acq(&force_result_kind);
    if (kind != 0 && la_cas32(&force_result_kind, &kind, 0,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      TValue value;
      TValue old;
      TValue *array = lj_tab_array_acq(t);
      assert(array != NULL && lj_tab_asize_acq(t) > 1);
      lj_tv_load_acq(&old, &array[1]);
      assert(tvisnumber(&old) && numV(&old) == 2.5);
      if (kind == 1)
        setboolV(&value, 1);
      else if (kind == 2)
        setnilV(&value);
      else {
        assert(kind == 3 && tvistab(&gc_replacement));
        value = gc_replacement;
      }
      tv_rawstore_rel(&array[1], tv_rawload(&value));
      (void)la_add32_rlx(&forced_result_hits, 1);
    }
  }
  if (stage == LJ_TAB_SCALAR_TEST_RESULT) {
    uint32_t expected = 1;
    if (la_cas32(&force_root_retry, &expected, 0,
                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      /* Force the exact source-root confirmation to refuse this attempt. */
      copyTVrel(L, &L2TG(L)->tmptv, &retry_replacement);
      (void)la_add32_rlx(&forced_root_retry_hits, 1);
    }
  }
}

typedef struct CleanState {
  uint32_t readers;
  uint32_t anchors;
  uint32_t tab_read_depth;
  uint64_t tab_read_epoch;
} CleanState;

typedef struct WaitState {
  uint32_t no_l;
  uint32_t l;
  uint32_t store_l;
} WaitState;

static WaitState wait_state(void)
{
  WaitState s;
  s.no_l = lj_tab_test_wait_no_l_calls();
  s.l = lj_tab_test_wait_l_calls();
  s.store_l = lj_tab_test_store_wait_l_calls();
  return s;
}

static void assert_wait_state(WaitState s)
{
  assert(lj_tab_test_wait_no_l_calls() == s.no_l);
  assert(lj_tab_test_wait_l_calls() == s.l);
  assert(lj_tab_test_store_wait_l_calls() == s.store_l);
}

static CleanState clean_state(lua_State *L)
{
  TGState *tg = L2TG(L);
  CleanState s;
  s.readers = gc2_smr_readers_acq(G(L));
  s.anchors = lj_tg_root_anchor_top_acq(tg);
  s.tab_read_depth = lj_tg_tab_read_depth_acq(tg);
  s.tab_read_epoch = lj_tg_tab_read_epoch_acq(tg);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return s;
}

static void assert_clean(lua_State *L, CleanState s)
{
  TGState *tg = L2TG(L);
  assert(gc2_smr_readers_acq(G(L)) == s.readers);
  assert(lj_tg_root_anchor_top_acq(tg) == s.anchors);
  assert(lj_tg_tab_read_depth_acq(tg) == s.tab_read_depth);
  assert(lj_tg_tab_read_epoch_acq(tg) == s.tab_read_epoch);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "rooted-read lifecycle chunk failed: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return funcproto(fn);
}

static lua_Number call_read(lua_State *L, lua_Integer n)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, "__arm64_rooted_read");
  lua_getglobal(L, "__arm64_rooted_table");
  lua_pushinteger(L, n);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "rooted-read call failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

static int call_read_status(lua_State *L, lua_Integer n, const char *error_text)
{
  void *saved_cframe = L->cframe;
  int status;
  lua_getglobal(L, "__arm64_rooted_read");
  lua_getglobal(L, "__arm64_rooted_table");
  lua_pushinteger(L, n);
  status = lua_pcall(L, 2, 1, 0);
  if (status == LUA_OK) {
    lua_pop(L, 1);
  } else {
    const char *msg = lua_tostring(L, -1);
    if (error_text)
      assert(msg && strstr(msg, error_text) != NULL);
    lua_pop(L, 1);
  }
  assert(L->cframe == saved_cframe);
  return status;
}

typedef struct PostAdmissionStop {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  uint32_t saw_pause;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionStop;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void *publish_stopreq_after_entry(void *arg)
{
  PostAdmissionStop *p = (PostAdmissionStop *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
        LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&p->saw_pause, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&p->saw_pause) == 1);
  assert(gc2_hs_epoch_acq(p->g) == p->epoch);
  assert(lj_tg_hs_epoch_ack_acq(p->tg) == p->epoch);
  assert(gc2_hs_pending_acq(p->g) == 0);
  assert(lj_tg_reqmask_acq(p->tg) == 0);
  assert(lj_tg_poll_acq(p->tg) == 0);
  assert(lj_tg_load_jit_base(p->tg) != NULL);
  la_store32_rel(&p->saw_jit_base, 1);

  /* One-TG counted STOPREQ publication; XPOLL is the only native observer. */
  gc2_hs_actions_rel(p->g, LJ_GC2_HS_STOPREQ);
  gc2_hs_pending_rel(p->g, 1);
  gc2_hs_epoch_rel(p->g, p->epoch + 1u);
  lj_tg_reqmask_rel(p->tg, LJ_GC2_HS_STOPREQ);
  lj_tg_poll_rel(p->tg, 1);
  la_store32_rel(&p->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static SnapNo loop_snapshot(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  IRRef r;
  for (r = REF_FIRST; r < trace_nins_acq(T); r++)
    if (ir[r].o == IR_LOOP) {
      SnapNo s;
      for (s = 0; s < trace_nsnap_acq(T); s++)
        if (snap_ref_acq(&snap[s]) == r)
          return s;
      break;
    }
  assert(!"rooted-read trace lacks a loop snapshot");
  return 0;
}

static unsigned value_snapshots(const GCtrace *T, SnapNo out[2])
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  unsigned found = 0;
  IRRef r;
  for (r = REF_FIRST; r < trace_nins_acq(T); r++)
    if (ir[r].o == IR_VLOAD && irt_type(ir[r].t) == IRT_NUM) {
      SnapNo s;
      for (s = 0; s < trace_nsnap_acq(T); s++)
        if (snap_ref_acq(&snap[s]) == r) {
          assert(found < 2);
          out[found++] = s;
        }
    }
  assert(found == 2);
  return found;
}

static unsigned status_snapshots(const GCtrace *T, SnapNo out[2])
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  IRRef n = trace_nins_acq(T);
  unsigned found = 0;
  IRRef r;
  for (r = REF_FIRST; r < n; r++)
    if (ir[r].o == IR_NE && irt_isguard(ir[r].t) &&
        ((ir[r].op1 >= REF_FIRST && ir[r].op1 < r && ir[ir[r].op1].o == IR_CALLS &&
          ir[ir[r].op1].op2 == IRCALL_lj_tab_getscalar_rooted_try) ||
         (ir[r].op2 >= REF_FIRST && ir[r].op2 < r && ir[ir[r].op2].o == IR_CALLS &&
          ir[ir[r].op2].op2 == IRCALL_lj_tab_getscalar_rooted_try))) {
      IRRef call = ir[r].op1 >= REF_FIRST && ir[r].op1 < r &&
                   ir[ir[r].op1].o == IR_CALLS ?
                   ir[r].op1 : ir[r].op2;
      SnapNo s, active = (SnapNo)~0u;
      assert(r == call + 1u);
      for (s = 0; s < trace_nsnap_acq(T); s++)
        if (snap_ref_acq(&snap[s]) <= r &&
            (active == (SnapNo)~0u ||
             snap_ref_acq(&snap[s]) > snap_ref_acq(&snap[active])))
          active = s;
      assert(active != (SnapNo)~0u);
      assert(found < 2);
      out[found++] = active;
    }
  assert(found == 2);
  return found;
}

static void assert_native_result_refusal(lua_State *L, GCtrace *T,
					 uint32_t kind, CleanState clean)
{
  SnapNo status_snap[2], value_snap[2];
  uint32_t before = la_load32_acq(&scalar_read_calls);
  uint32_t leased_before = la_load32_acq(&leased_scalar_read_calls);
  la_store32_rel(&forced_result_hits, 0);
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  la_store32_rel(&force_result_kind, kind);
  assert(call_read_status(L, 64, "arithmetic") == LUA_ERRRUN);
  assert(la_load32_acq(&force_result_kind) == 0);
  assert(la_load32_acq(&forced_result_hits) == 1);
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert(la_load32_acq(&leased_scalar_read_calls) == leased_before + 1u);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == T->traceno &&
	 lj_trace_test_last_exit_parent() == T->traceno);
  assert(status_snapshots(T, status_snap) == 2);
  assert(value_snapshots(T, value_snap) == 2);
  assert(status_snap[0] != value_snap[0] && status_snap[0] != value_snap[1]);
  assert(status_snap[1] != value_snap[0] && status_snap[1] != value_snap[1]);
  assert(lj_trace_test_last_exitno() == status_snap[0] ||
	 lj_trace_test_last_exitno() == status_snap[1]);
  assert_clean(L, clean);
}

static void test_lifecycle(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  global_State *g;
  GCproto *pt;
  GCtrace *T;
  CleanState clean;
  WaitState waits;
  uint32_t before, leased_before;
  uint64_t epoch;
  void *saved_cframe;
  int32_t idle_vmstate;
  pthread_t worker;
  PostAdmissionStop stop;
  int status;
  SnapNo value_snap[2];

  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "local th = require('threading')\n"
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2')\n"
    "__arm64_rooted_table = { 1.25 } -- before MT activation\n"
    "__arm64_rooted_alternate = { 99.0 }\n"
    "__arm64_rooted_ready = th.channel(1)\n"
    "__arm64_rooted_go = th.channel(1)\n"
    "__arm64_rooted_done = th.channel(1)\n"
    "__arm64_rooted_release = th.channel(1)\n"
    "__arm64_rooted_peer = th.spawn(function(t, ready, go, done, release)\n"
    "  assert(ready:send(true, 5) == true)\n"
    "  local _, ok = go:recv(30); assert(ok == true)\n"
    "  for i = 2, 1024 do\n"
    "    t[i] = i; t[1] = 0.5 + (i % 8) / 4\n"
    "    if i % 16 == 0 then collectgarbage('step'); th.sleep(0) end\n"
    "  end\n"
    "  assert(done:send(true, 5) == true)\n"
    "  local _, rok = release:recv(30); assert(rok == true)\n"
    "  return true\n"
    "end, __arm64_rooted_table, __arm64_rooted_ready, __arm64_rooted_go,\n"
    "      __arm64_rooted_done, __arm64_rooted_release)\n"
    "local _, ok = __arm64_rooted_ready:recv(5); assert(ok == true)\n"
    "function __arm64_rooted_read(t, n)\n"
    "  local i, sum = 1, 0\n"
    "  while i <= n do sum = sum + t[1]; i = i + 1 end\n"
    "  return sum\n"
    "end\n");

  J = L2J(L);
  tg = L2TG(L);
  g = G(L);
  idle_vmstate = lj_tg_vmstate_load_acq(tg);
  saved_cframe = L->cframe;
  lua_getglobal(L, "__arm64_rooted_table");
  assert(tvistab(L->top - 1));
  read_table = tabV(L->top - 1);
  lua_pop(L, 1);
  lua_getglobal(L, "__arm64_rooted_alternate");
  assert(tvistab(L->top - 1));
  gc_replacement = *(L->top - 1);
  lua_pop(L, 1);
  lj_tab_test_set_scalar_rooted_try_hook(count_scalar_read);
  clean = clean_state(L);

  waits = wait_state();
  assert(call_read(L, 64) == 80.0);
  assert_wait_state(waits);
  pt = global_proto(L, "__arm64_rooted_read");
  assert(proto_trace_acq(pt) != 0);
  T = traceref_safe(J, proto_trace_acq(pt));
  assert(trace_runnable_acq(T, proto_trace_acq(pt)));
  assert(bc_op(trace_startins_acq(T)) == BC_LOOP);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) > sizeof(MCode));
  assert(la_load32_acq(&scalar_read_calls) > 0);
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_exit_calls() > 0);
  assert_clean(L, clean);

  /* Full collection while both function and table remain live must preserve
  ** the trace inputs, and the next run must really re-enter native helper code. */
  before = la_load32_acq(&scalar_read_calls);
  run_lua(L, "collectgarbage('collect')");
  waits = wait_state();
  assert(call_read(L, 64) == 80.0);
  assert_wait_state(waits);
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert_clean(L, clean);

  /* Keep a live peer writing the indexed value and growing the array vector
  ** while the native loop reads it. Retry exits are allowed; values stay scalar. */
  run_lua(L, "assert(__arm64_rooted_go:send(true, 5) == true)");
  before = la_load32_acq(&scalar_read_calls);
  {
    lua_Number sum = call_read(L, 100000);
    assert(sum >= 50000.0 && sum <= 225000.0);
  }
  assert(la_load32_acq(&scalar_read_calls) > before);
  run_lua(L,
    "local _, ok = __arm64_rooted_done:recv(30); assert(ok == true)\n"
    "assert(__arm64_rooted_table[1024] == 1024)\n");
  assert_clean(L, clean);

  /* Deterministically race the helper's final table-root confirmation. It
  ** must refuse, and the interpreter retries this read from its real local. */
  run_lua(L, "__arm64_rooted_table[1] = 2.5");
  lua_getglobal(L, "__arm64_rooted_alternate");
  assert(tvistab(L->top - 1));
  retry_replacement = *(L->top - 1);
  lua_pop(L, 1);
  la_store32_rel(&forced_root_retry_hits, 0);
  la_store32_rel(&force_root_retry, 1);
  before = la_load32_acq(&scalar_read_calls);
  assert(call_read(L, 64) == 64.0 * 2.5);
  assert(la_load32_acq(&forced_root_retry_hits) == 1u);
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert_clean(L, clean);

  /* The resize peer is finished before the synthetic single-TG STOPREQ. */
  run_lua(L,
    "assert(__arm64_rooted_release:send(true, 5) == true)\n"
    "local ok, result = __arm64_rooted_peer:join(5)\n"
    "assert(ok == true and result == true)\n");

  /* A boolean result is a successful scalar helper hit but fails the guarded
  ** NUM VLOAD. Change it atomically at the value hook, after native entry. */
  run_lua(L, "__arm64_rooted_table[1] = 2.5");
  before = la_load32_acq(&scalar_read_calls);
  leased_before = la_load32_acq(&leased_scalar_read_calls);
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  la_store32_rel(&force_result_kind, 1);
  status = call_read_status(L, 64, "arithmetic");
  assert(value_snapshots(T, value_snap) == 2);
  assert(status == LUA_ERRRUN);
  assert(la_load32_acq(&force_result_kind) == 0);
  /* Exactly one native entry exits at this trace's guarded numeric VLOAD;
  ** interpreter replay after that side exit invokes no rooted helper. */
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert(la_load32_acq(&leased_scalar_read_calls) == leased_before + 1u);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == T->traceno &&
         lj_trace_test_last_exit_parent() == T->traceno);
  assert(lj_trace_test_last_exitno() == value_snap[0] ||
         lj_trace_test_last_exitno() == value_snap[1]);
  assert_clean(L, clean);

  /* A nil slot and a pre-rooted GC value both refuse scalar extraction. The
  ** status guard must leave before the numeric VLOAD touches the output root. */
  run_lua(L, "__arm64_rooted_table[1] = 2.5");
  assert_native_result_refusal(L, T, 2, clean);
  run_lua(L, "__arm64_rooted_table[1] = 2.5");
  assert_native_result_refusal(L, T, 3, clean);

  /* Restore a numeric input, then interrupt only after ARM64 native admission.
  ** The helper must finish its bounded read, release all root/lifetime scopes,
  ** and the next XPOLL must leave the trace cleanly for interpreter shutdown. */
  run_lua(L, "__arm64_rooted_table[1] = 2.5");
  assert(proto_trace_acq(pt) != 0);
  T = traceref_safe(J, proto_trace_acq(pt));
  assert(trace_runnable_acq(T, proto_trace_acq(pt)));
  clear_stopreq(tg);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  stop = (PostAdmissionStop){ g, tg, epoch, 0, 0, 0 };
  assert(pthread_create(&worker, NULL, publish_stopreq_after_entry, &stop) == 0);
  before = la_load32_acq(&scalar_read_calls);
  status = call_read_status(L, 100000,
                            "thread interrupted: VM shutdown");
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(la_load32_acq(&stop.saw_pause) == 1 &&
         la_load32_acq(&stop.saw_jit_base) == 1 &&
         la_load32_acq(&stop.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1 &&
         lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == loop_snapshot(T));
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u);
  assert(gc2_hs_leader_acq(g) == 0 && gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0 && lj_tg_poll_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert(lj_tg_load_jit_base(tg) == NULL && lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate && L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert_clean(L, clean);

  /* After clearing STOPREQ, the same published trace must re-enter and run. */
  before = la_load32_acq(&scalar_read_calls);
  assert(call_read(L, 64) == 160.0);
  assert(la_load32_acq(&scalar_read_calls) > before);
  assert(lj_tg_load_jit_base(tg) == NULL && lj_tg_in_native_acq(tg) == 0);
  assert(L->cframe == saved_cframe);
  assert_clean(L, clean);

  lj_tab_test_set_scalar_rooted_try_hook(NULL);
  lua_close(L);
}

int main(void)
{
  test_lifecycle();
  puts("t-arm64-jit-rooted-read-lifecycle OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-rooted-read-lifecycle SKIP");
  return 0;
}

#endif
