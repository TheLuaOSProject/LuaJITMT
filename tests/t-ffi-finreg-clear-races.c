/*
** Deterministic FINREG transaction races.
**
** Pause owners across slot, order and flag transitions to prove which
** operations can finish while a publisher is still parked.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cdata.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_CDATA_TEST_HELPERS)
#error "t-ffi-finreg-clear-races requires LJ_CDATA_TEST_HELPERS"
#endif

#if LJ_HASFFI

#define FINREG_TEST_RETRY_MAX 1000000u
#define FINREG_TEST_SPINE_MAX 1000000u

static void wait_for_pause(uint32_t point)
{
  uint32_t retry;
  for (retry = 0; retry < FINREG_TEST_RETRY_MAX; retry++) {
    if (lj_cdata_test_fin_pause_waiting(point))
      return;
    (void)sched_yield();
  }
  fprintf(stderr, "FINREG pause %u was not reached\n", point);
  abort();
}

static GCcdata *global_cdata(lua_State *L, const char *name)
{
  GCcdata *cd;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  cd = cdataV(L->top - 1);
  lua_pop(L, 1);
  return cd;
}

/* Return an exact active-order count without allowing a raw spine pointer to
** cross either the tactical SMR interval or its node's body lease. */
static uint32_t active_order_refs(global_State *g, GCobj *target)
{
  CTState *cts = ctype_ctsG(g);
  uint32_t retry;
  assert(cts != NULL);
  for (retry = 0; retry < FINREG_TEST_RETRY_MAX; retry++) {
    FinRegOrderNode *ord;
    uint32_t count = 0, steps = 0;
    int complete = 1;
    if (!lj_gc2_smr_read_try(g)) {
      (void)sched_yield();
      continue;
    }
    ord = fin_order_head_acq(cts);
    while (ord != NULL) {
      LJGC2Lease lease = { 0 };
      FinRegOrderNode *next;
      uint32_t active;
      GCobj *obj;
      if (++steps > FINREG_TEST_SPINE_MAX ||
          lj_gc2_mem_lease_acquire(g, ord, &lease) < 0) {
        lj_gc2_lease_release(&lease);
        complete = 0;
        break;
      }
      next = fin_order_next_acq(ord);
      active = fin_order_active_acq(ord);
      obj = active == 1 ? fin_order_obj_acq(ord) : NULL;
      lj_gc2_lease_release(&lease);
      if (active == 1 && obj == target)
        count++;
      ord = next;
    }
    lj_gc2_smr_read_leave(g);
    if (complete)
      return count;
    (void)sched_yield();
  }
  fprintf(stderr, "FINREG order spine never admitted a complete read\n");
  abort();
}

static void assert_registered(lua_State *L, const char *cdname,
                              const char *finname)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
  GCcdata *cd = global_cdata(L, cdname);
  TValue key, callback, expected;
  int rc;

  setcdataV(L, &key, cd);
  rc = lj_ctype_fin_get(L, cts, &key, &held);
  assert(rc == LJ_CTYPE_FIN_FOUND);
  assert(held.tab != NULL && held.slot != NULL && held.smr_held);
  assert(lj_tab_read_current_keyed(G(L), held.tab, held.slot, &key,
				   &callback) ==
         LJ_TAB_STORE_CAS_OK);
  assert(!tvisnil(&callback));
  assert(!tvisforward(&callback));
  assert(!lj_cdata_fin_isclaim(&callback));
  lua_getglobal(L, finname);
  lj_tv_load_acq(&expected, L->top - 1);
  assert(tvisfunc(&expected));
  assert(lj_obj_equal(&callback, &expected));
  lua_pop(L, 1);
  lj_ctype_fin_lease_release(&held);

  assert(active_order_refs(g, obj2gco(cd)) == 1u);
  assert((lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN) != 0);
  assert(lj_gc2_finreg_cdata_pending(g));
}

static void assert_cleared(lua_State *L, const char *cdname)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
  GCcdata *cd = global_cdata(L, cdname);
  TValue key, callback;
  int rc;

  setcdataV(L, &key, cd);
  rc = lj_ctype_fin_get(L, cts, &key, &held);
  assert(rc == LJ_CTYPE_FIN_FOUND);
  assert(lj_tab_read_current_keyed(G(L), held.tab, held.slot, &key,
				   &callback) ==
         LJ_TAB_STORE_CAS_OK);
  assert(tvisnil(&callback));
  lj_ctype_fin_lease_release(&held);
  assert(active_order_refs(g, obj2gco(cd)) == 0u);
  assert((lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN) == 0);
  assert(!lj_gc2_finreg_cdata_pending(g));
}

static void cleanup_registration(lua_State *L, const char *cdname)
{
  char chunk[256];
  int n = snprintf(chunk, sizeof(chunk),
                   "local ffi=require('ffi'); ffi.gc(%s, nil)", cdname);
  assert(n > 0 && (size_t)n < sizeof(chunk));
  ljt_lua_dostring(L, chunk);
  assert_cleared(L, cdname);
}

static void test_unrelated_enable_progress(int first)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  FinRegGen *oldhead;
  uint32_t pause = first ? LJ_CDATA_FIN_PAUSE_FIRST_KEY :
			     LJ_CDATA_FIN_PAUSE_ENABLE_ORDER;

  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "require('threading')\n"
    "collectgarbage('stop')\n"
    "cd_stalled=ffi.new('int[?]', 8)\n"
    "cd_peer=ffi.new('int[?]', 12)\n"
    "hits_stalled, hits_peer=0, 0\n"
    "fin_stalled=function() hits_stalled=hits_stalled+1 end\n"
    "fin_peer=function() hits_peer=hits_peer+1 end\n");
  cts = ctype_ctsG(G(L));
  lj_cdata_test_fin_pause_arm(pause);
  ljt_lua_dostring(L,
    "worker_stalled=assert(require('threading').spawn(function(cd, fin)\n"
    "  return rawequal(require('ffi').gc(cd, fin), cd)\n"
    "end, cd_stalled, fin_stalled))\n");
  wait_for_pause(pause);
  if (first) {
    ljt_lua_dostring(L,
      "local ffi=require('ffi'); "
      "assert(rawequal(ffi.gc(cd_stalled, fin_stalled), cd_stalled))");
    assert(lj_cdata_test_fin_pause_waiting(pause));
    assert_registered(L, "cd_stalled", "fin_stalled");
  }

  oldhead = fin_gen_head_acq(cts);
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "cd_stalled[0]=41\n"
    "assert(cd_stalled[0] == 41 and ffi.sizeof(cd_stalled) >= ffi.sizeof('int'))\n"
    "assert(rawequal(ffi.gc(cd_peer, nil), cd_peer))\n"
    "assert(rawequal(ffi.gc(cd_peer, fin_peer), cd_peer))\n");
  assert(lj_cdata_test_fin_pause_waiting(pause));
  assert(fin_gen_head_acq(cts) != oldhead);
  assert_registered(L, "cd_peer", "fin_peer");

  lj_cdata_test_fin_pause_release(pause);
  ljt_lua_dostring(L,
    "local ok, result=worker_stalled:join(10)\n"
    "assert(ok and result == true)\n"
    "worker_stalled=nil\n");
  assert_registered(L, "cd_stalled", "fin_stalled");
  {
    FinRegGen *node;
    size_t generations = 0;
    assert(lj_gc2_smr_read_try(G(L)));
    for (node = fin_gen_head_acq(cts); node; node = fin_gen_next_acq(node))
      assert(++generations <= 3);
    lj_gc2_smr_read_leave(G(L));
    assert(generations == 3);  /* Initial table and precisely one per key. */
  }

  ljt_lua_dostring(L,
    "cd_stalled, cd_peer=nil, nil\n"
    "for i=1,4 do collectgarbage('collect') end\n"
    "assert(hits_stalled == 1 and hits_peer == 1)\n");
  lua_close(L);
}

static void test_first_key_generation_reclaim(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  CTState *cts;
  unsigned batch;
  ljt_lua_dostring(L, "require('ffi'); collectgarbage('stop')");
  cts = ctype_ctsG(g);
  for (batch = 0; batch < 6; batch++) {
    FinRegGen *head;
    ljt_lua_dostring(L,
      "local ffi=require('ffi'); local f=function() end "
      "for i=1,128 do local x=ffi.new('int[1]', i); ffi.gc(x, f); "
      "  ffi.gc(x, nil); assert(x[0] == i) end "
      "collectgarbage('collect'); collectgarbage('stop')");
    assert(lj_gc2_smr_read_try(g));
    head = fin_gen_head_acq(cts);
    assert(head && !head->insert_nodes && !fin_gen_next_acq(head));
    lj_gc2_smr_read_leave(g);
  }
  lua_close(L);
}

static void test_replacement_identity(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);
  uint64_t retired0, retired1;

  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "cd_replace=ffi.new('int[1]')\n"
    "fin_replace_old=function() end\n"
    "fin_replace_new=function() end\n"
    "ffi.gc(cd_replace, fin_replace_old)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  assert_registered(L, "cd_replace", "fin_replace_old");

  retired0 = gc2_finreg_cdata_order_retired_acq(g);
  ljt_lua_dostring(L,
    "require('ffi').gc(cd_replace, fin_replace_new)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  assert(gc2_finreg_cdata_order_retired_acq(g) == retired0 + 1u);
  assert_registered(L, "cd_replace", "fin_replace_new");

  /* Repeating the identical callback reuses its exact active membership. */
  retired1 = gc2_finreg_cdata_order_retired_acq(g);
  ljt_lua_dostring(L,
    "require('ffi').gc(cd_replace, fin_replace_new)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  assert(gc2_finreg_cdata_order_retired_acq(g) == retired1);
  assert_registered(L, "cd_replace", "fin_replace_new");
  cleanup_registration(L, "cd_replace");
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  ljt_lua_dostring(L,
    "require('ffi').gc(cd_replace, nil)\n");
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert_cleared(L, "cd_replace");
}

static void test_replacement_vs_clear(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);
  uint64_t retired0;

  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "cd_replace_race=ffi.new('int[1]')\n"
    "fin_replace_race_old=function() end\n"
    "fin_replace_race_new=function() end\n"
    "ffi.gc(cd_replace_race, fin_replace_race_old)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  retired0 = gc2_finreg_cdata_order_retired_acq(g);

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_replace_race=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, cd_replace_race, fin_replace_race_new)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  assert(gc2_finreg_cdata_order_retired_acq(g) == retired0 + 1u);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);

  /* The clear completes the ready enable, then removes that generation
  ** while the publishing owner is still stopped. */
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_replace_clear=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_replace_race)\n");
  ljt_lua_dostring(L,
    "local ok, result=worker_replace_clear:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_replace_clear=nil\n");
  assert(lj_cdata_test_fin_pause_waiting(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER));
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert_cleared(L, "cd_replace_race");

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ok, result=worker_replace_race:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_replace_race=nil\n");
  assert_cleared(L, "cd_replace_race");
}

static void test_enable_order_vs_clear(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "local th=require('threading')\n"
    "cd_enable_order=ffi.new('int[1]')\n"
    "fin_enable_order=function() end\n"
    "worker_enable_order=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, cd_enable_order, fin_enable_order)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);

  /* The clear completes the ready enable, then removes that generation
  ** while the publishing owner is still stopped. */
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_clear_enable=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_enable_order)\n");
  ljt_lua_dostring(L,
    "local ok, result=worker_clear_enable:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_clear_enable=nil\n");
  assert(lj_cdata_test_fin_pause_waiting(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER));
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert_cleared(L, "cd_enable_order");

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ok, result=worker_enable_order:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_enable_order=nil\n");
  assert_cleared(L, "cd_enable_order");
}

static void test_clear_miss_vs_enable(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_MISS);
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "local th=require('threading')\n"
    "cd_clear_miss=ffi.new('int[1]')\n"
    "fin_clear_miss=function() end\n"
    "worker_clear_miss=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_clear_miss)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_MISS);

  ljt_lua_dostring(L,
    "require('ffi').gc(cd_clear_miss, fin_clear_miss)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_MISS);
  ljt_lua_dostring(L,
    "local ok, result=worker_clear_miss:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_clear_miss=nil\n");

  /* A true MISS is a no-op: it cannot clear the unseen enable's flag/counter. */
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  assert_registered(L, "cd_clear_miss", "fin_clear_miss");
  cleanup_registration(L, "cd_clear_miss");
}

static void test_clear_nil_vs_reenable(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);

  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "cd_clear_nil=ffi.new('int[1]')\n"
    "fin_clear_nil_old=function() end\n"
    "fin_clear_nil_new=function() end\n"
    "ffi.gc(cd_clear_nil, fin_clear_nil_old)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  assert_registered(L, "cd_clear_nil", "fin_clear_nil_old");

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_clear_nil=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_clear_nil)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);

  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_reenable=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, cd_clear_nil, fin_clear_nil_new)\n");
  /* Re-enable must finish while the old clear remains stopped. */
  ljt_lua_dostring(L,
    "local ok, result=worker_reenable:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_reenable=nil\n");
  assert(lj_cdata_test_fin_pause_waiting(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL));
  assert_registered(L, "cd_clear_nil", "fin_clear_nil_new");

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local ok, result=worker_clear_nil:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_clear_nil=nil\n");

  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 2u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert_registered(L, "cd_clear_nil", "fin_clear_nil_new");
  cleanup_registration(L, "cd_clear_nil");
}

typedef struct FinOrderReader {
  global_State *g;
  FinRegOrderNode *held;
  uint32_t ready;
  uint32_t release;
} FinOrderReader;

static void *hold_order_reader(void *opaque)
{
  FinOrderReader *reader = (FinOrderReader *)opaque;
  assert(lj_gc2_smr_read_try(reader->g));
  reader->held = fin_order_head_acq(ctype_ctsG(reader->g));
  assert(reader->held && fin_order_active_acq(reader->held));
  la_store32_rel(&reader->ready, 1);
  while (!la_load32_acq(&reader->release))
    (void)sched_yield();
  assert(!fin_order_active_acq(reader->held));
  assert(lj_gc2_mem_registered(reader->g, reader->held));
  lj_gc2_smr_read_leave(reader->g);
  return NULL;
}

static size_t fin_order_storage_count(global_State *g)
{
  FinRegOrderNode *node;
  size_t count = 0;
  assert(lj_gc2_smr_read_try(g));
  for (node = fin_order_lifetime_acq(ctype_ctsG(g)); node != NULL;
       node = fin_order_lifetime_next_acq(node)) {
    assert(lj_gc2_mem_registered(g, node));
    assert(++count < FINREG_TEST_SPINE_MAX);
  }
  lj_gc2_smr_read_leave(g);
  return count;
}

static void test_order_storage_reclaim(lua_State *L)
{
  global_State *g = G(L);
  FinOrderReader reader = { 0 };
  uint64_t completed_epoch;
  pthread_t thread;
  unsigned batch;
  /* Public collection completes the handshake before any test reader starts. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
  completed_epoch = lj_gc2_retire_epoch(g);
  assert(completed_epoch != 0);
  reader.g = g;
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "order_storage_fin=function(x) assert(x[0] == 42) end\n"
    "order_storage_cd=ffi.gc(ffi.new('int[1]', 42), order_storage_fin)\n");
  assert(pthread_create(&thread, NULL, hold_order_reader, &reader) == 0);
  while (!la_load32_acq(&reader.ready))
    (void)sched_yield();
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.gc(order_storage_cd, nil)\n"
    "for i=1,1000 do ffi.gc(order_storage_cd, order_storage_fin); "
    "ffi.gc(order_storage_cd, nil) end\n");
  assert(lj_gc2_reclaim_retired(g, completed_epoch) == 0);
  assert(fin_order_storage_count(g) >= 1001);
  la_store32_rel(&reader.release, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_gc2_reclaim_retired(g, completed_epoch) >= 1001);
  assert(fin_order_storage_count(g) == 0);

  for (batch = 0; batch < 12; batch++) {
    ljt_lua_dostring(L,
      "local ffi=require('ffi')\n"
      "for i=1,1000 do ffi.gc(order_storage_cd, order_storage_fin); "
      "ffi.gc(order_storage_cd, nil) end\n"
      "ffi.gc(order_storage_cd, order_storage_fin)\n"
      "collectgarbage('collect'); collectgarbage('stop')\n");
    assert(fin_order_storage_count(g) == 1);
    assert_registered(L, "order_storage_cd", "order_storage_fin");
    cleanup_registration(L, "order_storage_cd");
  }
}

typedef enum FinPrepareTest {
  PREPARE_REARM, PREPARE_FIRST, PREPARE_REPLACE, PREPARE_CLEAR
} FinPrepareTest;

static void test_prepare_vs_reenable(lua_State *L, FinPrepareTest state)
{
  uint32_t pause = state == PREPARE_CLEAR ? LJ_CDATA_FIN_PAUSE_CLEAR_PREPARE :
                                LJ_CDATA_FIN_PAUSE_ENABLE_PREPARE;
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "prepare_cd=ffi.new('int[1]', 19)\n"
    "prepare_first=function() end\n"
    "prepare_second=function() end\n");
  if (state != PREPARE_FIRST)
    ljt_lua_dostring(L,
      "local ffi=require('ffi'); ffi.gc(prepare_cd, function() end)");
  if (state == PREPARE_REARM)
    ljt_lua_dostring(L, "require('ffi').gc(prepare_cd, nil)");
  if (state == PREPARE_CLEAR)
    ljt_lua_dostring(L, "prepare_first=nil");
  lj_cdata_test_fin_pause_arm(pause);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "prepare_worker=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, prepare_cd, prepare_first)\n");
  wait_for_pause(pause);

  ljt_lua_dostring(L, "require('ffi').gc(prepare_cd, prepare_second)");
  assert(lj_cdata_test_fin_pause_waiting(pause));
  assert_registered(L, "prepare_cd", "prepare_second");

  lj_cdata_test_fin_pause_release(pause);
  ljt_lua_dostring(L,
    "local ok, result=prepare_worker:join(10)\n"
    "assert(ok == true and result == true)\n"
    "prepare_worker=nil\n");
  if (state == PREPARE_CLEAR) {
    assert_cleared(L, "prepare_cd");
  } else {
    assert_registered(L, "prepare_cd", "prepare_first");
    cleanup_registration(L, "prepare_cd");
  }
}

static void test_identical_rearm_vs_replace(lua_State *L)
{
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "same_fin=function() end\n"
    "different_fin=function() end\n"
    "same_cd=ffi.gc(ffi.new('int[1]'), same_fin)\n");
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_UNCHANGED);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "same_worker=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, same_cd, same_fin)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_UNCHANGED);
  ljt_lua_dostring(L, "require('ffi').gc(same_cd, different_fin)");
  assert(lj_cdata_test_fin_pause_waiting(LJ_CDATA_FIN_PAUSE_ENABLE_UNCHANGED));
  assert_registered(L, "same_cd", "different_fin");

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_UNCHANGED);
  ljt_lua_dostring(L,
    "local ok, result=same_worker:join(10)\n"
    "assert(ok == true and result == true)\n"
    "same_worker=nil\n");
  assert_registered(L, "same_cd", "different_fin");
  cleanup_registration(L, "same_cd");
}

static void test_queued_delivery_vs_suspended_rearm(lua_State *L)
{
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "queued_a, queued_b=0, 0\n"
    "queued_b_fin=function(x) assert(x[0] == 51); queued_b=queued_b+1 end\n"
    "queued_cd=ffi.gc(ffi.new('int[1]', 51),function() queued_a=queued_a+1 end)\n");
  assert(lj_gc2_finreg_cdata_finalize_close(G(L)) == 1);

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "queued_worker=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, queued_cd, queued_b_fin)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  lj_gc2_finalizer_dispatch_all(L);
  assert(lj_cdata_test_fin_pause_waiting(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER));
  ljt_lua_dostring(L, "assert(queued_a == 0 and queued_b == 0)");

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ok, result=queued_worker:join(10)\n"
    "assert(ok == true and result == true)\n"
    "queued_worker, queued_cd=nil, nil\n"
    "for i=1,4 do collectgarbage('collect') end\n"
    "assert(queued_a == 0 and queued_b == 1)\n");
}

static void test_rearm_after_callback(lua_State *L)
{
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "local first, second, late, revived=0, 0, 0, nil\n"
    "do\n"
    "  local cd=ffi.gc(ffi.new('int[1]'), function(value)\n"
    "    first=first+1\n"
    "    assert(rawequal(ffi.gc(value, function() second=second+1 end), value))\n"
    "  end)\n"
    "end\n"
    "do\n"
    "  local cd=ffi.gc(ffi.new('int[1]'), function(value) revived=value end)\n"
    "end\n"
    "for i=1,4 do collectgarbage('collect') end\n"
    "assert(first == 1 and second == 1 and revived ~= nil)\n"
    "ffi.gc(revived, function() late=late+1 end)\n"
    "revived=nil\n"
    "for i=1,4 do collectgarbage('collect') end\n"
    "assert(first == 1 and second == 1 and late == 1)\n");
  assert(!lj_gc2_finreg_cdata_pending(G(L)));
}

static void test_nested_finalizer_collect(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);

  /* The outer dispatcher retains the cdata body across this callback. A full
  ** collection requested by the same finalizer owner must defer, return, and
  ** let the outer SWEEP finish after that body scope is released. */
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "nested_fin_count=0\n"
    "do\n"
    "  local cd=ffi.gc(ffi.new('int[1]'), function()\n"
    "    nested_fin_count=nested_fin_count+1\n"
    "    collectgarbage('collect')\n"
    "  end)\n"
    "end\n"
    "collectgarbage('restart')\n"
    "collectgarbage('collect')\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n"
    "assert(nested_fin_count == 1)\n");
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert(!lj_gc2_finreg_cdata_pending(g));
}

#endif

int main(void)
{
#if LJ_HASFFI
  lua_State *L = ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "require('ffi')\n"
    "require('threading')\n"
    "collectgarbage('collect')\n"
    "collectgarbage('stop')\n");
  assert(!lj_gc2_finreg_cdata_pending(G(L)));

  test_order_storage_reclaim(L);
  test_unrelated_enable_progress(0);
  test_unrelated_enable_progress(1);
  test_first_key_generation_reclaim();
  test_replacement_identity(L);
  test_replacement_vs_clear(L);
  test_enable_order_vs_clear(L);
  test_clear_miss_vs_enable(L);
  test_clear_nil_vs_reenable(L);
  test_prepare_vs_reenable(L, PREPARE_REARM);
  test_prepare_vs_reenable(L, PREPARE_FIRST);
  test_prepare_vs_reenable(L, PREPARE_REPLACE);
  test_prepare_vs_reenable(L, PREPARE_CLEAR);
  test_identical_rearm_vs_replace(L);
  test_queued_delivery_vs_suspended_rearm(L);
  test_nested_finalizer_collect(L);
  test_rearm_after_callback(L);

  lua_close(L);
  printf("t-ffi-finreg-clear-races OK: all FINREG transaction races agree\n");
#else
  printf("t-ffi-finreg-clear-races SKIP: FFI disabled\n");
#endif
  return 0;
}
