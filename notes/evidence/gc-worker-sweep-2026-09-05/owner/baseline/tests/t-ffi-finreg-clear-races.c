/*
** Deterministic FINREG transaction races.
**
** These pauses cover the three slot/order/flag interleavings which a broad
** registration hammer cannot prove:
**   - clear observes an enable's published order node while FINCLAIM is held;
**   - clear has authoritatively missed immediately before an enable commits;
**   - re-enable retries while clear has retired order/flag but not published
**     the nil callback slot yet.
*/

#include <assert.h>
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
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_replace_race=th.spawn(function(cd, fin)\n"
    "  require('ffi').gc(cd, fin)\n"
    "  return true\n"
    "end, cd_replace_race, fin_replace_race_new)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  assert(gc2_finreg_cdata_order_retired_acq(g) == retired0 + 1u);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);

  /* Replacement owns FINCLAIM after atomically swapping the order identity.
  ** A clear can only retry without retiring the new node or accounting a clear
  ** against the still-active registration. */
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_replace_clear=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_replace_race)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  assert(active_order_refs(g,
         obj2gco(global_cdata(L, "cd_replace_race"))) == 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ok, result=worker_replace_race:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_replace_race=nil\n");
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);

  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local ok, result=worker_replace_clear:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_replace_clear=nil\n");
  assert_cleared(L, "cd_replace_race");
}

static void test_enable_order_vs_clear(lua_State *L)
{
  global_State *g = G(L);
  uint64_t sets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t clears0 = gc2_finreg_cdata_clears_acq(g);

  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
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

  /* The clear retries while the exact callback slot is FINCLAIM. It must not
  ** retire the enable's already-published order node. */
  ljt_lua_dostring(L,
    "local th=require('threading')\n"
    "worker_clear_enable=th.spawn(function(cd)\n"
    "  require('ffi').gc(cd, nil)\n"
    "  return true\n"
    "end, cd_enable_order)\n");
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  assert(active_order_refs(g,
         obj2gco(global_cdata(L, "cd_enable_order"))) == 1u);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0);

  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  ljt_lua_dostring(L,
    "local ok, result=worker_enable_order:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_enable_order=nil\n");
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
  wait_for_pause(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 1u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local ok, result=worker_clear_enable:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_clear_enable=nil\n");
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
  lj_cdata_test_fin_pause_arm(LJ_CDATA_FIN_PAUSE_ENABLE_RETRY);
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
  wait_for_pause(LJ_CDATA_FIN_PAUSE_ENABLE_RETRY);

  /* Clear owns the nil publication. Re-enable may only retry after releasing
  ** its transient lease, so it cannot have overwritten the clear's FIN bit. */
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  ljt_lua_dostring(L,
    "local ok, result=worker_clear_nil:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_clear_nil=nil\n");
  lj_cdata_test_fin_pause_release(LJ_CDATA_FIN_PAUSE_ENABLE_RETRY);
  ljt_lua_dostring(L,
    "local ok, result=worker_reenable:join(10)\n"
    "assert(ok == true and result == true)\n"
    "worker_reenable=nil\n");

  assert(gc2_finreg_cdata_sets_acq(g) == sets0 + 2u);
  assert(gc2_finreg_cdata_clears_acq(g) == clears0 + 1u);
  assert_registered(L, "cd_clear_nil", "fin_clear_nil_new");
  cleanup_registration(L, "cd_clear_nil");
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

  test_replacement_identity(L);
  test_replacement_vs_clear(L);
  test_enable_order_vs_clear(L);
  test_clear_miss_vs_enable(L);
  test_clear_nil_vs_reenable(L);
  test_nested_finalizer_collect(L);

  lua_close(L);
  printf("t-ffi-finreg-clear-races OK: all FINREG transaction races agree\n");
#else
  printf("t-ffi-finreg-clear-races SKIP: FFI disabled\n");
#endif
  return 0;
}
