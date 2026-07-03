/*
** Focused test for arena free-run reuse and realloc scaffolding.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static void fill_seq(uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    p[i] = (uint8_t)(base + i);
}

static void check_seq(const uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    assert(p[i] == (uint8_t)(base + i));
}

int main(void)
{
  PRNGState rs;
  TGAlloc alloc, alloc2;
  void *p1, *p2, *p3, *p4, *p5, *p6, *hp, *hp2, *hp3;
  void *q1, *q2, *q3;
  GCArena *a;
  uint32_t c1, c3, c4, c5, qmask;
  size_t hsize = LJ_HUGE_THRESHOLD + 100u;
  size_t hsize2 = (size_t)LJ_ARENA_SIZE * 2u + 257u;

  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  lj_arena_alloc_init(&alloc2);

  q1 = lj_arena_alloc(&alloc2, &rs, 32, 0);
  q2 = lj_arena_alloc(&alloc2, &rs, 32, 0);
  assert(q1 != NULL && q2 != NULL);
  assert(alloc2.binmask[LJ_ARENAK_PLAIN] == 0);
  lj_arena_free(&alloc2, q1, 32);
  qmask = (uint32_t)1u << lj_arena_bin_from_ncells(lj_arena_ncells(32));
  assert(alloc2.binmask[LJ_ARENAK_PLAIN] == qmask);
  assert(lj_arena_alloc_has_run_ge(&alloc2, LJ_ARENAK_PLAIN,
				   lj_arena_ncells(32)));
  assert(!lj_arena_alloc_has_run_ge(&alloc2, LJ_ARENAK_PLAIN,
				    lj_arena_ncells(48)));
  q3 = lj_arena_alloc(&alloc2, &rs, 32, 0);
  assert(q3 == q1);
  assert(alloc2.binmask[LJ_ARENAK_PLAIN] == 0);
  lj_arena_free(&alloc2, q2, 32);
  lj_arena_free(&alloc2, q3, 32);
  lj_arena_alloc_fini(&alloc2);

  p1 = lj_arena_alloc(&alloc, &rs, 64, 0);
  p2 = lj_arena_alloc(&alloc, &rs, 32, 0);
  assert(p1 != NULL && p2 != NULL);
  a = lj_arena_of(p1);
  c1 = lj_arena_cellof(p1);
  assert(lj_arena_cellof(p2) == c1 + lj_arena_ncells(64));
  fill_seq((uint8_t *)p1, 64, 0x20);

  lj_arena_free(&alloc, p1, 64);
  assert(lj_arena_state(a, c1) == 1);
  assert(lj_arena_count_free_runs(a) == 1);

  p3 = lj_arena_alloc(&alloc, &rs, 32, 0);
  assert(p3 == p1);
  c3 = lj_arena_cellof(p3);
  assert(lj_arena_state(a, c3) == 2);
  assert(lj_arena_state(a, c3 + lj_arena_ncells(32)) == 1);

  p4 = lj_arena_alloc(&alloc, &rs, 16, 0);
  c4 = lj_arena_cellof(p4);
  assert(c4 == c3 + lj_arena_ncells(32));
  assert(lj_arena_state(a, c4) == 2);
  assert(lj_arena_state(a, c4 + lj_arena_ncells(16)) == 1);

  fill_seq((uint8_t *)p3, 32, 0x80);
  p5 = lj_arena_realloc(&alloc, &rs, p3, 32, 96, 0);
  assert(p5 != NULL && p5 != p3);
  c5 = lj_arena_cellof(p5);
  assert(c5 > lj_arena_cellof(p2));
  check_seq((uint8_t *)p5, 32, 0x80);
  assert(lj_arena_state(a, c3) == 1);

  p5 = lj_arena_realloc(&alloc, &rs, p5, 96, 16, 0);
  assert(p5 != NULL);
  check_seq((uint8_t *)p5, 16, 0x80);
  assert(lj_arena_state(lj_arena_of(p5),
			lj_arena_cellof(p5) + lj_arena_ncells(16)) == 1);

  lj_arena_free(&alloc, p2, 32);
  lj_arena_free(&alloc, p4, 16);
  lj_arena_free(&alloc, p5, 16);
  p6 = lj_arena_alloc(&alloc, &rs, 16, 0);
  assert(p6 != NULL);
  assert(lj_arena_realloc(&alloc, &rs, p6, 16, 0, 0) == NULL);

  hp = lj_arena_alloc(&alloc, &rs, hsize, LJ_AF_TRAVERSABLE);
  assert(hp != NULL);
  assert(lj_arena_ishuge(lj_arena_of(hp)));
  assert((lj_arena_of(hp)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  fill_seq((uint8_t *)hp, 128, 0x11);

  hp2 = lj_arena_realloc(&alloc, &rs, hp, hsize, hsize2, LJ_AF_TRAVERSABLE);
  assert(hp2 != NULL);
  assert(lj_arena_ishuge(lj_arena_of(hp2)));
  check_seq((uint8_t *)hp2, 128, 0x11);

  hp3 = lj_arena_realloc(&alloc, &rs, hp2, hsize2, 128, 0);
  assert(hp3 != NULL);
  assert(!lj_arena_ishuge(lj_arena_of(hp3)));
  check_seq((uint8_t *)hp3, 128, 0x11);
  lj_arena_free(&alloc, hp3, 128);

  lj_arena_alloc_fini(&alloc);
  printf("t-arena-realloc OK: free-run reuse and realloc verified\n");
  return 0;
}
