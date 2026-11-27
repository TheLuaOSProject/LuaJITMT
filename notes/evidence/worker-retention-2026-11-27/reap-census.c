#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

typedef struct Counts {
  uint64_t tgs, arenas, empty, blocks, late, actors, queued;
} Counts;

static void field(lua_State *L, const char *name, uint64_t value)
{
  lua_pushnumber(L, (lua_Number)value);
  lua_setfield(L, -2, name);
}

static int census(lua_State *L)
{
  global_State *g = G(L);
  Counts live = {0}, dead = {0};
  TGState *tg;
  uint64_t visited = 0, retired_pending, dead_trav = 0, dead_reclaimed = 0, pending = 0, unlinked = 0, ack = 0;
  if (!lj_gc2_smr_read_try(g)) return 0;
  for (tg = gc2_tg_list_acq(g); tg; tg = lj_tg_next_acq(tg)) {
    Counts *c = lj_tg_flags_test_acq(tg, TGF_DEAD) ? &dead : &live;
    GCArena *a;
    assert(++visited < 1000000);
    c->tgs++;
    if (c == &dead) {
      dead_trav += lj_arena_alloc_owned_count_acq(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
      dead_reclaimed += !!lj_arena_alloc_reclaimed_head(&tg->alloc, LJ_ARENAK_TRAVERSABLE) +
        !!lj_arena_alloc_empty_reclaimed_head(&tg->alloc);
    } else if (lj_tg_flags_test_acq(tg, TGF_LUA_ALLOC)) {
      ack = la_load64_acq(&tg->dead_tg_epoch);
    }
    if (!lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) continue;
    a = (GCArena *)la_loadptr_acq(
      (void *const *)&tg->alloc.owned[LJ_ARENAK_PLAIN]);
    for (; a; a = lj_arena_next_acq(a)) {
      uint32_t w;
      uint64_t blocks = 0;
      assert(++c->arenas < 1000000);
      assert(!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE));
      for (w = 0; w < LJ_ARENA_WORDS; w++) {
        blocks += __builtin_popcountll(la_load64_acq(&a->block[w]));
        c->late += __builtin_popcountll(la_load64_acq(&a->late[w]));
      }
      c->blocks += blocks;
      c->empty += blocks == 0;
      c->actors += lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_COUNT_MASK;
      c->queued += la_loadptr_acq((void *const *)&a->hdr.remote_free) != NULL;
    }
  }
  retired_pending = gc2_finreg_udata_retired_acq(g) != NULL;
  unlinked = la_load64_acq(&g->main_tg->dead_tg_epoch);
  for (tg = g->main_tg->dead_tg_head; tg; tg = lj_tg_worker_retire_next_acq(tg))
    pending++;
  lj_gc2_smr_read_leave(g);
  lua_createtable(L, 0, 14);
#define FIELD(side, key) field(L, #side "_" #key, side.key)
  FIELD(live, tgs); FIELD(live, arenas); FIELD(live, empty);
  FIELD(live, blocks); FIELD(live, late); FIELD(live, actors); FIELD(live, queued);
  FIELD(dead, tgs); FIELD(dead, arenas); FIELD(dead, empty);
  FIELD(dead, blocks); FIELD(dead, late); FIELD(dead, actors); FIELD(dead, queued);
#undef FIELD
  field(L, "retired_pending", retired_pending);
  field(L, "dead_trav", dead_trav);
  field(L, "dead_reclaimed", dead_reclaimed);
  field(L, "pending", pending);
  field(L, "unlinked_epoch", unlinked);
  field(L, "ack", ack);
  return 1;
}

int main(int argc, char **argv)
{
  lua_State *L;
  assert(argc == 2);
  alarm(120);
  L = ljt_lua_newstate_openlibs();
  lua_register(L, "plain_census", census);
  ljt_lua_assert_ok(L, luaL_loadfile(L, argv[1]), "load census workload");
  ljt_lua_pcall(L, 0, 0, "worker plain census");
  lua_close(L);
  puts("closed");
  return 0;
}
