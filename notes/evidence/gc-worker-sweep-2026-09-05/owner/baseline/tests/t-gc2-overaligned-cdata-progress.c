/* Bounded GC2 progress for typed HugeTab allocation bodies. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_cdata.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

static HugeTab *hugetab_for(global_State *g, void *base, LJHugeInfo *hi)
{
  TGState *tg;
  tg = lj_tg_find_owner(g, lj_arena_owner_acq(lj_arena_of(base)));
  assert(tg != NULL && lj_tg_flags_test_acq(tg, TGF_HUGETAB));
  assert(lj_arena_hugetab_lookup(&tg->huge, base, hi) == 1);
  return &tg->huge;
}

static void collect_dead_huge_stack_object(lua_State *L, global_State *g,
					   HugeTab *ht, void *base,
					   const char *label)
{
  LJHugeInfo hi;
  GC2StatsSnapshot before, after;
  uint32_t i;
  int done = 0;

  setnilV(L->top - 1);
  lua_settop(L, 0);

  lj_gc2_stats_snapshot(g, &before);
  (void)lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < 4096u; i++) {
    if (lua_gc(L, LUA_GCSTEP, 1) > 0) {
      done = 1;
      break;
    }
  }
  lj_gc2_stats_snapshot(g, &after);
  if (!done) {
    fprintf(stderr,
      "%s GC stalled: phase=%u steps=%u cycles=%llu "
      "worker_runs=%llu sweep_runs=%llu sweep_arenas=%llu "
      "recovery_items=%llu recovery_failed=%u\n",
      label, after.phase, i,
      (unsigned long long)after.cycle_starts,
      (unsigned long long)after.worker_runs,
      (unsigned long long)after.sweep_owner_runs,
      (unsigned long long)after.sweep_owner_arenas,
      (unsigned long long)after.recovery_items,
      after.recovery_failed);
    if (lj_arena_hugetab_lookup(ht, base, &hi) == 1) {
      GCArena *a = lj_arena_of(base);
      GCobj *retire_obj = (GCobj *)
        la_loadptr_acq((void *const *)&a->hdr.retire_obj);
      fprintf(stderr,
        "  huge=%p size=%zu flags=%x readers=%u retire_obj=%p "
        "retire_epoch=%llu\n",
        base, hi.size, hi.flags, hi.readers, (void *)retire_obj,
        (unsigned long long)la_load64_acq(&a->hdr.retire_epoch));
    }
  }
  assert(done);
  assert(after.cycle_starts > before.cycle_starts);
  assert(lj_arena_hugetab_lookup(ht, base, NULL) == 0);
}

static void test_overaligned_cdata(lua_State *L, global_State *g)
{
  GCcdata *cd;
  HugeTab *ht;
  void *base = NULL;
  GCSize size = 0;
  LJHugeInfo hi;

  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = ffi.typeof([[struct {\n"
    "  char __attribute__((aligned(8192))) a;\n"
    "}]])\n"
    "local cd = ct()\n"
    "assert(tonumber(ffi.cast('intptr_t', ffi.cast('void *', cd))) % "
      "8192 == 0)\n"
    "return cd\n");
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  assert(lj_cdata_validate(g, cd, &base, &size));
  assert(size > LJ_HUGE_THRESHOLD && base != (void *)cd);
  ht = hugetab_for(g, base, &hi);
  assert((hi.flags & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA|
		      LJ_HUGEF_READY)) ==
	 (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA|LJ_HUGEF_READY));
  collect_dead_huge_stack_object(L, g, ht, base, "overaligned cdata");
}

static void test_huge_userdata(lua_State *L, global_State *g)
{
  GCudata *ud;
  HugeTab *ht;
  LJHugeInfo hi;

  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  (void)lua_newuserdata(L, LJ_HUGE_THRESHOLD + 1024u);
  ud = udataV(L->top - 1);
  assert(sizeudata(ud) > LJ_HUGE_THRESHOLD);
  ht = hugetab_for(g, ud, &hi);
  assert((hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) ==
	 (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  collect_dead_huge_stack_object(L, g, ht, ud, "huge userdata");
}

static void test_huge_proto(lua_State *L, global_State *g)
{
  GCfunc *fn;
  GCproto *pt;
  HugeTab *ht;
  LJHugeInfo hi;

  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  ljt_lua_dostring(L,
    "local t = {'return function() local x = 0\\n'}\n"
    "for i = 1, 6000 do t[#t+1] = 'x = x + 1\\n' end\n"
    "t[#t+1] = 'return x end'\n"
    "return assert(loadstring(table.concat(t)))()\n");
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  assert(pt->sizept > LJ_HUGE_THRESHOLD);
  ht = hugetab_for(g, pt, &hi);
  assert((hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) ==
	 (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  collect_dead_huge_stack_object(L, g, ht, pt, "huge proto");
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);

  assert(lj_gc2_workers_set(g, 0));
  test_overaligned_cdata(L, g);
  test_huge_userdata(L, g);
  test_huge_proto(L, g);

  lua_close(L);
  puts("t-gc2-overaligned-cdata-progress OK");
  return 0;
}
