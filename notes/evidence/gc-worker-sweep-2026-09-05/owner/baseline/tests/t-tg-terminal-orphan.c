/*
** Deterministic close-path TG allocator-orphan regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_arena.h"
#include "lj_dispatch.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"

enum { NTG = 3, NMAP = 4 };

typedef struct MapRecord {
  void *base;
  size_t mapsize;
  uint32_t unmaps;
} MapRecord;

static TGState *forced_tg[NTG];
static void *forced_payload[NTG];
static uint32_t transfer_failures[NTG];
static MapRecord map_record[NMAP];

extern int __real_lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src,
					     uint32_t owner_tid);

int __wrap_lj_arena_hugetab_transfer(HugeTab *dst, HugeTab *src,
				      uint32_t owner_tid)
{
  uint32_t i;
  for (i = 0; i < NTG; i++) {
    if (forced_tg[i] && src == &forced_tg[i]->huge) {
      transfer_failures[i]++;
      return 0;
    }
  }
  return __real_lj_arena_hugetab_transfer(dst, src, owner_tid);
}

#if defined(__linux__)
extern int __real_munmap(void *addr, size_t len);

int __wrap_munmap(void *addr, size_t len)
{
  uint32_t i;
  for (i = 0; i < NMAP; i++)
    if (map_record[i].base == addr && map_record[i].mapsize == len)
      map_record[i].unmaps++;
  return __real_munmap(addr, len);
}
#endif

static void track_mapping(uint32_t slot, void *p, size_t size)
{
  assert(slot < NMAP && p != NULL);
  map_record[slot].base = (void *)lj_arena_of(p);
  map_record[slot].mapsize = lj_arena_huge_mapsize(size);
  map_record[slot].unmaps = 0;
  assert(map_record[slot].mapsize != 0);
}

static int tg_list_contains(global_State *g, TGState *target)
{
  TGState *tg;
  uint32_t n = 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == target)
      return 1;
    assert(tg != lj_tg_next_acq(tg));
    assert(++n < 1000000u);
  }
  return 0;
}

static GCArena *first_registered_arena(TGAlloc *alloc)
{
  uint32_t k;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    GCArena *a = alloc->owned[k];
    if (!a)
      a = alloc->needsweep[k];
    if (!a)
      a = alloc->quarantine[k];
    if (!a)
      a = (GCArena *)la_loadptr_acq(
	(void *const *)&alloc->reclaimed[k]);
    if (!a && k == LJ_ARENAK_TRAVERSABLE)
      a = lj_arena_alloc_empty_reclaimed_head(alloc);
    if (a && (lj_arena_flags_acq(a) & LJ_AF_REGISTERED))
      return a;
  }
  return NULL;
}

static void init_live_tg(global_State *g, TGState *tg, uint8_t flags,
			 uint32_t slot)
{
  size_t huge_size = LJ_HUGE_THRESHOLD + 4096u + slot * 16u;
  lj_tg_init_thread(g, tg, NULL, 1);
  lj_tg_flags_or_rlx(tg, flags);
  lj_tg_tid_rel(tg, lj_thr_newid());
  lj_tg_derive_prng(g, tg, lj_tg_tid_acq(tg));
  forced_tg[slot] = tg;
  forced_payload[slot] =
    lj_arena_allocd_alloc(&tg->allocd, huge_size, LJ_AF_TRAVERSABLE);
  assert(forced_payload[slot] != NULL);
  assert(lj_arena_hugetab_lookup(&tg->huge, forced_payload[slot], NULL) == 1);
  track_mapping(slot, forced_payload[slot], huge_size);
  lj_tg_attach(g, tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *parent_tg;
  TGState *child_tg;
  TGState *worker_tg;
  HugeTab *small_registry;
  LJHugeTabHdr *small_registry_h;
  GCArena *registered_arena;
  LJHugeReader worker_reader = { NULL, NULL, 0 };
  LJTGSlotSnap slot_snap;
  size_t child_size = sizeof(TGState);
  uint32_t i;

  assert(L != NULL);
  g = G(L);
  lj_thr_set_tg(g->main_tg);
  assert(gc2_n_threads_acq(g) == 1u);
  assert(lj_arena_hugetab_lookup(&g->main_tg->huge, G2GG(g), NULL) == 0);

  /* Raw GC2 teardown cannot free the small-arena directory while allocator
  ** entries remain. A blocked try preserves every enclosing alias and exact
  ** lookup; close retries only after main allocator deletion drains it. */
  small_registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  assert(small_registry != NULL && small_registry->h != NULL);
  small_registry_h = small_registry->h;
  registered_arena = first_registered_arena(&g->main_tg->alloc);
  assert(registered_arena != NULL);
  assert(lj_arena_hugetab_lookup(small_registry, registered_arena, NULL));
  assert(!lj_gc2_small_arena_registry_fini_try(g));
  assert(gc2_small_arena_tab_acq(g) == small_registry);
  assert(g->main_tg->alloc.smalltab == small_registry);
  assert(small_registry->h == small_registry_h);
  assert(lj_arena_hugetab_lookup(small_registry, registered_arena, NULL));

  parent_tg = (TGState *)malloc(sizeof(*parent_tg));
  worker_tg = (TGState *)malloc(sizeof(*worker_tg));
  assert(parent_tg != NULL && worker_tg != NULL);
  init_live_tg(g, parent_tg, TGF_HEAP, 0);

  /* Model threading.spawn exactly: the already-linked parent owns the raw TG
  ** body, then the child CAS-prepends. Reversing terminal list order would
  ** unmap this body with the parent before the child can finalize it. */
  child_tg = (TGState *)lj_arena_allocd_alloc(&parent_tg->allocd,
					       child_size, 0);
  assert(child_tg != NULL && lj_arena_huge_mapsize(child_size) != 0);
  lj_gc_total_add(g, (GCSize)child_size);
  track_mapping(3, child_tg, child_size);
  init_live_tg(g, child_tg, TGF_LUA_ALLOC|TGF_DEFER_FREE, 1);

  init_live_tg(g, worker_tg, 0, 2);
  lj_tg_detach(g, worker_tg);
  lj_tg_detach(g, child_tg);
  lj_tg_detach(g, parent_tg);
  assert(gc2_n_threads_acq(g) == 1u);
  assert(gc2_worker_tg_retired_acq(g) == NULL);
  lj_tg_worker_retire_next_rel(worker_tg, NULL);
  gc2_worker_tg_retired_rel(g, worker_tg);

  /* Every ordinary transfer is forced to fail. A transient terminal gate
  ** owner must make the orphan pass return without changing registry/list
  ** ownership; lua_close then exercises the real final owner-lookup helper. */
  mt_shutdown_rel(g, 1);
  gc2_worker_active_rel(g, 1);
  assert(lj_tg_reclaim_dead_terminal_orphans(g) == 0u);
  assert(tg_list_contains(g, parent_tg));
  assert(tg_list_contains(g, child_tg));
  assert(tg_list_contains(g, worker_tg));
  assert(gc2_worker_tg_retired_acq(g) == worker_tg);
  gc2_worker_active_rel(g, 0);

  /* First force the ordinary transfer route to visit every dead TG. The
  ** wrapper rejects each transfer, leaving terminal orphan ownership as the
  ** capacity-independent fallback. */
  assert(lj_tg_reclaim_dead_terminal(g) == 0u);
  assert(lj_arena_hugetab_reader_acquire(
	   &worker_tg->huge, forced_payload[2], &worker_reader, NULL) ==
	 LJ_ARENA_HUGE_READER_ACQUIRED);

  /* Checked inner refusal must retain both enclosing owners. The stable slot
  ** stays non-borrowable RECLAIMING and the embedded retire list remains the
  ** raw storage owner until a later retry observes DONE. */
  assert(lj_tg_reclaim_dead_terminal_orphans(g) == 2u);
  assert(tg_list_contains(g, worker_tg));
  assert(gc2_worker_tg_retired_acq(g) == worker_tg);
  assert(lj_tg_fini_state_acq(worker_tg) == TG_FINI_RETRY);
  assert(lj_tg_flags_test_acq(worker_tg, TGF_HUGETAB));
  assert(worker_tg->huge.h != NULL);
  assert(lj_arena_hugetab_lookup(
	   &worker_tg->huge, forced_payload[2], NULL));
  assert(map_record[2].unmaps == 0u);
  assert(lj_tgregistry_key_snapshot(&worker_tg->registry_key, &slot_snap) ==
	 LJ_TGSLOT_OK);
  assert(slot_snap.state == LJ_TGSLOT_RECLAIMING &&
	 slot_snap.lease_count == 0u);

  assert(lj_arena_hugetab_reader_release(&worker_reader, NULL) ==
	 LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_tg_reclaim_dead_terminal_orphans(g) == 1u);
  assert(!tg_list_contains(g, worker_tg));
  assert(gc2_worker_tg_retired_acq(g) == worker_tg);
  assert(lj_tg_fini_state_acq(worker_tg) == TG_FINI_DONE);
  assert(map_record[2].unmaps == 1u);

  mt_shutdown_rel(g, 0);

  lua_close(L);

  for (i = 0; i < NTG; i++)
    assert(transfer_failures[i] != 0);
#if defined(__linux__)
  for (i = 0; i < NMAP; i++)
    assert(map_record[i].unmaps == 1u);
#endif
  printf("t-tg-terminal-orphan OK: close drained failed transfers once in "
	 "owner dependency order\n");
  return 0;
}
