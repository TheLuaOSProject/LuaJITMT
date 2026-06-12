/*
** Arena heap bitmap scaffolding.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_arena_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_arena.h"

/* Apply the 04_allocator.md sweep identities over the arena bitmaps. */
void lj_arena_sweep_words(GCArena *a, int minor)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = a->block[w];
    uint64_t m = a->mark[w];
    a->block[w] = b & m;
    a->mark[w] = minor ? (b | m) : (b ^ m);
  }
}

void lj_arena_scan_free_runs(const GCArena *a, LJArenaRunCB cb, void *ud)
{
  int32_t run_start = -1;
  uint32_t i = LJ_AFIRST_CELL;
  while (i < LJ_ARENA_CELLS) {
    uint64_t starts = (a->block[i >> 6] | a->mark[i >> 6]) >> (i & 63);
    uint32_t st;
    if (!starts) {
      i = (i | 63u) + 1u;
      continue;
    }
    i += (uint32_t)__builtin_ctzll(starts);
    if (i >= LJ_ARENA_CELLS)
      break;
    st = lj_arena_state(a, i);
    if (st == 1) {
      if (run_start < 0)
	run_start = (int32_t)i;
    } else if (run_start >= 0) {
      cb((uint32_t)run_start, i - (uint32_t)run_start, ud);
      run_start = -1;
    }
    i++;
  }
  if (run_start >= 0)
    cb((uint32_t)run_start, LJ_ARENA_CELLS - (uint32_t)run_start, ud);
}

static void arena_count_run(uint32_t start, uint32_t len, void *ud)
{
  uint32_t *count = (uint32_t *)ud;
  UNUSED(start);
  UNUSED(len);
  (*count)++;
}

uint32_t lj_arena_count_free_runs(const GCArena *a)
{
  uint32_t count = 0;
  lj_arena_scan_free_runs(a, arena_count_run, &count);
  return count;
}
