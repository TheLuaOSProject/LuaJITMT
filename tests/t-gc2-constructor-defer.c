/* Real constructor ownership, explicit deferral, publication/cancellation.
 * Initialization follows existing t-gc-root-pending-race.c:242-261. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arena.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

static uint64_t now_ns(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static GCupval *new_constructing(lua_State *L, const TValue *value)
{
  GCupval *uv = (GCupval *)lj_mem_newgco_unlinked_nothrow(L, sizeof(GCupval));
  assert(uv != NULL);
  uv->gct = ~LJ_TUPVAL;
  uv->closed = 1;
  uv->immutable = 0;
  copyTV(L, &uv->tv, value);
  setmref(uv->v, &uv->tv);
  uv->dhash = 0;
  newwhite(G(L), uv);
  lj_obj_setgcwnullrel(obj2gco(uv));
  return uv;
}

static void held(GCArena *a, uint32_t cell)
{
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_CONSTRUCT);
  assert(lj_arena_ready_get(a, cell) == 0);
}

static void state(const char *tag, global_State *g, TGState *tg,
                  GCArena *a, uint32_t cell)
{
  printf("STATE %s phase=%u cycle=%u deferred=%" PRIu64
         " owner_runs=%" PRIu64 " arenas=%" PRIu64
         " ptr=%p cell=%u block=%u root=%u life=%u ready=%u"
         " mark=%u flags=%u cursor=%u late=%u retire=%" PRIu64
         " quarantine=%p owned=%p\n",
    tag, gc2_phase_acq(g), gc2_cycle_acq(g), gc2_deferred_epoch_acq(g),
    gc2_sweep_owner_runs_acq(g), gc2_sweep_owner_arenas_acq(g),
    (void *)a, cell, lj_arena_bm_get(a->block, cell),
    lj_arena_root_state_acq(a, cell), lj_arena_lifetime_state_acq(a, cell),
    lj_arena_ready_get(a, cell), lj_arena_bm_get(a->mark, cell),
    lj_arena_flags_acq(a), a->hdr.reclaim_cell, lj_arena_late_get(a, cell),
    la_load64_acq(&a->hdr.retire_epoch),
    (void *)tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE],
    (void *)tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]);
  fflush(stdout);
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCupval *uv;
  GCtab *child;
  GCArena *a;
  uint32_t cell, anchoridx;
  uint64_t before, start, elapsed;
  int result;
  TValue v;
  TValue *anchor;
  int cancel = argc > 1 && strcmp(argv[1], "cancel") == 0;
  int automatic = argc > 1 && strcmp(argv[1], "automatic") == 0;
  L = ljt_lua_newstate_openlibs();
  g = G(L); tg = L2TG(L);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  lua_newtable(L);
  lua_pushinteger(L, 7391);
  lua_setfield(L, -2, "owner_defer_sentinel");
  child = tabV(L->top-1);
  uv = new_constructing(L, L->top-1);
  a = lj_arena_of(uv); cell = lj_arena_cellof(uv);
  held(a, cell);
  state("birth", g, tg, a, cell);
  if (automatic) {
    unsigned i;
    /* Enter SWEEP through the production bounded driver, stopping before its
     * first physical owner quantum. No protocol state is manufactured. */
    for (i = 0; i < 64 && gc2_phase_acq(g) != LJ_GC2_SWEEP; i++)
      (void)lj_gc2_step_explicit(L, 1);
    assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
    assert(gc2_deferred_epoch_acq(g) == 0);
    state("automatic-entry", g, tg, a, cell);
  }
  before = gc2_deferred_epoch_acq(g);
  start = now_ns();
  result = automatic ? lj_gc_step(L) : lj_gc2_collect_active(L);
  elapsed = now_ns() - start;
  state("nested-return", g, tg, a, cell);
  printf("COLLECT result=%d elapsed_ns=%" PRIu64 " defer_delta=%" PRIu64 "\n",
         result, elapsed, gc2_deferred_epoch_acq(g) - before);
  fflush(stdout);
  assert(result == (automatic ? -1 : 0) && elapsed < UINT64_C(1000000000));
  assert(gc2_deferred_epoch_acq(g) == before + 1u);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == a);
  assert((lj_arena_flags_acq(a) & LJ_AF_PREPSWEEP) == 0);
  held(a, cell);
  if (cancel) {
    lj_mem_freegco_unpublished(g, uv, sizeof(GCupval));
    state("cancelled", g, tg, a, cell);
    assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
    /* Quarantine owns the later physical transition. The real cancel/free
     * path publishes irrevocable late intent and a fresh grace requirement. */
    assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_late_get(a, cell));
    assert(la_load64_acq(&a->hdr.retire_epoch) == UINT64_MAX);
  } else {
    assert(lj_gc_linkobj_new(g, obj2gco(uv)) == LJ_GC_ROOT_LINKED);
    lj_gc_pubobjroot(L, obj2gco(uv));
    setgcV(L, &v, obj2gco(uv), LJ_TUPVAL);
    anchor = lj_tg_root_anchor_push(L, tg, &v, &anchoridx);
    assert(anchor != NULL);
    lj_gc_pubroot(L, anchor);
    assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
    assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_ready_get(a, cell));
    state("published", g, tg, a, cell);
    lua_settop(L, 0);  /* The upvalue anchor is now the child's only Lua root. */
  }
  start = now_ns();
  result = lj_gc2_collect_active(L);
  elapsed = now_ns() - start;
  printf("TERMINAL result=%d elapsed_ns=%" PRIu64 " phase=%u quarantine=%p\n",
         result, elapsed, gc2_phase_acq(g),
         (void *)tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE]);
  fflush(stdout);
  assert(result == 1 && elapsed < UINT64_C(1000000000));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(lj_gc2_test_ssb_empty(g));
  if (!cancel) {
    assert(lj_gc2_obj_valid(g, obj2gco(uv)));
    assert(tvistab(uvval(uv)) && tabV(uvval(uv)) == child);
    assert(lj_gc2_obj_valid(g, obj2gco(child)));
    copyTVrel(L, L->top, uvval(uv));
    lj_state_stack_pubtv(L, L, L->top);
    L->top++;
    lua_getfield(L, -1, "owner_defer_sentinel");
    assert(lua_tointeger(L, -1) == 7391);
    lj_tg_root_anchor_pop(tg, anchoridx);
  }
  lua_settop(L, 0);
  assert(lj_gc2_collect_active(L) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_close(L);
  puts("t-owner-defer OK");
  return 0;
}
