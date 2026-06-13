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

int main(void)
{
  PRNGState rs;
  TGAlloc alloc;
  void *dead1, *live1, *oldfree, *live2, *taildead;
  void *tdead, *tlive;
  GCArena *plain, *trav, *swept;
  uint32_t cdead1, clive1, coldfree, clive2, ctaildead;
  uint32_t ctdead, ctlive;

  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);

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
  assert(lj_arena_alloc(&alloc, &rs, 64, 0) == dead1);
  assert(lj_arena_alloc(&alloc, &rs, 32, 0) == oldfree);

  swept = lj_arena_sweep_one(&alloc, LJ_ARENAK_TRAVERSABLE, 9, 1);
  assert(swept == trav);
  assert(alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == trav);
  assert((trav->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(trav->hdr.sweep_epoch == 9);
  assert(trav->hdr.live_cells == 2);
  assert(lj_arena_state(trav, ctdead) == 1);
  assert(lj_arena_state(trav, ctlive) == 3);
  assert(bin_count(&alloc, LJ_ARENAK_TRAVERSABLE) == 1);
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
    big = lj_arena_alloc(&rebuild, &rs, 96, 0);
    assert(big == r1);
    lj_arena_free(&rebuild, r3, 16);
    lj_arena_alloc_rebuild_free(&rebuild);
    tail = lj_arena_alloc(&rebuild, &rs, 64, 0);
    assert(tail != r3);
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

  printf("t-arena-sweep OK: owner-local sweep rebuild verified\n");
  return 0;
}
