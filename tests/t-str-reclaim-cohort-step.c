/* A source pin delays its own VM string body after a real cohort root grace. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_str.h"
#include "lj_tg.h"

static GCstr *new_huge(lua_State *L, char fill)
{
  char bytes[LJ_HUGE_THRESHOLD * 2u];
  GCstr *s;
  memset(bytes, fill, sizeof(bytes));
  lua_pushlstring(L, bytes, sizeof(bytes));
  s = strV(L->top - 1);
  return s;
}

static int mapped(lua_State *L, GCstr *s)
{
  LJHugeInfo info;
  return lj_arena_hugetab_lookup(&L2TG(L)->huge, s, &info);
}

static void collect_major(lua_State *L)
{
  unsigned pass;
  global_State *g = G(L);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_force_major(g);
  for (pass = 0; pass < 10000; pass++)
    if (lua_gc(L, LUA_GCSTEP, 64))
      break;
  assert(pass < 10000 && gc2_phase_acq(g) == LJ_GC2_IDLE);
  lua_gc(L, LUA_GCSTOP, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCstr *first, *next;
  TGState *reader;
  StrCanonRec *first_record;
  uint64_t first_close, prior_grace;
  unsigned round;
  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);

  first = new_huge(L, 'A');
  assert(mapped(L, first));
  reader = lj_str_source_pin(L, first);
  assert(reader);

  lua_pop(L, 1);
  collect_major(L);
  assert(mapped(L, first));
  assert(lj_str_canon_state(lj_str_canon_acq(first)) == LJ_STR_CANON_QCLOSING);
  assert(lj_str_qcount_acq(g) == 1);
  first_record = lj_str_canon_record(lj_str_canon_acq(first));
  first_close = la_load64_acq(&first_record->close_epoch);
  for (round = 0; round < 4 &&
       gc2_idle_retire_epoch_acq(g) < first_close + LJ_GC2_GRACE_EPOCHS; round++)
    collect_major(L);
  prior_grace = gc2_idle_retire_epoch_acq(g);
  assert(prior_grace >= first_close + LJ_GC2_GRACE_EPOCHS);
  assert(la_load64_acq(&reader->str_owner_scan_epoch) > first_close);

  next = new_huge(L, 'B');
  assert(mapped(L, next));
  lua_pop(L, 1);
  for (round = 0; round < 4 && mapped(L, next); round++)
    collect_major(L);
  assert(mapped(L, first));
  assert(lj_str_qcount_acq(g) == 1);
  assert(!mapped(L, next));
  assert(gc2_idle_retire_epoch_acq(g) > prior_grace);
  assert(lj_str_canon_record(lj_str_canon_acq(first)) == first_record);
  for (size_t i = 0; i < (size_t)LJ_HUGE_THRESHOLD * 2u; i++)
    assert(strdata(first)[i] == 'A');

  lj_str_source_unpin(reader);
  for (round = 0; round < 4 && mapped(L, first); round++)
    collect_major(L);
  assert(!mapped(L, first));
  lua_close(L);
  puts("t-str-reclaim-cohort-step OK");
  return 0;
}
