#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

static int census(lua_State *L)
{
  global_State *g = G(L);
  TGState *owner = L2TG(L);
  TGAlloc *alloc = &owner->alloc;
  const uint32_t kind = LJ_ARENAK_PLAIN;
  const char *stage = luaL_checkstring(L, 1);
  int wave = luaL_checkinteger(L, 2);
  uint32_t mapped = 0, empty = 0, occupied = 0, late = 0;
  uint32_t empty_bump = 0, uncounted_empty = 0;
  uint32_t runs = 0, max_run = 0;
  uint32_t b;
  GCArena *a;
  assert(owner == g->main_tg);
  for (a = alloc->owned[kind]; a; a = lj_arena_next_acq(a)) {
    uint32_t w, blocks = 0, deferred = 0;
    assert(lj_arena_owner_acq(a) == lj_tg_tid_acq(owner));
    assert((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) == 0);
    for (w = 0; w < LJ_ARENA_WORDS; w++) {
      blocks += __builtin_popcountll(la_load64_acq(&a->block[w]));
      deferred += __builtin_popcountll(la_load64_acq(&a->late[w]));
    }
    occupied += blocks;
    late += deferred;
    mapped++;
    if (blocks == 0 && deferred == 0) {
      empty++;
      empty_bump += alloc->bump[kind].a == a;
      uncounted_empty += !lj_arena_remote_active_acq(a);
    }
  }
  assert(mapped == lj_arena_alloc_owned_count_acq(alloc, kind));
  for (b = 0; b < sizeof(alloc->bins[kind]) / sizeof(alloc->bins[kind][0]); b++) {
    const LJArenaFreeRun *r;
    for (r = alloc->bins[kind][b]; r; r = r->next) {
      assert(++runs != 10000000);
      if (r->len > max_run) max_run = r->len;
    }
  }
  printf("%s,%d,%u,%u,%u,%u,%u,%u,%u,%u,%" PRIu64 ",%u\n", stage, wave,
    mapped, empty, uncounted_empty, empty_bump, occupied, late, runs,
    max_run, gc2_sweep_to_idle_acq(g), mt_live_acq(g));
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  assert(argc == 2);
  alarm(180);
  lua_register(L, "plain_census", census);
  puts("stage,wave,mapped,fully_empty,empty_ungated,empty_bump,occupied,late,bin_runs,max_run_cells,sweeps,secondary");
  ljt_lua_assert_ok(L, luaL_loadfile(L, argv[1]), "load live plain census");
  ljt_lua_pcall(L, 0, 0, "live plain census");
  lua_close(L);
  return 0;
}
