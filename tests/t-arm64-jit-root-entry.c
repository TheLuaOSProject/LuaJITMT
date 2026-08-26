/*
** Direct C contract for the fail-closed ARM64 root-entry helper.
** No valid trace or native target is constructed or executed here.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

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
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || !LJ_ARM64_JIT_FAIL_CLOSED
#error "t-arm64-jit-root-entry requires fail-closed experimental ARM64 JIT"
#endif

typedef enum RootEntryRaceMode {
  ROOT_ENTRY_CLOSER_BEFORE_PUBLISH,
  ROOT_ENTRY_CLOSER_AFTER_PUBLISH
} RootEntryRaceMode;

typedef struct RootEntryRace {
  global_State *g;
  RootEntryRaceMode mode;
  uint32_t entry_done;
  uint32_t worker_done;
  uint32_t saw_active;
  int entered;
} RootEntryRace;

/* Kept out of line for the contract script: its call site proves that Clang's
** Darwin AAPCS64 lowering consumes LJTraceRootEntry.trace from x0 and target
** from x1, with no hidden result pointer. It is deliberately never executed. */
__attribute__((noinline, used))
GCtrace *lj_test_root_entry_abi_probe(jit_State *J, const BCIns *pc,
	TraceNo traceno, lua_State *L, TValue *base, uint32_t sourceop,
	ASMFunction *targetp)
{
  LJTraceRootEntry entry =
    lj_trace_enter_root(J, pc, traceno, L, base, sourceop);
  *targetp = entry.target;
  return entry.trace;
}

static void expect_reject(LJTraceRootEntry entry)
{
  assert(entry.trace == NULL);
  assert(entry.target == NULL);
}

static void wait_for_pause(uint32_t stage, const RootEntryRace *race)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() == stage)
      return;
    assert(la_load32_acq(&race->worker_done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"root-entry helper did not reach requested pause");
}

static void *root_entry_closer(void *arg)
{
  RootEntryRace *race = (RootEntryRace *)arg;
  uint32_t stage = race->mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH ?
    LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH :
    LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH;
  wait_for_pause(stage, race);
  if (race->mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    uint32_t i;
    for (i = 0; i < 10000000u; i++) {
      if (lj_gc2_test_idle_reclaim_enter(race->g)) {
        race->entered = 1;
        break;
      }
      (void)lj_thr_retry_yield(NULL);
    }
    assert(race->entered == 1);
    assert(gc2_jit_phase_gate_acq(race->g) == 0);
    lj_trace_test_root_entry_release();
    while (la_load32_acq(&race->entry_done) == 0)
      la_cpu_pause();
    lj_gc2_test_idle_reclaim_leave(race->g);
  } else {
    race->saw_active = (uint32_t)lj_tg_any_jit_active(race->g);
    race->entered = lj_gc2_test_idle_reclaim_enter(race->g);
    if (race->entered)
      lj_gc2_test_idle_reclaim_leave(race->g);
    lj_trace_test_root_entry_release();
  }
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void require_idle_reclaim_preflight(global_State *g)
{
  if (!lj_gc2_test_idle_reclaim_enter(g)) {
    fprintf(stderr, "root-entry idle preflight failed: phase=%u gate=%u "
      "active=%d smr=%u\n", gc2_phase_acq(g),
      gc2_jit_phase_gate_acq(g), lj_tg_any_jit_active(g),
      gc2_smr_reclaiming_acq(g));
    assert(0);
  }
  lj_gc2_test_idle_reclaim_leave(g);
}

static void run_pause_race(lua_State *L, BCIns *pc, RootEntryRaceMode mode)
{
  global_State *g = G(L);
  TGState *tg = L->tg_hint;
  RootEntryRace race = { g, mode, 0, 0, 0, 0 };
  pthread_t closer;
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  uint32_t stage = mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH ?
    LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH :
    LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH;
  LJTraceRootEntry entry;

  gc2_jit_sweep_displaced_rel(g, 0);
  lj_trace_test_root_entry_pause(stage);
  assert(pthread_create(&closer, NULL, root_entry_closer, &race) == 0);
  entry = lj_trace_enter_root(L2J(L), pc, 1, L, L->base, BC_JLOOP);
  expect_reject(entry);
  assert(lj_tg_load_jit_base(tg) == NULL);
  la_store32_rel(&race.entry_done, 1);
  assert(pthread_join(closer, NULL) == 0);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
  if (mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    assert(race.entered == 1);
    assert(gc2_jit_sweep_displaced_acq(g) == 0); /* Leave consumed it. */
  } else {
    assert(race.saw_active == 1);
    assert(race.entered == 0);
  }
  assert(gc2_jit_phase_gate_acq(g) != 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  jit_State *J;
  BCIns jloop = BCINS_AD(BC_JLOOP, 0, 1);
  BCIns jfuncf = BCINS_AD(BC_JFUNCF, 0, 1);
  uint32_t publishes, cleanups;
  int32_t vmstate;
  lua_State *tmpbuf_L;

  assert(L != NULL);
  g = G(L);
  tg = L->tg_hint;
  J = L2J(L);
  assert(g != NULL && tg != NULL && J != NULL);
  assert(tg == lj_thr_get_tg() && tg->gl == g);
  assert(lj_tg_load_cur_L(tg) == L && lj_tg_owns_state_acq(tg, L));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(tracevec_acq(J) == NULL); /* Recorder is still fail-closed. */
  tmpbuf_L = sbufL(&tg->tmpbuf);
  lua_gc(L, LUA_GCSTOP, 0);
  require_idle_reclaim_preflight(g);
  lj_trace_test_root_entry_reset();

  /* Invalid calls reject before publication and never clear a foreign lease. */
  expect_reject(lj_trace_enter_root(NULL, &jloop, 1, L, L->base,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(
    (jit_State *)((char *)J + sizeof(void *)), &jloop, 1, L, L->base,
    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, NULL, 1, L, L->base, BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, &jloop, 0, L, L->base, BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, &jloop, 1, NULL, L->base,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, NULL, BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JFUNCV));
  lj_tg_store_jit_base(tg, L->base);
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == L->base);
  lj_tg_store_jit_base(tg, NULL);
  vmstate = lj_tg_vmstate_load_acq(tg);
  lj_tg_vmstate_store_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JLOOP));
  lj_tg_vmstate_store_rel(tg, vmstate);
  lj_tg_in_native_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JLOOP));
  lj_tg_in_native_rel(tg, 0);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* An open gate admits intent, then absent metadata rejects through the one
  ** release cleanup path for both root source families. */
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, &jfuncf, 1, L, L->base, BC_JFUNCF));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 2);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* A gate owner wins before publication: entry records displacement and does
  ** not claim a TG lifetime lease. */
  require_idle_reclaim_preflight(g);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  publishes = lj_trace_test_root_entry_publishes();
  cleanups = lj_trace_test_root_entry_cleanups();
  gc2_jit_sweep_displaced_rel(g, 0);
  expect_reject(lj_trace_enter_root(J, &jloop, 1, L, L->base, BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(gc2_jit_sweep_displaced_acq(g) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);
  lj_gc2_test_idle_reclaim_leave(g);

  require_idle_reclaim_preflight(g);
  run_pause_race(L, &jloop, ROOT_ENTRY_CLOSER_BEFORE_PUBLISH);
  require_idle_reclaim_preflight(g);
  run_pause_race(L, &jloop, ROOT_ENTRY_CLOSER_AFTER_PUBLISH);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  lj_trace_test_root_entry_reset();
  lua_close(L);
  puts("arm64_jit_root_entry OK: rejection and two-sided gate races verified");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_root_entry SKIP: requires native experimental macOS ARM64");
  return 0;
}

#endif
