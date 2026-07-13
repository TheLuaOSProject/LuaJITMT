/*
** Focused test for owner-local arena sweep scaffolding.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static uint32_t bin_count(TGAlloc *alloc, uint32_t kind)
{
  uint32_t b, n = 0;
  for (b = 0; b < LJ_ALLOC_NBINS; b++) {
    LJArenaFreeRun *r = alloc->bins[kind][b];
    while (r) {
      n++;
      r = r->next;
    }
  }
  return n;
}

static void assert_no_bins(TGAlloc *alloc, uint32_t kind)
{
  uint32_t b;
  for (b = 0; b < LJ_ALLOC_NBINS; b++)
    assert(alloc->bins[kind][b] == NULL);
}

static void test_dtor_identity_pins_legacy_sweep(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  uint32_t cell;
  const uint32_t ncells = 1u;

  lj_arena_alloc_init(&alloc);
  alloc.alloc_black = 0;
  assert(lj_arena_reserve_bump_dtor(&alloc, rs, LJ_AF_TRAVERSABLE,
    ncells, LJ_ARENA_DTOR_LFUNC0, &a, &cell));
  assert(lj_arena_dtor_kind_acq(a, cell) == LJ_ARENA_DTOR_LFUNC0);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  /* No body is needed: the legacy bitmap owner must pin arena-typed identity
  ** without inspecting or dispatching bytes it cannot semantically destroy. */
  lj_arena_ready_set_unpublished(a, cell);
  lj_arena_block_set(a, cell);
  assert(lj_arena_dtor_construct_commit(a, cell));
  assert(lj_arena_alloc_prepare_sweep_kind(
    &alloc, LJ_ARENAK_TRAVERSABLE));
  assert(lj_arena_sweep_one(
    &alloc, LJ_ARENAK_TRAVERSABLE, 1u, 0) == a);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_dtor_kind_acq(a, cell) == LJ_ARENA_DTOR_LFUNC0);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
    LJ_ARENA_LIFETIME_LIVE);
  lj_arena_alloc_fini(&alloc);
}

static void test_blockzero_dtor_survives_quarantine(PRNGState *rs)
{
  TGAlloc alloc;
  GCArena *a;
  void *p;
  uint32_t cell, interior, reason = LJ_ARENA_FINISH_NONE;

  lj_arena_alloc_init(&alloc);
  alloc.alloc_black = 0;
  p = lj_arena_alloc(&alloc, rs, 32, LJ_AF_TRAVERSABLE);
  assert(p != NULL);
  a = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  interior = cell + 1u;
  assert(lj_arena_ncells(32) > 1u);
  assert(!lj_arena_bm_get(a->block, interior));
  assert(lj_arena_lifetime_state_acq(a, interior) ==
	 LJ_ARENA_LIFETIME_FREE);

  /* A block-zero kind is malformed, but it remains authoritative fail-closed
  ** metadata. Quarantine may only clear kinds at starts whose old block bit it
  ** actually removes; it must not erase unrelated constructor/corruption
  ** evidence merely because the cell is absent from the live-start bitmap. */
  lj_arena_bm_set(a->dtor[3], interior);
  assert(lj_arena_dtor_kind_acq(a, interior) == 8u);
  assert(lj_arena_alloc_prepare_sweep_kind(
    &alloc, LJ_ARENAK_TRAVERSABLE));
  assert(lj_arena_alloc_quarantine_one(&alloc, LJ_ARENAK_TRAVERSABLE,
					  0) == a);
  assert(lj_arena_reclaim_seal(a));
  assert(lj_arena_alloc_quarantine_finish(&alloc,
    LJ_ARENAK_TRAVERSABLE, a, 1u, 0, &reason));
  assert(reason == LJ_ARENA_FINISH_COMMITTED);
  assert(lj_arena_dtor_kind_acq(a, interior) == 8u);
  lj_arena_bm_clear(a->dtor[3], interior);
  lj_arena_alloc_fini(&alloc);
}

static void test_root_membership(PRNGState *rs)
{
  TGAlloc src, dst;
  GCArena *a;
  void *p, *adjacent, *other, *reuse;
  uint32_t cell, adjacent_cell, before;

  lj_arena_alloc_init(&src);
  lj_arena_alloc_init(&dst);
  src.alloc_black = 0;
  p = lj_arena_alloc(&src, rs, 32, 0);
  src.alloc_black = 1;
  adjacent = lj_arena_alloc(&src, rs, 32, 0);
  other = lj_arena_alloc(&src, rs, 32, 0);
  assert(p != NULL && adjacent != NULL && other != NULL);
  a = lj_arena_of(p);
  assert(lj_arena_of(adjacent) == a && lj_arena_of(other) == a);
  cell = lj_arena_cellof(p);
  adjacent_cell = lj_arena_cellof(adjacent);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(lj_arena_root_state_acq(a, adjacent_cell) == LJ_ARENA_ROOT_NONE);

  /* Exercise every encoded state while proving that masked CAS updates do not
  ** disturb another allocation start packed into the same metadata word. */
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_NONE,
					 LJ_ARENA_ROOT_LINKING));
  assert(lj_arena_root_state_cas(a, adjacent_cell, LJ_ARENA_ROOT_NONE,
					 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_root_state_acq(a, adjacent_cell) ==
	 LJ_ARENA_ROOT_UNLINKING);
  assert(!lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_NONE,
					  LJ_ARENA_ROOT_MEMBER));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_LINKING,
					 LJ_ARENA_ROOT_MEMBER));
  assert(lj_arena_root_state_acq(a, adjacent_cell) ==
	 LJ_ARENA_ROOT_UNLINKING);
  assert(lj_arena_root_state_cas(a, adjacent_cell,
					 LJ_ARENA_ROOT_UNLINKING,
					 LJ_ARENA_ROOT_NONE));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER,
					 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_UNLINKING,
					 LJ_ARENA_ROOT_MEMBER));
  assert(!lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER, 4u));

  /* A logical free while MEMBER must retain the exact body and cannot publish
  ** a reusable bin node. The late bit records the free for a later cycle. */
  before = bin_count(&src, LJ_ARENAK_PLAIN);
  lj_arena_free(&src, p, 32);
  assert(bin_count(&src, LJ_ARENAK_PLAIN) == before);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_alloc(&src, rs, 32, 0) != p);

  /* Sweep and owner transfer preserve both committed and transient root state.
  ** The allocation remains unavailable even though its ordinary mark was dead. */
  assert(lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_PLAIN));
  assert(lj_arena_sweep_one(&src, LJ_ARENAK_PLAIN, 71u, 0) == a);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_alloc_transfer(&dst, &src) == 1u);
  assert(src.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(dst.owned[LJ_ARENAK_PLAIN] == a);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);

  /* Once the explicit owner relinquishes membership, the remembered free is
  ** consumed by the next sweep and only then can allocation reuse the cell. */
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_MEMBER,
					 LJ_ARENA_ROOT_UNLINKING));
  assert(lj_arena_root_state_cas(a, cell, LJ_ARENA_ROOT_UNLINKING,
					 LJ_ARENA_ROOT_NONE));
  lj_arena_alloc_clear_marks(&dst);
  assert(lj_arena_alloc_prepare_sweep_kind(&dst, LJ_ARENAK_PLAIN));
  assert(lj_arena_sweep_one(&dst, LJ_ARENAK_PLAIN, 72u, 0) == a);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  assert(!lj_arena_late_get(a, cell));
  dst.alloc_black = 0;
  reuse = lj_arena_alloc(&dst, rs, 32, 0);
  assert(reuse == p);

  lj_arena_alloc_fini(&src);
  lj_arena_alloc_fini(&dst);

  {
    TGAlloc hold;
    GCArena *held_arena;
    void *held;
    uint32_t held_cell, reason = LJ_ARENA_FINISH_NONE;
    lj_arena_alloc_init(&hold);
    hold.alloc_black = 0;
    held = lj_arena_alloc(&hold, rs, 64, LJ_AF_TRAVERSABLE);
    assert(held != NULL);
    held_arena = lj_arena_of(held);
    held_cell = lj_arena_cellof(held);
    assert(lj_arena_lifetime_state_acq(held_arena, held_cell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_root_state_cas(held_arena, held_cell,
	LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_MEMBER));
    assert(lj_arena_alloc_prepare_sweep_kind(&hold,
	LJ_ARENAK_TRAVERSABLE));
    assert(lj_arena_alloc_quarantine_one(&hold, LJ_ARENAK_TRAVERSABLE,
					  0) == held_arena);
    assert(lj_arena_reclaim_seal(held_arena));
    /* Membership is a conservative live pin, not actionable recovery work:
    ** quarantine may commit the other cells while preserving this one. */
    assert(lj_arena_alloc_quarantine_finish(&hold,
	LJ_ARENAK_TRAVERSABLE, held_arena, 73u, 0, &reason));
    assert(reason == LJ_ARENA_FINISH_COMMITTED);
    assert(lj_arena_root_state_acq(held_arena, held_cell) ==
	   LJ_ARENA_ROOT_MEMBER);
    assert(lj_arena_bm_get(held_arena->block, held_cell));
    assert(lj_arena_lifetime_state_acq(held_arena, held_cell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_root_state_cas(held_arena, held_cell,
	LJ_ARENA_ROOT_MEMBER, LJ_ARENA_ROOT_NONE));
    lj_arena_alloc_fini(&hold);
  }
}

int main(void)
{
  PRNGState rs;
  TGAlloc alloc;
  LJArenaAllocD travad;
  void *dead1, *live1, *oldfree, *live2, *taildead, *bump64, *bump32;
  void *tdead, *tlive;
  GCArena *plain, *trav, *swept;
  uint32_t cdead1, clive1, coldfree, clive2, ctaildead;
  uint32_t ctdead, ctlive;

  lj_prng_seed_fixed(&rs);
  test_dtor_identity_pins_legacy_sweep(&rs);
  test_blockzero_dtor_survives_quarantine(&rs);
  test_root_membership(&rs);
  lj_arena_alloc_init(&alloc);
  lj_arena_allocd_init(&travad, &alloc, &rs, LJ_AF_TRAVERSABLE);

  alloc.alloc_black = 0;
  dead1 = lj_arena_alloc(&alloc, &rs, 64, 0);
  alloc.alloc_black = 1;
  live1 = lj_arena_alloc(&alloc, &rs, 32, 0);
  alloc.alloc_black = 0;
  oldfree = lj_arena_alloc(&alloc, &rs, 32, 0);
  alloc.alloc_black = 1;
  live2 = lj_arena_alloc(&alloc, &rs, 16, 0);
  alloc.alloc_black = 0;
  taildead = lj_arena_alloc(&alloc, &rs, 16, 0);

  assert(dead1 && live1 && oldfree && live2 && taildead);
  plain = lj_arena_of(dead1);
  assert(lj_arena_of(live1) == plain);
  assert(lj_arena_of(oldfree) == plain);
  assert(lj_arena_of(live2) == plain);
  assert(lj_arena_of(taildead) == plain);
  cdead1 = lj_arena_cellof(dead1);
  clive1 = lj_arena_cellof(live1);
  coldfree = lj_arena_cellof(oldfree);
  clive2 = lj_arena_cellof(live2);
  ctaildead = lj_arena_cellof(taildead);
  lj_arena_free(&alloc, oldfree, 32);

  alloc.alloc_black = 0;
  tdead = lj_arena_alloc(&alloc, &rs, 128, LJ_AF_TRAVERSABLE);
  alloc.alloc_black = 1;
  tlive = lj_arena_alloc(&alloc, &rs, 32, LJ_AF_TRAVERSABLE);
  assert(tdead && tlive);
  trav = lj_arena_of(tdead);
  assert(lj_arena_of(tlive) == trav);
  ctdead = lj_arena_cellof(tdead);
  ctlive = lj_arena_cellof(tlive);
  /* Model a dead fixed cdata allocation in a non-largest free run. Sweep
  ** rebuild must scrub its complete coverage before linking that run into a
  ** reusable bin; otherwise the next allocation reaches arena_set_alloc with
  ** stale typed metadata. */
  assert(lj_arena_allocd_publish_cdata(&travad, tdead, 128, 0) == 1);
  assert(lj_arena_cdata_get(trav, ctdead) == 1);
  assert(lj_arena_ready_get(trav, ctdead) == 1);

  assert(alloc.owned[LJ_ARENAK_PLAIN] == plain);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == trav);
  lj_arena_alloc_prepare_sweep(&alloc);
  assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.needsweep[LJ_ARENAK_PLAIN] == plain);
  assert(alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == trav);
  assert((plain->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  assert((trav->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  assert(alloc.bump[LJ_ARENAK_PLAIN].a == NULL);
  assert(alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert_no_bins(&alloc, LJ_ARENAK_PLAIN);
  assert_no_bins(&alloc, LJ_ARENAK_TRAVERSABLE);

  swept = lj_arena_sweep_one(&alloc, LJ_ARENAK_PLAIN, 7, 0);
  assert(swept == plain);
  assert(alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.owned[LJ_ARENAK_PLAIN] == plain);
  assert((plain->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(plain->hdr.sweep_epoch == 7);
  assert(plain->hdr.live_cells == 3);
  assert(lj_arena_state(plain, cdead1) == 1);
  assert(lj_arena_state(plain, clive1) == 2);
  assert(lj_arena_state(plain, coldfree) == 1);
  assert(lj_arena_state(plain, clive2) == 2);
  assert(lj_arena_state(plain, ctaildead) == 1);
  assert(alloc.bump[LJ_ARENAK_PLAIN].a == plain);
  assert(alloc.bump[LJ_ARENAK_PLAIN].cell == ctaildead);
  assert(alloc.bump[LJ_ARENAK_PLAIN].end == LJ_ARENA_CELLS);
  assert(bin_count(&alloc, LJ_ARENAK_PLAIN) == 2);

  assert(lj_arena_alloc(&alloc, &rs, 128, 0) == taildead);
  bump64 = lj_arena_alloc(&alloc, &rs, 64, 0);
  bump32 = lj_arena_alloc(&alloc, &rs, 32, 0);
  assert(lj_arena_of(bump64) == plain && lj_arena_of(bump32) == plain);
  assert(lj_arena_cellof(bump64) ==
	 ctaildead + lj_arena_ncells(128));
  assert(lj_arena_cellof(bump32) ==
	 lj_arena_cellof(bump64) + lj_arena_ncells(64));
  assert(lj_arena_state(plain, cdead1) == 1);
  assert(lj_arena_state(plain, coldfree) == 1);
  assert(bin_count(&alloc, LJ_ARENAK_PLAIN) == 2);

  swept = lj_arena_sweep_one(&alloc, LJ_ARENAK_TRAVERSABLE, 9, 1);
  assert(swept == trav);
  assert(alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == trav);
  assert((trav->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(trav->hdr.sweep_epoch == 9);
  assert(trav->hdr.live_cells == 2);
  assert(lj_arena_state(trav, ctdead) == 1);
  assert(lj_arena_state(trav, ctlive) == 3);
  {
    uint32_t i;
    for (i = 0; i < lj_arena_ncells(128); i++)
      assert(lj_arena_cdata_get(trav, ctdead + i) == 0);
  }
  assert(lj_arena_ready_get(trav, ctdead) == 0);
  assert(bin_count(&alloc, LJ_ARENAK_TRAVERSABLE) == 1);
  /* Force the fixture's private tail empty to exercise the separately
  ** published, fully scrubbed tdead run. */
  alloc.bump[LJ_ARENAK_TRAVERSABLE].cell =
    alloc.bump[LJ_ARENAK_TRAVERSABLE].end;
  assert(lj_arena_alloc(&alloc, &rs, 128, LJ_AF_TRAVERSABLE) == tdead);

  assert(lj_arena_sweep_one(&alloc, LJ_ARENAK_TRAVERSABLE, 10, 0) == NULL);
  assert(lj_arena_sweep_one(&alloc, LJ_ARENA_NKINDS, 10, 0) == NULL);

  lj_arena_alloc_fini(&alloc);
  assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);

  {
    TGAlloc rebuild;
    void *r1, *r2, *r3, *big, *tail;
    lj_arena_alloc_init(&rebuild);
    r1 = lj_arena_alloc(&rebuild, &rs, 32, 0);
    r2 = lj_arena_alloc(&rebuild, &rs, 64, 0);
    r3 = lj_arena_alloc(&rebuild, &rs, 16, 0);
    assert(r1 != NULL && r2 != NULL && r3 != NULL);
    lj_arena_free(&rebuild, r1, 32);
    lj_arena_free(&rebuild, r2, 64);
    assert(bin_count(&rebuild, LJ_ARENAK_PLAIN) == 2);
    lj_arena_alloc_rebuild_free(&rebuild);
    assert(bin_count(&rebuild, LJ_ARENAK_PLAIN) == 1);
    rebuild.bump[LJ_ARENAK_PLAIN].cell =
      rebuild.bump[LJ_ARENAK_PLAIN].end;
    big = lj_arena_alloc(&rebuild, &rs, 96, 0);
    assert(big == r1);
    /* Rebuild coalesces the adjacent 2- and 4-cell free records into this
    ** six-cell allocation. No old state-1 boundary may survive inside it. */
    assert(lj_arena_state(lj_arena_of(big),
			  lj_arena_cellof(big) + 2u) == 0);
    lj_arena_free(&rebuild, r3, 16);
    lj_arena_alloc_rebuild_free(&rebuild);
    tail = lj_arena_alloc(&rebuild, &rs, 64, 0);
    assert((uintptr_t)tail < (uintptr_t)big ||
	   (uintptr_t)tail >= (uintptr_t)big + 96u);
    lj_arena_alloc_fini(&rebuild);
  }

  {
    TGAlloc clear;
    void *black, *freep;
    GCArena *a;
    uint32_t cblack, cfree;
    lj_arena_alloc_init(&clear);
    clear.alloc_black = 1;
    black = lj_arena_alloc(&clear, &rs, 32, 0);
    clear.alloc_black = 0;
    freep = lj_arena_alloc(&clear, &rs, 32, 0);
    assert(black != NULL && freep != NULL);
    a = lj_arena_of(black);
    assert(lj_arena_of(freep) == a);
    cblack = lj_arena_cellof(black);
    cfree = lj_arena_cellof(freep);
    lj_arena_free(&clear, freep, 32);
    assert(lj_arena_state(a, cblack) == 3);
    assert(lj_arena_state(a, cfree) == 1);
    lj_arena_alloc_clear_marks(&clear);
    assert(lj_arena_state(a, cblack) == 2);
    assert(lj_arena_state(a, cfree) == 1);
    lj_arena_alloc_fini(&clear);
  }

  {
    TGAlloc reserve;
    GCArena *a, *ra;
    void *r1, *r2, *guard;
    uint32_t c1, c2, rc, i;
    lj_arena_alloc_init(&reserve);
    r1 = lj_arena_alloc(&reserve, &rs, 32, 0);
    r2 = lj_arena_alloc(&reserve, &rs, 64, 0);
    guard = lj_arena_alloc(&reserve, &rs, 16, 0);
    assert(r1 != NULL && r2 != NULL && guard != NULL);
    a = lj_arena_of(r1);
    assert(lj_arena_of(r2) == a && lj_arena_of(guard) == a);
    c1 = lj_arena_cellof(r1);
    c2 = lj_arena_cellof(r2);
    assert(c2 == c1 + 2u);
    lj_arena_free(&reserve, r1, 32);
    lj_arena_free(&reserve, r2, 64);
    /* Retire the unrelated fresh-arena bump tail so reserve_bump must consume
    ** the rebuilt adjacent run below. */
    reserve.bump[LJ_ARENAK_PLAIN].cell =
      reserve.bump[LJ_ARENAK_PLAIN].end;
    lj_arena_alloc_rebuild_free(&reserve);
    assert(lj_arena_reserve_bump(&reserve, &rs, 0, 2u, &ra, &rc));
    assert(ra == a && rc == c1);
    /* The six-cell coalesced run becomes one private reservation plus bump
    ** tail. Neither the old r1 nor r2 boundary may remain visible. */
    for (i = 0; i < 6u; i++)
      assert(lj_arena_state(a, c1 + i) == 0);
    lj_arena_bm_set(a->block, rc);
    assert(reserve.bump[LJ_ARENAK_PLAIN].a == a);
    assert(reserve.bump[LJ_ARENAK_PLAIN].cell == c1 + 2u);
    assert(reserve.bump[LJ_ARENAK_PLAIN].end == c1 + 6u);
    lj_arena_alloc_fini(&reserve);
  }

  {
    TGAlloc kind;
    void *plainp, *travp;
    GCArena *plaina, *trava;
    lj_arena_alloc_init(&kind);
    plainp = lj_arena_alloc(&kind, &rs, 32, 0);
    travp = lj_arena_alloc(&kind, &rs, 32, LJ_AF_TRAVERSABLE);
    assert(plainp != NULL && travp != NULL);
    plaina = lj_arena_of(plainp);
    trava = lj_arena_of(travp);
    assert(kind.owned[LJ_ARENAK_PLAIN] == plaina);
    assert(kind.owned[LJ_ARENAK_TRAVERSABLE] == trava);
    lj_arena_alloc_prepare_sweep_kind(&kind, LJ_ARENAK_TRAVERSABLE);
    assert(kind.owned[LJ_ARENAK_PLAIN] == plaina);
    assert(kind.needsweep[LJ_ARENAK_PLAIN] == NULL);
    assert(kind.bump[LJ_ARENAK_PLAIN].a == plaina);
    assert(kind.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(kind.needsweep[LJ_ARENAK_TRAVERSABLE] == trava);
    lj_arena_alloc_restore_sweep_kind(&kind, LJ_ARENAK_TRAVERSABLE);
    assert(kind.owned[LJ_ARENAK_PLAIN] == plaina);
    assert(kind.needsweep[LJ_ARENAK_PLAIN] == NULL);
    assert(kind.owned[LJ_ARENAK_TRAVERSABLE] == trava);
    assert(kind.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert((trava->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
    lj_arena_alloc_prepare_sweep_kind(&kind, LJ_ARENAK_TRAVERSABLE);
    lj_arena_alloc_sweep_kind(&kind, LJ_ARENAK_TRAVERSABLE, 12, 0);
    assert(kind.owned[LJ_ARENAK_TRAVERSABLE] == trava);
    assert(kind.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(trava->hdr.sweep_epoch == 12);
    lj_arena_alloc_fini(&kind);
  }

  {
    TGAlloc bump;
    void *d1, *d2, *pad[4], *live, *wide;
    GCArena *a;
    uint32_t cd1, cd2, clive;
    uint32_t i;
    lj_arena_alloc_init(&bump);
    bump.alloc_black = 0;
    d1 = lj_arena_alloc(&bump, &rs, 8192, 0);
    d2 = lj_arena_alloc(&bump, &rs, 8192, 0);
    bump.alloc_black = 1;
    for (i = 0; i < 4; i++)
      pad[i] = lj_arena_alloc(&bump, &rs, 8000, 0);
    live = lj_arena_alloc(&bump, &rs, 16, 0);
    assert(d1 != NULL && d2 != NULL && live != NULL);
    a = lj_arena_of(d1);
    assert(lj_arena_of(d2) == a);
    for (i = 0; i < 4; i++) {
      assert(pad[i] != NULL);
      assert(lj_arena_of(pad[i]) == a);
    }
    assert(lj_arena_of(live) == a);
    cd1 = lj_arena_cellof(d1);
    cd2 = lj_arena_cellof(d2);
    clive = lj_arena_cellof(live);
    lj_arena_alloc_prepare_sweep(&bump);
    swept = lj_arena_sweep_one(&bump, LJ_ARENAK_PLAIN, 11, 0);
    assert(swept == a);
    assert(lj_arena_state(a, cd1) == 1);
    assert(lj_arena_state(a, cd2) == 0);
    assert(lj_arena_state(a, clive) == 2);
    assert(a->hdr.live_cells == 2001);
    assert(bump.bump[LJ_ARENAK_PLAIN].a == a);
    assert(bump.bump[LJ_ARENAK_PLAIN].cell == cd1);
    bump.alloc_black = 0;
    wide = lj_arena_alloc(&bump, &rs, 16384, 0);
    assert(wide == d1);
    assert(lj_arena_state(a, cd1) == 2);
    assert(lj_arena_state(a, cd2) == 0);
    lj_arena_alloc_fini(&bump);
  }

  {
    TGAlloc publish;
    void *first, *fill1, *fill2, *fill3, *second, *reuse, *expected_reuse;
    size_t quarter_arena =
      (size_t)((LJ_ARENA_CELLS - LJ_AFIRST_CELL) / 4u) << LJ_CELL_SHIFT;
    GCArena *a1, *a2, *swept1, *swept2;
    lj_arena_alloc_init(&publish);
    publish.alloc_black = 0;
    /* Keep four allocations in the first arena after the lifetime and
    ** destructor-kind sidecars increased LJ_AFIRST_CELL, while the fifth
    ** forces a second arena. */
    first = lj_arena_alloc(&publish, &rs, quarter_arena, 0);
    fill1 = lj_arena_alloc(&publish, &rs, quarter_arena, 0);
    fill2 = lj_arena_alloc(&publish, &rs, quarter_arena, 0);
    fill3 = lj_arena_alloc(&publish, &rs, quarter_arena, 0);
    second = lj_arena_alloc(&publish, &rs, quarter_arena, 0);
    assert(first != NULL && fill1 != NULL && fill2 != NULL &&
	   fill3 != NULL && second != NULL);
    a1 = lj_arena_of(first);
    assert(lj_arena_of(fill1) == a1);
    assert(lj_arena_of(fill2) == a1);
    assert(lj_arena_of(fill3) == a1);
    a2 = lj_arena_of(second);
    assert(a1 != a2);
    lj_arena_alloc_prepare_sweep_kind(&publish, LJ_ARENAK_PLAIN);
    assert_no_bins(&publish, LJ_ARENAK_PLAIN);
    swept1 = lj_arena_sweep_one(&publish, LJ_ARENAK_PLAIN, 21, 0);
    assert(swept1 == a1 || swept1 == a2);
    assert(publish.bump[LJ_ARENAK_PLAIN].a == swept1);
    assert(bin_count(&publish, LJ_ARENAK_PLAIN) == 0);
    swept2 = lj_arena_sweep_one(&publish, LJ_ARENAK_PLAIN, 21, 0);
    assert(swept2 == (swept1 == a1 ? a2 : a1));
    assert(publish.bump[LJ_ARENAK_PLAIN].a == swept2);
    assert(bin_count(&publish, LJ_ARENAK_PLAIN) >= 1);
    expected_reuse = swept2 == a1 ? first : second;
    reuse = lj_arena_alloc(&publish, &rs, 14400, 0);
    assert(reuse == expected_reuse);
    lj_arena_alloc_fini(&publish);
  }

  {
    TGAlloc dst, src;
    void *freep, *livep, *travp;
    GCArena *plaina, *trava;
    uint32_t n;
    lj_arena_alloc_init(&dst);
    lj_arena_alloc_init(&src);
    dst.owner_tid = 0x1001u;
    src.owner_tid = 0x2002u;
    freep = lj_arena_alloc(&src, &rs, 64, 0);
    livep = lj_arena_alloc(&src, &rs, 64, 0);
    travp = lj_arena_alloc(&src, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(freep != NULL && livep != NULL && travp != NULL);
    plaina = lj_arena_of(freep);
    trava = lj_arena_of(travp);
    assert(plaina->hdr.owner_tid == src.owner_tid);
    assert(trava->hdr.owner_tid == src.owner_tid);
    lj_arena_free(&src, freep, 64);
    assert(bin_count(&src, LJ_ARENAK_PLAIN) == 1);
    lj_arena_alloc_prepare_sweep_kind(&src, LJ_ARENAK_TRAVERSABLE);
    assert(src.needsweep[LJ_ARENAK_TRAVERSABLE] == trava);
    n = lj_arena_alloc_transfer(&dst, &src);
    assert(n == 2);
    assert(src.owned[LJ_ARENAK_PLAIN] == NULL);
    assert(src.needsweep[LJ_ARENAK_PLAIN] == NULL);
    assert(src.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(src.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(src.bump[LJ_ARENAK_PLAIN].a == NULL);
    assert(src.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
    assert_no_bins(&src, LJ_ARENAK_PLAIN);
    assert_no_bins(&src, LJ_ARENAK_TRAVERSABLE);
    assert(plaina->hdr.owner_tid == dst.owner_tid);
    assert(trava->hdr.owner_tid == dst.owner_tid);
    assert(dst.owned[LJ_ARENAK_PLAIN] == plaina);
    assert(dst.needsweep[LJ_ARENAK_TRAVERSABLE] == trava);
    assert((trava->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
    assert(bin_count(&dst, LJ_ARENAK_PLAIN) >= 1);
    assert(lj_arena_alloc(&dst, &rs, 64, 0) == freep);
    lj_arena_alloc_sweep_kind(&dst, LJ_ARENAK_TRAVERSABLE, 13, 0);
    assert(dst.owned[LJ_ARENAK_TRAVERSABLE] == trava);
    assert(dst.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert((trava->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
    assert(trava->hdr.sweep_epoch == 13);
    lj_arena_free(&dst, livep, 64);
    lj_arena_alloc_fini(&src);
    lj_arena_alloc_fini(&dst);
  }

  {
    TGAlloc late;
    void *p, *already_free, *same_cycle, *ordinary, *ordinary_reuse;
    void *next_cycle;
    GCArena *a;
    uint32_t cell, freecell, samecell, ordinarycell;
    lj_arena_alloc_init(&late);
    late.alloc_black = 1;
    p = lj_arena_alloc(&late, &rs, 64, LJ_AF_TRAVERSABLE);
    already_free = lj_arena_alloc(&late, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(p != NULL && already_free != NULL);
    a = lj_arena_of(p);
    assert(lj_arena_of(already_free) == a);
    cell = lj_arena_cellof(p);
    freecell = lj_arena_cellof(already_free);
    assert(lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_lifetime_state_acq(a, freecell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    lj_arena_alloc_prepare_sweep_kind(&late, LJ_ARENAK_TRAVERSABLE);
    assert(lj_arena_alloc_quarantine_one(&late, LJ_ARENAK_TRAVERSABLE,
					  11u) == a);
    assert(lj_arena_sweep_state_cas(a, freecell, LJ_ARENA_SWEEP_WHITE,
					    LJ_ARENA_SWEEP_FREEING));
    assert(lj_arena_reclaim_seal(a));
    assert(lj_arena_alloc_quarantine_finish(&late,
	LJ_ARENAK_TRAVERSABLE, a, 31u, 0, NULL));
    assert((a->hdr.flags & LJ_AF_RECLAIMED) != 0);
    assert(lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    assert(lj_arena_lifetime_state_acq(a, freecell) ==
	   LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_remote_active_acq(a) != 0);  /* CLOSED admission gate. */

    /* The physical free loses the CLOSED/open race, so it may publish only a
    ** bit-only late pin. The still-SMR-visible body remains untouched. */
    assert(lj_arena_quarantine_owns_body(p, 64));
    /* A duplicate physical free for an already-free committed cell is legal
    ** too and does not need a pin. */
    assert(lj_arena_quarantine_owns_body(already_free, 64));
    assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
    assert((la_load64_acq(&a->late[cell >> 6]) &
	    ((uint64_t)1 << (cell & 63))) != 0);
    assert((la_load64_acq(&a->late[freecell >> 6]) &
	    ((uint64_t)1 << (freecell & 63))) == 0);
    assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);

    late.alloc_black = 0;
    same_cycle = lj_arena_alloc(&late, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(same_cycle == already_free);
    samecell = lj_arena_cellof(same_cycle);
    assert(lj_arena_lifetime_state_acq(a, samecell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    assert(same_cycle != p);  /* No reuse without a fresh grace. */
    assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
    assert((la_load64_acq(&a->late[cell >> 6]) &
	    ((uint64_t)1 << (cell & 63))) != 0);
    assert((la_load64_acq(&a->late[freecell >> 6]) &
	    ((uint64_t)1 << (freecell & 63))) == 0);
    assert(lj_arena_bm_get(a->block, cell));
    assert(lj_arena_remote_active_acq(a) == 0);
    assert((a->hdr.flags & LJ_AF_RECLAIMED) == 0);

    /* A duplicate ordinary publication for the retained body must observe the
    ** stable late bit before touching sweep state or the intrusive link. */
    assert(((LJArenaRemoteFree *)p)->next == NULL);
    assert(lj_arena_remote_free_publish(&late, p, 64));
    assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
    assert(((LJArenaRemoteFree *)p)->next == NULL);
    assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);

    /* A normal OPEN remote record still uses the intrusive owner queue and is
    ** immediately reusable; the unrelated bit-only late pin remains intact. */
    ordinary = lj_arena_alloc(&late, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(ordinary != NULL && lj_arena_of(ordinary) == a && ordinary != p);
    assert(lj_arena_remote_free_publish(&late, ordinary, 64));
    assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(ordinary)) ==
	   LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_remote_free_drain(&late) == 1u);
    assert(lj_arena_lifetime_state_acq(a, lj_arena_cellof(ordinary)) ==
	   LJ_ARENA_LIFETIME_FREE);
    assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
    assert((la_load64_acq(&a->late[cell >> 6]) &
	    ((uint64_t)1 << (cell & 63))) != 0);
    assert(lj_arena_bm_get(a->block, cell));
    late.bump[LJ_ARENAK_TRAVERSABLE].cell =
      late.bump[LJ_ARENAK_TRAVERSABLE].end;
    ordinary_reuse = lj_arena_alloc(&late, &rs, 64,
	LJ_AF_TRAVERSABLE);
    assert(ordinary_reuse == ordinary);
    ordinarycell = lj_arena_cellof(ordinary_reuse);
    assert(lj_arena_lifetime_state_acq(a, ordinarycell) ==
	   LJ_ARENA_LIFETIME_LIVE);

    /* The next cycle consumes the deferred ticket only after sweep[] has
    ** reset. Root pruning can now detach a remaining gct=0 ticket and this
    ** cycle's quarantine grace covers conversion to reusable bitmap space. */
    lj_arena_alloc_prepare_sweep_kind(&late, LJ_ARENAK_TRAVERSABLE);
    assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
    assert((la_load64_acq(&a->late[cell >> 6]) &
	    ((uint64_t)1 << (cell & 63))) != 0);
    assert(lj_arena_sweep_state_acq(a, cell) ==
	   LJ_ARENA_SWEEP_FREEING);
    assert(lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_FREE);
    /* Stand in for classification of the other live allocation; the deferred
    ** body itself must stay FREEING through this cycle's quarantine. */
    lj_arena_bm_set(a->mark, samecell);
    lj_arena_bm_set(a->mark, ordinarycell);
    assert(lj_arena_alloc_quarantine_one(&late,
	LJ_ARENAK_TRAVERSABLE, 41u) == a);
    assert(lj_arena_reclaim_seal(a));
    assert(lj_arena_alloc_quarantine_finish(&late,
	LJ_ARENAK_TRAVERSABLE, a, 51u, 0, NULL));
    assert(!lj_arena_bm_get(a->block, cell));
    assert(!lj_arena_late_get(a, cell));
    assert(lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_FREE);
    next_cycle = lj_arena_alloc(&late, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(next_cycle == p);  /* Reuse is legal after the fresh cycle grace. */
    assert(lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_LIVE);
    lj_arena_free(&late, next_cycle, 64);
    lj_arena_free(&late, same_cycle, 64);
    lj_arena_free(&late, ordinary_reuse, 64);
    lj_arena_alloc_fini(&late);
  }

  {
    TGAlloc recovery;
    LJArenaAllocD recovery_ad;
    GCArena *a, *raw;
    void *p, *allocfp, *deferredp;
    uint32_t cell;
    lj_arena_alloc_init(&recovery);
    lj_arena_allocd_init(&recovery_ad, &recovery, &rs,
			 LJ_AF_TRAVERSABLE);
    recovery.alloc_black = 0;
    p = lj_arena_alloc(&recovery, &rs, 64, LJ_AF_TRAVERSABLE);
    assert(p != NULL);
    a = lj_arena_of(p);
    cell = lj_arena_cellof(p);
    assert(lj_arena_recovery_empty(a));
    assert(lj_arena_recovery_state_acq(a, cell) ==
	   LJ_ARENA_RECOVERY_IDLE);
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING));
    assert(!lj_arena_recovery_empty(a));

    /* A logical free is retained and durably remembered without overwriting
    ** the object body. Recovery also keeps a carried mark-zero allocation out
    ** of sweep free-run reconstruction. */
    lj_arena_free(&recovery, p, 64);
    assert(lj_arena_bm_get(a->block, cell));
    assert(lj_arena_late_get(a, cell));
    lj_arena_sweep_words(a, 0);
    assert(lj_arena_bm_get(a->block, cell));
    lj_arena_alloc_rebuild_free(&recovery);
    assert(lj_arena_bm_get(a->block, cell));

    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_CLAIMED, LJ_ARENA_RECOVERY_REDIRTY));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_REDIRTY, LJ_ARENA_RECOVERY_PENDING));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_CLAIMED, LJ_ARENA_RECOVERY_IDLE));
    assert(lj_arena_recovery_empty(a));
    /* This fixture owns both sides of the deferred-free protocol. Clear the
    ** observed late ticket, then perform its modeled owner consumption. */
    lj_arena_bm_clear(a->late, cell);
    lj_arena_free(&recovery, p, 64);

    /* Both public free funnels consult quarantine ownership before their
    ** direct recovery checks. That early owned return must still remember the
    ** logical free in late[] so completion cannot strand the allocation. */
    allocfp = lj_arena_allocf(&recovery_ad, NULL, 0, 64);
    assert(allocfp != NULL);
    a = lj_arena_of(allocfp);
    cell = lj_arena_cellof(allocfp);
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING));
    assert(lj_arena_allocf(&recovery_ad, allocfp, 64, 0) == NULL);
    assert(lj_arena_late_get(a, cell));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_IDLE));
    lj_arena_bm_clear(a->late, cell);
    lj_arena_free(&recovery, allocfp, 64);

    deferredp = lj_arena_alloc(&recovery, &rs, 64,
			       LJ_AF_TRAVERSABLE);
    assert(deferredp != NULL);
    a = lj_arena_of(deferredp);
    cell = lj_arena_cellof(deferredp);
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING));
    assert(lj_arena_free_deferred(&recovery, deferredp, 64));
    assert(lj_arena_late_get(a, cell));
    assert(lj_arena_recovery_state_cas(a, cell,
	   LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_IDLE));
    lj_arena_bm_clear(a->late, cell);
    lj_arena_free(&recovery, deferredp, 64);
    lj_arena_alloc_fini(&recovery);

    /* Even the direct mapping teardown helper must retain a non-IDLE plane. */
    raw = lj_arena_map(&rs, LJ_AF_TRAVERSABLE);
    assert(raw != NULL);
    assert(lj_arena_recovery_state_cas(raw, LJ_AFIRST_CELL,
	   LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING));
    lj_arena_unmap(raw);
    assert(lj_arena_recovery_state_acq(raw, LJ_AFIRST_CELL) ==
	   LJ_ARENA_RECOVERY_PENDING);
    assert(lj_arena_recovery_state_cas(raw, LJ_AFIRST_CELL,
	   LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_IDLE));
    /* A destructor kind is allocation authority even when every lifetime lane
    ** is FREE. Direct unmap must retain malformed/stale authority instead of
    ** erasing the only information which prevents unsafe address reuse. */
    lj_arena_bm_set(raw->dtor[0], LJ_AFIRST_CELL);
    lj_arena_unmap(raw);
    assert(lj_arena_dtor_kind_acq(raw, LJ_AFIRST_CELL) ==
	   LJ_ARENA_DTOR_LFUNC1);
    lj_arena_bm_clear(raw->dtor[0], LJ_AFIRST_CELL);
    lj_arena_unmap(raw);
  }

  printf("t-arena-sweep OK: owner-local sweep rebuild verified\n");
  return 0;
}
