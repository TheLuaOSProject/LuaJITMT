/* A certified FREE mapping rejects stale semantic publishers without bytes. */
#if !defined(LJ_ARENA_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS)
#error "runtime empty-spare fixture requires arena and GC2 test helpers"
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TGAlloc alloc;
  PRNGState rs;
  GCArena *a;
  void *padding, *old;
  uintptr_t page;
  size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
  uint32_t reason = LJ_ARENA_FINISH_NONE, i, cell;
  GC2StatsSnapshot before, after;
  LJGC2TabStamp *stamp;
  LJGC2TableDescSnap desc0, desc1;
  assert(L && pagesize != 0 && (pagesize & (pagesize-1u)) == 0);
  g = G(L);
  tg = g->main_tg;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  lj_arena_alloc_owner_rel(&alloc, lj_tg_tid_acq(tg));
  lj_arena_alloc_owner_tg_rel(&alloc, tg);
  lj_arena_alloc_set_registry(&alloc, (HugeTab *)gc2_small_arena_tab_acq(g));
  /* Register a private allocator's real mapping in this universe. Its entire
  ** incarnation is freed before invoking any semantic helper or collector. */
  padding = lj_arena_alloc(&alloc, &rs, 8192u, LJ_AF_TRAVERSABLE);
  old = lj_arena_alloc(&alloc, &rs, 64u, LJ_AF_TRAVERSABLE);
  assert(padding && old && lj_arena_of(padding) == lj_arena_of(old));
  a = lj_arena_of(old);
  cell = lj_arena_cellof(old);
  page = (uintptr_t)old & ~(uintptr_t)(pagesize-1u);
  assert(page >= (uintptr_t)a + sizeof(*a));
  assert(page + pagesize <= (uintptr_t)a + LJ_ARENA_SIZE);
  memset(old, 0xa5, 64u);
  lj_arena_free(&alloc, old, 64u);
  lj_arena_free(&alloc, padding, 8192u);
  assert(lj_arena_alloc_prepare_sweep_kind(&alloc, LJ_ARENAK_TRAVERSABLE));
  assert(lj_arena_alloc_quarantine_one(
    &alloc, LJ_ARENAK_TRAVERSABLE, 0) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(
    &alloc, LJ_ARENAK_TRAVERSABLE, a, 7u, 1, &reason));
  assert(lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED);

  /* This is a real MARK cycle, so token publication cannot reject only on an
  ** IDLE phase gate. Main-owner execution excludes parallel GC workers. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK && gc2_n_workers_acq(g) == 0);
  lj_gc2_stats_snapshot(g, &before);
  for (i = 0; i < 64u; i++) {
    assert(lj_arena_alloc_prepare_sweep_kind(&alloc, LJ_ARENAK_TRAVERSABLE));
    assert(alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(a->hdr.live_cells == 0);
  }
  lj_gc2_stats_snapshot(g, &after);
  assert(before.total_bytes == after.total_bytes);
  assert(before.alloc_total_bytes == after.alloc_total_bytes);
  assert(before.live_estimate == after.live_estimate);
  assert(before.cycle_starts == after.cycle_starts);
  assert(before.root_spine_objects == after.root_spine_objects);
  assert(before.sweep_owner_arenas == after.sweep_owner_arenas);
  assert(before.sweep_owner_live_cells == after.sweep_owner_live_cells);
  assert(lj_arena_alloc_owned_count_acq(&alloc, LJ_ARENAK_TRAVERSABLE) == 0);
  assert(lj_arena_alloc_needsweep_count_acq(&alloc, LJ_ARENAK_TRAVERSABLE) == 0);
  stamp = lj_arena_gc2_stamp_acq(old);
  assert(stamp);
  desc0 = lj_gc2_tabledesc_snapshot(&g->gc2.table_rescan_desc);
  assert(desc0.state == LJ_GC2_TABLEDESC_IDLE);
  assert(mprotect((void *)page, pagesize, PROT_NONE) == 0);
  assert(lj_gc2_test_recovery_publish(g, (GCobj *)old) == 0);
  assert(lj_gc2_test_table_token_request(g, (GCtab *)old) == 0);
  assert(lj_gc2_test_table_expected_status(g, (GCtab *)old) ==
         LJ_GC2_TV_EDGE_STALE);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_recovery_state_acq(a, cell) == LJ_ARENA_RECOVERY_IDLE);
  assert(lj_gc2_table_token_state(la_load64_acq(&stamp->token.control)) ==
         LJ_GC2_TABLE_TOKEN_NONE);
  desc1 = lj_gc2_tabledesc_snapshot(&g->gc2.table_rescan_desc);
  assert(desc1.state == desc0.state && desc1.generation == desc0.generation);
  assert(mprotect((void *)page, pagesize, PROT_READ|PROT_WRITE) == 0);
  assert(lj_arena_alloc_terminal_reconcile(&alloc));
  assert(lj_arena_alloc_fini_try(&alloc));
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_close(L);
  puts("arena empty reclaimed runtime tests passed");
  return 0;
}
