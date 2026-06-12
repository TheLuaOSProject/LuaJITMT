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
  assert(plain->hdr.live_cells == 2);
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
  assert(trav->hdr.live_cells == 1);
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

  printf("t-arena-sweep OK: owner-local sweep rebuild verified\n");
  return 0;
}
