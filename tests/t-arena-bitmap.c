/*
** Focused test for the arena bitmap scaffold.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lj_arena.h"

enum {
  S_EXT = 0,
  S_FREE = 1,
  S_WHITE = 2,
  S_BLACK = 3
};

struct runlist {
  uint32_t n;
  uint32_t start[LJ_ARENA_CELLS];
  uint32_t len[LJ_ARENA_CELLS];
};

static uint64_t rngs = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void)
{
  rngs ^= rngs << 13;
  rngs ^= rngs >> 7;
  rngs ^= rngs << 17;
  return rngs;
}

static void bm_set(uint64_t *bm, uint32_t i)
{
  bm[i >> 6] |= (uint64_t)1 << (i & 63);
}

static void encode(GCArena *a, const uint8_t *shadow)
{
  uint32_t i;
  memset(a, 0, sizeof(*a));
  for (i = 0; i < LJ_ARENA_CELLS; i++) {
    if (shadow[i] & 2) bm_set(a->block, i);
    if (shadow[i] & 1) bm_set(a->mark, i);
  }
}

static void sweep_shadow(uint8_t *s, int minor)
{
  uint32_t i = LJ_AFIRST_CELL;
  while (i < LJ_ARENA_CELLS) {
    uint32_t len = 1;
    int st = s[i];
    while (i+len < LJ_ARENA_CELLS && s[i+len] == S_EXT)
      len++;
    if (st == S_WHITE)
      s[i] = S_FREE;
    else if (st == S_BLACK)
      s[i] = minor ? S_BLACK : S_WHITE;
    i += len;
  }
}

static void collect(uint32_t start, uint32_t len, void *ud)
{
  struct runlist *r = (struct runlist *)ud;
  r->start[r->n] = start;
  r->len[r->n] = len;
  r->n++;
}

static void scan_free_runs_ref(const uint8_t *s, LJArenaRunCB cb, void *ud)
{
  int32_t run_start = -1;
  uint32_t i = LJ_AFIRST_CELL;
  while (i < LJ_ARENA_CELLS) {
    uint32_t len = 1;
    while (i+len < LJ_ARENA_CELLS && s[i+len] == S_EXT)
      len++;
    if (s[i] == S_FREE) {
      if (run_start < 0)
	run_start = (int32_t)i;
    } else if (run_start >= 0) {
      cb((uint32_t)run_start, i - (uint32_t)run_start, ud);
      run_start = -1;
    }
    i += len;
  }
  if (run_start >= 0)
    cb((uint32_t)run_start, LJ_ARENA_CELLS - (uint32_t)run_start, ud);
}

int main(void)
{
  uint8_t shadow[LJ_ARENA_CELLS], shadow2[LJ_ARENA_CELLS];
  GCArena arena;
  int cycles = 20000;
  int total_runs = 0;
  int c;

  for (c = 0; c < cycles; c++) {
    uint32_t i, k;
    int minor;
    struct runlist a, b;
    memset(shadow, S_EXT, sizeof(shadow));
    i = LJ_AFIRST_CELL;
    while (i < LJ_ARENA_CELLS) {
      uint32_t len = 1u + (uint32_t)(rnd() % 16u);
      uint32_t kind = (uint32_t)(rnd() % 10u);
      if (i+len > LJ_ARENA_CELLS)
	len = LJ_ARENA_CELLS - i;
      shadow[i] = (uint8_t)((kind < 2) ? S_FREE : (kind < 8) ? S_WHITE : S_BLACK);
      for (k = 1; k < len; k++)
	shadow[i+k] = S_EXT;
      i += len;
    }

    encode(&arena, shadow);
    for (k = LJ_AFIRST_CELL; k < LJ_ARENA_CELLS; k++) {
      int is_start = shadow[k] != S_EXT;
      assert(lj_arena_state(&arena, k) == shadow[k]);
      assert(((lj_arena_bm_get(arena.block, k) |
	       lj_arena_bm_get(arena.mark, k)) != 0) == is_start);
    }

    for (k = LJ_AFIRST_CELL; k < LJ_ARENA_CELLS; k++) {
      if (shadow[k] == S_WHITE && (rnd() & 3u) == 0) {
	shadow[k] = S_BLACK;
	bm_set(arena.mark, k);
      }
    }

    minor = (int)(rnd() & 1u);
    memcpy(shadow2, shadow, sizeof(shadow));
    lj_arena_sweep_words(&arena, minor);
    sweep_shadow(shadow2, minor);
    for (k = LJ_AFIRST_CELL; k < LJ_ARENA_CELLS; k++)
      assert(lj_arena_state(&arena, k) == shadow2[k]);

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    lj_arena_scan_free_runs(&arena, collect, &a);
    scan_free_runs_ref(shadow2, collect, &b);
    assert(a.n == b.n);
    for (k = 0; k < a.n; k++) {
      assert(a.start[k] == b.start[k]);
      assert(a.len[k] == b.len[k]);
      assert(a.len[k] >= 1);
    }
    assert(lj_arena_count_free_runs(&arena) == a.n);
    total_runs += (int)a.n;
  }

  printf("t-arena-bitmap OK: %d cycles, %d free runs verified\n",
	 cycles, total_runs);
  return 0;
}
