/*
** Persistent table-resize descriptor identity and lifetime substrate.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_trace.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-resize-descriptor requires LJ_TAB_TEST_HELPERS"
#endif

enum {
  INSTALL_HOOK_HELP = 1,
  INSTALL_HOOK_INVALIDATE,
  INSTALL_HOOK_PRECONTROL_HELP,
  INSTALL_HOOK_PRECONTROL_DISPLACE,
  INSTALL_HOOK_MAINTAIN,
  INSTALL_HOOK_PRECLAIM_RACE,
  INSTALL_HOOK_PRECLAIM_DISCARD
};

static uint32_t install_hook_expect;
static uint32_t install_hook_mode;
static uint32_t install_hook_calls;
static lua_State *clear_hook_L;
static uint32_t clear_hook_later_acap;
static uint64_t clear_hook_weak_record;
static TabResizeDesc *install_racing_desc;
static uint64_t install_racing_weak_record;

static void install_cancel_pause_hook(lua_State *L, GCtab *t,
				      TabResizeDesc *desc, uint32_t stage);
static void install_pause_hook(lua_State *L, GCtab *t, TabResizeDesc *desc,
			       uint32_t stage)
{
  if (stage != install_hook_expect) {
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    return;
  }
  install_hook_calls++;
  if (install_hook_mode == INSTALL_HOOK_PRECLAIM_DISCARD) {
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_BEFORE_PHASE_CAS);
    /*
    ** Discard cannot free an intent already held by this paused installer.
    ** It publishes a terminal registry record; the outer SMR reader retains
    ** that record until the losing phase-CAS caller returns.
    */
    assert(lj_tab_resize_desc_discard(G(L), desc));
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_TERMINAL);
  } else if (install_hook_mode == INSTALL_HOOK_PRECLAIM_RACE) {
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_BEFORE_PHASE_CAS);
    /*
    ** The nested contender wins PREPARED->INSTALLING and initializes the
    ** immutable install snapshot. The delayed outer caller must lose without
    ** writing any descriptor field after publication.
    */
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLED);
    assert(lj_tab_resize_desc_control_acq(t) == desc);
  } else if (install_hook_mode == INSTALL_HOOK_HELP) {
    assert(!lj_tab_resize_desc_terminal(G(L), desc,
					TAB_RESIZE_DESC_INSTALLING));
    if (stage == LJ_TAB_RESIZE_DESC_HOOK_PUBLISHED) {
      assert(!lj_tab_resize_desc_install(L, t, desc));
      assert(lj_tab_resize_desc_phase_acq(desc) ==
	     TAB_RESIZE_DESC_INSTALLING);
      assert(lj_tab_resize_desc_control_acq(t) == NULL);
    } else {
      assert(stage == LJ_TAB_RESIZE_DESC_HOOK_CONTROL);
      assert(lj_tab_resize_desc_install(L, t, desc));
    }
  } else if (install_hook_mode == INSTALL_HOOK_INVALIDATE) {
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_CONTROL);
    lj_tab_weak_acap_rel(t, desc->oldacap + 1u);
    lj_tab_test_set_resize_desc_install_hook(install_cancel_pause_hook);
  } else if (install_hook_mode == INSTALL_HOOK_PRECONTROL_HELP) {
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS);
    /*
    ** A published helper cannot issue or cancel the initial control CAS.
    ** Leaving this stable window untouched prevents terminal reinstallation.
    */
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLING);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
  } else if (install_hook_mode == INSTALL_HOOK_PRECONTROL_DISPLACE) {
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS);
    /*
    ** Model an intervening stable generation after the issuer sampled the
    ** expected word. Its sole CAS must fail and preserve this generation.
    */
    lj_tab_acap_rel(t, desc->oldacap + 1u);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
  } else {
    assert(install_hook_mode == INSTALL_HOOK_MAINTAIN);
    assert(stage == LJ_TAB_RESIZE_DESC_HOOK_PUBLISHED);
    /* Exercise the real global-root scanner wiring, not only its callee. */
    lj_gc2_test_scan_roots(G(L), L);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLING);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
  }
}

static void install_cancel_pause_hook(lua_State *L, GCtab *t,
				      TabResizeDesc *desc, uint32_t stage)
{
  assert(stage == LJ_TAB_RESIZE_DESC_HOOK_CANCELLING);
  install_hook_calls++;
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
}

static void stale_clear_hook(GCtab *t, TabResizeDesc *desc, uint32_t acap)
{
  assert(clear_hook_L != NULL);
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_terminal(G(clear_hook_L), desc,
				      TAB_RESIZE_DESC_TERMINATING));
  /* Model a later completed structural generation before the stale clear. */
  lj_tab_acap_rel(t, clear_hook_later_acap);
}

static void weak_clear_hook(GCtab *t, TabResizeDesc *desc, uint32_t acap)
{
  uint64_t current = lj_tab_weak_record_acq(t);
  UNUSED(desc);
  UNUSED(acap);
  assert(lj_tab_weak_record_cas(t, &current, clear_hook_weak_record));
}

static void displaced_clear_hook(GCtab *t, TabResizeDesc *desc,
				  uint32_t acap)
{
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  /* Leave TERMINATING while modeling a later stable generation. */
  lj_tab_acap_rel(t, clear_hook_later_acap);
}

static void precutover_displaced_clear_hook(GCtab *t, TabResizeDesc *desc,
					     uint32_t acap)
{
  uint64_t control = lj_tab_struct_control_acq(t);
  uint64_t stable;
  UNUSED(acap);
  assert(lj_tab_struct_control_desc(control) == desc);
  stable = lj_tab_struct_control_pack(clear_hook_later_acap, 0);
  assert(lj_tab_struct_control_cas(t, &control, stable));
}

static void structural_clear_hook(GCtab *t, TabResizeDesc *desc,
				   uint32_t acap)
{
  int acquired;
  UNUSED(acap);
  assert(clear_hook_L != NULL);
  acquired = lj_tab_struct_enter(clear_hook_L, t);
  assert(acquired);
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  lj_tab_struct_leave(t, acquired);
}

static void resize_clear_hook(GCtab *t, TabResizeDesc *desc, uint32_t acap)
{
  TValue *array;
  Node *node;
  MSize asize, hmask;
  uint32_t hbits;
  UNUSED(acap);
  assert(clear_hook_L != NULL);
  asize = lj_tab_array_snapshot_acq(t, &array);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  UNUSED(array);
  UNUSED(node);
  hbits = hmask > 0 ? lj_fls((uint32_t)hmask) + 2u : 1u;
  lj_tab_resize(clear_hook_L, t, (uint32_t)asize, hbits);
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
}

static void table_clear_hook(GCtab *t, TabResizeDesc *desc, uint32_t acap)
{
  UNUSED(acap);
  assert(clear_hook_L != NULL);
  lj_tab_test_reset_clear_shared_calls();
  lj_tab_clear(clear_hook_L, t);
  assert(lj_tab_test_clear_shared_calls() == 1);
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
}

static void exercise_marker_encoding(void)
{
#if LJ_64
  static const uint64_t ids[] = {
    1u, 17u, U64x(0000000f,ffffffff)
  };
  static const uint32_t kinds[] = {
    LJ_TAB_RESIZE_MARK_SRC,
    LJ_TAB_RESIZE_MARK_DST,
    LJ_TAB_RESIZE_MARK_DONE,
    LJ_TAB_RESIZE_MARK_NIL_DONE
  };
  TValue tv;
  size_t i, k;

  assert(ids[sizeof(ids) / sizeof(ids[0]) - 1u] ==
	 LJ_TAB_RESIZE_MARK_ID_MAX);
  setforwardV(&tv);
  assert(tvisforward(&tv));
  assert(!tvisresizemarker(&tv));
  assert(lj_tab_resize_marker_id(&tv) == 0);
  setkeylockV(&tv);
  assert(tviskeylock(&tv));
  assert(!tvisresizemarker(&tv));

  for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
      setresizemarkerV(&tv, ids[i], kinds[k]);
      assert(tvisresizemarker(&tv));
      assert(tvistabinternal(&tv));
      assert(!tvisforward(&tv));
      assert(!tviskeylock(&tv));
      assert(lj_tab_resize_marker_id(&tv) == ids[i]);
      assert(lj_tab_resize_marker_kind(&tv) == kinds[k]);
    }
  }

  tv.u64 = LJ_LIGHTUD_INTERNAL_BASE | 7u;
  assert(!tvisresizemarker(&tv));
  tv.u64 = LJ_TAB_RESIZE_MARK_BITS(0, LJ_TAB_RESIZE_MARK_SRC);
  assert(!tvisresizemarker(&tv));
#endif
}

static void exercise_packed_vm_guard_word(void)
{
  global_State fake;
  uint32_t expect;

  memset(&fake, 0, sizeof(fake));
  assert(mt_active_acq(&fake) == 0);
  assert(mt_active_word_acq(&fake) == 0);
  assert(mt_resize_guard_enter(&fake) == 2);
  assert(mt_resize_guard_count_acq(&fake) == 1);
  assert(mt_active_acq(&fake) == 0);

  expect = 0;
  assert(mt_active_cas(&fake, &expect, 1));
  assert(mt_active_acq(&fake) == 1);
  assert(mt_active_word_acq(&fake) == 3);
  mt_resize_guard_leave(&fake);
  assert(mt_active_word_acq(&fake) == LJ_MT_ACTIVE_LATCH);

  la_store32_rlx(&fake.mt_active, 0);
  assert(mt_resize_guard_enter(&fake) == 2);
  assert(mt_resize_guard_enter(&fake) == 1);
  assert(mt_resize_guard_count_acq(&fake) == 2);
  mt_resize_guard_leave(&fake);
  mt_resize_guard_leave(&fake);
  assert(mt_active_word_acq(&fake) == 0);

  la_store32_rlx(&fake.mt_active, LJ_MT_RESIZE_GUARD_MASK);
  assert(!mt_resize_guard_enter(&fake));
  assert(mt_active_word_acq(&fake) == LJ_MT_RESIZE_GUARD_MASK);
}

static void exercise_cross_universe_tracked_smr(lua_State *L)
{
  lua_State *other = luaL_newstate();
  global_State *g = G(L);
  global_State *otherg;
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t readers, other_readers;

  assert(other != NULL);
  otherg = G(other);
  readers = gc2_smr_readers_acq(g);
  other_readers = gc2_smr_readers_acq(otherg);
  lua_createtable(L, 2, 2);
  t = tabV(L->top - 1);
  desc = lj_tab_resize_desc_reserve(
    L, t, (uint32_t)lj_tab_acap_acq(t));
  assert(desc != NULL);

  /*
  ** A normal leaf reader may be counted in an independent universe while
  ** retaining that universe as the TLS identity. A descriptor install can
  ** enter nested trace/GC readers, so both publication paths must refuse the
  ** cross-universe fallback without claiming or adding a count they could
  ** self-wait behind.
  */
  assert(lj_gc2_smr_read_try(otherg));
  assert(gc2_smr_readers_acq(otherg) == other_readers + 1u);
  assert(!lj_gc2_smr_read_tracked_try(g));
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(!lj_tab_resize_desc_discard(g, desc));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_PREPARED);
  assert(gc2_smr_readers_acq(g) == readers);
  lj_gc2_smr_read_leave(otherg);
  assert(gc2_smr_readers_acq(otherg) == other_readers);

  assert(lj_gc2_smr_read_tracked_try(g));
  assert(gc2_smr_readers_acq(g) == readers + 1u);
  lj_gc2_smr_read_leave(g);
  assert(gc2_smr_readers_acq(g) == readers);
  assert(lj_tab_resize_desc_discard(g, desc));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  lua_pop(L, 1);
  lua_close(other);
}

static TabResizeDesc *find_desc_held(global_State *g, GCtab *t, uint64_t id)
{
  return lj_tab_resize_desc_find_held(g, t, id);
}

static void finish_installed(global_State *g, GCtab *t, TabResizeDesc *desc,
			     uint32_t acap)
{
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));
}

static void install_competing_generation_hook(lua_State *L, GCtab *t,
					     TabResizeDesc *desc,
					     uint32_t stage)
{
  uint64_t weak;
  if (stage != LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS) {
    lj_tab_test_set_resize_desc_install_hook(
      install_competing_generation_hook);
    return;
  }
  install_hook_calls++;
  assert(install_racing_desc != NULL);
  assert(install_racing_desc->newacap > desc->oldacap);
  /* The old installer has sampled stable control but has not published either
  ** half of its control/capacity pair. A peer grows the real vector and then
  ** installs a different descriptor. The delayed loser must not overwrite the
  ** peer's capacity shadow when its own table-control CAS fails. */
  lj_tab_resize(L, t, install_racing_desc->newacap, 3);
  weak = lj_tab_weak_record_acq(t);
  assert(lj_tab_weak_record_cas(t, &weak, install_racing_weak_record));
  assert(lj_tab_resize_desc_install(L, t, install_racing_desc));
  assert(install_racing_desc->oldacap == install_racing_desc->newacap);
  assert(lj_tab_acap_acq(t) == install_racing_desc->oldacap);
}

static void exercise_install_competing_generation(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;
  uint64_t weak;
  lua_createtable(L, 4, 4);
  lua_pushinteger(L, 47);
  lua_rawseti(L, -2, 1);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  weak = lj_tab_weak_record_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  install_racing_desc = lj_tab_resize_desc_reserve(L, t, acap + 8u);
  install_racing_weak_record = U64x(00000123,00000001);
  install_hook_calls = 0;
  lj_tab_test_set_resize_desc_install_hook(install_competing_generation_hook);
  assert(lj_gc2_smr_read_try(g));
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(install_hook_calls == 1);
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == install_racing_desc);
  assert(lj_tab_acap_acq(t) == acap + 8u);
  assert(lj_tab_weak_record_acq(t) == install_racing_weak_record);
  assert(lj_tab_resize_desc_maintain_held(g, install_racing_desc));
  assert(mt_resize_guard_count_acq(g) == 1);
  finish_installed(g, t, install_racing_desc, acap + 8u);
  assert(lj_tab_acap_acq(t) == acap + 8u);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  lj_tab_weak_record_store_rlx(t, weak);
  install_racing_desc = NULL;
  lua_rawgeti(L, -1, 1);
  assert(lua_tointeger(L, -1) == 47);
  lua_pop(L, 2);
}

static void install_weak_update_hook(lua_State *L, GCtab *t,
				     TabResizeDesc *desc, uint32_t stage)
{
  uint64_t weak;
  UNUSED(L);
  UNUSED(desc);
  if (stage != LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS) {
    lj_tab_test_set_resize_desc_install_hook(install_weak_update_hook);
    return;
  }
  install_hook_calls++;
  weak = lj_tab_weak_record_acq(t);
  assert(lj_tab_weak_record_cas(t, &weak, install_racing_weak_record));
}

static void exercise_install_weak_update(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;
  uint64_t weak;
  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  weak = lj_tab_weak_record_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  install_racing_weak_record = U64x(00000124,00000002);
  install_hook_calls = 0;
  lj_tab_test_set_resize_desc_install_hook(install_weak_update_hook);
  assert(lj_gc2_smr_read_try(g));
  /* Losing to a weak-state update consumes this one install attempt without
  ** erasing the GC's newer state or leaking a VM guard. A fresh attempt still
  ** succeeds while preserving that exact state. */
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(install_hook_calls == 1);
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(lj_tab_acap_acq(t) == acap);
  assert(lj_tab_weak_record_acq(t) == install_racing_weak_record);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_weak_record_acq(t) == install_racing_weak_record);
  finish_installed(g, t, desc, acap);
  assert(lj_tab_weak_record_acq(t) == install_racing_weak_record);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  lj_tab_weak_record_store_rlx(t, weak);
  lua_pop(L, 1);
}

static void exercise_install_pause_points(lua_State *L)
{
  static const uint32_t stages[] = {
    LJ_TAB_RESIZE_DESC_HOOK_PUBLISHED,
    LJ_TAB_RESIZE_DESC_HOOK_CONTROL
  };
  global_State *g = G(L);
  size_t i;

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_BEFORE_PHASE_CAS;
    install_hook_mode = INSTALL_HOOK_PRECLAIM_RACE;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    assert(lj_gc2_smr_read_try(g));
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLED);
    assert(lj_tab_resize_desc_control_acq(t) == desc);
    assert(desc->stable_control ==
	   lj_tab_struct_control_pack(acap, 0));
    assert(desc->oldacap == acap);
    finish_installed(g, t, desc, acap);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint64_t id;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    id = lj_tab_resize_desc_id_acq(desc);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_BEFORE_PHASE_CAS;
    install_hook_mode = INSTALL_HOOK_PRECLAIM_DISCARD;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    assert(lj_gc2_smr_read_try(g));
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_TERMINAL);
    assert(find_desc_held(g, t, id) == desc);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = stages[i];
    install_hook_mode = INSTALL_HOOK_HELP;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    assert(lj_gc2_smr_read_try(g));
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLED);
    assert(lj_tab_resize_desc_control_acq(t) == desc);
    finish_installed(g, t, desc, acap);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_CONTROL;
    install_hook_mode = INSTALL_HOOK_INVALIDATE;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    /*
    ** The first hook invalidates the captured capacity. The cancellation hook
    ** then acts as a different helper paused after TERMINATING was claimed.
    */
    assert(lj_gc2_smr_read_try(g));
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 2);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_TERMINAL);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
    assert(lj_tab_acap_acq(t) == acap + 1u);
    lj_tab_acap_rel(t, acap);
    assert(!(lj_tab_resize_desc_flags_acq(desc) &
	     TAB_RESIZE_DESC_F_CUTOVER));
    assert(!lj_tab_resize_desc_clear(t, desc, acap));
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS;
    install_hook_mode = INSTALL_HOOK_PRECONTROL_DISPLACE;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    assert(lj_gc2_smr_read_try(g));
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_TERMINAL);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
    assert(lj_tab_acap_acq(t) == acap + 1u);
    lj_tab_acap_rel(t, acap);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS;
    install_hook_mode = INSTALL_HOOK_PRECONTROL_HELP;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    /*
    ** A helper arriving after registry publication cannot issue the initial
    ** table-control CAS. The publishing call remains its sole issuer.
    */
    assert(lj_gc2_smr_read_try(g));
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLED);
    assert(lj_tab_resize_desc_control_acq(t) == desc);
    finish_installed(g, t, desc, acap);
    /*
    ** A published helper cannot reinstall a descriptor after its one
    ** descriptor-to-stable detachment.
    */
    assert(!lj_tab_resize_desc_install(L, t, desc));
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_TERMINAL);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
    assert(lj_tab_acap_acq(t) == acap);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    install_hook_expect = LJ_TAB_RESIZE_DESC_HOOK_PUBLISHED;
    install_hook_mode = INSTALL_HOOK_MAINTAIN;
    install_hook_calls = 0;
    lj_tab_test_set_resize_desc_install_hook(install_pause_hook);
    assert(lj_gc2_smr_read_try(g));
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(install_hook_calls == 1);
    assert(lj_tab_resize_desc_phase_acq(desc) ==
	   TAB_RESIZE_DESC_INSTALLED);
    assert(lj_tab_resize_desc_control_acq(t) == desc);
    finish_installed(g, t, desc, acap);
    assert(lj_tab_acap_acq(t) == acap);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }
  lj_tab_test_set_resize_desc_install_hook(NULL);
}

static void exercise_stale_clear(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t newacap, oldacap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  oldacap = (uint32_t)lj_tab_acap_acq(t);
  assert(oldacap < TABARRAY_ACAP_MASK);
  newacap = oldacap + 1u;
  desc = lj_tab_resize_desc_reserve(L, t, newacap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));

  clear_hook_L = L;
  /*
  ** The nested helper commits A->B and terminalizes D1. A later structural
  ** generation then returns to A before the outer stale clear resumes. The
  ** stale D1 writer must not restore B over that later A generation.
  */
  clear_hook_later_acap = oldacap;
  lj_tab_test_set_resize_desc_clear_hook(stale_clear_hook);
  assert(!lj_tab_resize_desc_clear(t, desc, newacap));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  lj_gc2_smr_read_leave(g);
  assert(lj_tab_acap_acq(t) == clear_hook_later_acap);
  assert(lj_tab_struct_control_acap(lj_tab_struct_control_acq(t)) ==
	 clear_hook_later_acap);
  clear_hook_L = NULL;
  lj_tab_test_set_resize_desc_clear_hook(NULL);
  lua_pop(L, 1);
}

static void exercise_distinct_capacity_cutover(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint64_t prior, changed, expect;
  uint32_t oldacap, newacap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  oldacap = (uint32_t)lj_tab_acap_acq(t);
  assert(oldacap < TABARRAY_ACAP_MASK);
  newacap = oldacap + 1u;
  desc = lj_tab_resize_desc_reserve(L, t, newacap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));

  /* A mismatched completion must leave both phase and control untouched. */
  assert(!lj_tab_resize_desc_clear(t, desc, oldacap));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_PUBLISHING);
  assert(lj_tab_resize_desc_control_acq(t) == desc);

  /*
  ** Change the semantic weak word after clear sampled the 128-bit pair. The
  ** failed first cmpxchg16b must retry with the new cycle/state while
  ** publishing the genuinely different target shadow under descriptor control.
  */
  prior = lj_tab_weak_record_acq(t);
  changed = lj_tab_weak_record_pack(
    lj_tab_weak_record_cycle(prior) + 1u, LJ_TAB_WEAK_RECORD_INSTALLING);
  clear_hook_weak_record = changed;
  lj_tab_test_set_resize_desc_clear_hook(weak_clear_hook);
  assert(lj_tab_resize_desc_clear(t, desc, newacap));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_TERMINATING);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(lj_tab_struct_control_acap(lj_tab_struct_control_acq(t)) ==
	 newacap);
  assert(lj_tab_acap_acq(t) == newacap);
  assert(lj_tab_weak_record_acq(t) == changed);
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));

  /*
  ** The fixture changes only descriptor metadata, not the real allocation;
  ** restore both capacity and semantic weak state before ordinary table use.
  */
  lj_tab_acap_rel(t, oldacap);
  expect = changed;
  assert(lj_tab_weak_record_cas(t, &expect, prior));
  lj_tab_test_set_resize_desc_clear_hook(NULL);
  clear_hook_weak_record = 0;
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_displaced_clear(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));

  clear_hook_later_acap = acap + 1u;
  lj_tab_test_set_resize_desc_clear_hook(displaced_clear_hook);
  /*
  ** The stale outer clear observes a different stable generation. That is
  ** a completed but displaced attempt: it must not overwrite the later pair.
  */
  assert(!lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_TERMINATING);
  assert(lj_tab_acap_acq(t) == clear_hook_later_acap);
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_acap_acq(t) == clear_hook_later_acap);
  lj_tab_acap_rel(t, acap);
  lj_tab_test_set_resize_desc_clear_hook(NULL);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_precutover_displaced_clear(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  assert(acap < TABARRAY_ACAP_MASK);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));

  /*
  ** Displace descriptor control after clear sampled the pair but before its
  ** first cmpxchg16b. The failed attempt must preserve the later generation
  ** and must not claim exact cutover provenance merely because newacap equals
  ** the descriptor target.
  */
  clear_hook_later_acap = acap + 1u;
  lj_tab_test_set_resize_desc_clear_hook(
    precutover_displaced_clear_hook);
  assert(!lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_TERMINATING);
  assert(!(lj_tab_resize_desc_flags_acq(desc) &
	   TAB_RESIZE_DESC_F_CUTOVER));
  assert(lj_tab_acap_acq(t) == clear_hook_later_acap);
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_tab_acap_rel(t, acap);
  lj_tab_test_set_resize_desc_clear_hook(NULL);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_structural_help(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));

  /*
  ** A normal structural contender arriving at CLEARING must finish the
  ** descriptor through the production contention path, without a GC scan.
  */
  clear_hook_L = L;
  lj_tab_test_set_resize_desc_clear_hook(structural_clear_hook);
  assert(!lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));
  clear_hook_L = NULL;
  lj_tab_test_set_resize_desc_clear_hook(NULL);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_structural_mutation_gates(lua_State *L)
{
  global_State *g = G(L);

  {
    TabResizeDesc *desc;
    GCtab *t;
    MSize oldhmask;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    (void)lj_tab_node_snapshot_acq(t, &oldhmask);
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    assert(lj_gc2_smr_read_try(g));
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				      TAB_RESIZE_DESC_RETIRING));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				      TAB_RESIZE_DESC_MIGRATING));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				      TAB_RESIZE_DESC_PUBLISHING));
    /*
    ** The normal resize entry must treat descriptor control as shared even
    ** after global MT/GC state has returned to a private window. Its structural
    ** entry helps CLEARING to terminal before replacing either root.
    */
    clear_hook_L = L;
    lj_tab_test_set_resize_desc_clear_hook(resize_clear_hook);
    assert(!lj_tab_resize_desc_clear(t, desc, acap));
    assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
    assert(lj_tab_resize_desc_flags_acq(desc) &
	   TAB_RESIZE_DESC_F_CUTOVER);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
    {
      MSize hmask;
      (void)lj_tab_node_snapshot_acq(t, &hmask);
      assert(hmask > oldhmask);
    }
    clear_hook_L = NULL;
    lj_tab_test_set_resize_desc_clear_hook(NULL);
    lj_gc2_smr_read_leave(g);
    lua_pop(L, 1);
  }

  {
    TabResizeDesc *desc;
    GCtab *t;
    uint32_t acap;
    lua_createtable(L, 4, 4);
    t = tabV(L->top - 1);
    lua_pushinteger(L, 11);
    lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 22);
    lua_setfield(L, -2, "descriptor-clear");
    acap = (uint32_t)lj_tab_acap_acq(t);
    desc = lj_tab_resize_desc_reserve(L, t, acap);
    assert(lj_gc2_smr_read_try(g));
    assert(lj_tab_resize_desc_install(L, t, desc));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				      TAB_RESIZE_DESC_RETIRING));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				      TAB_RESIZE_DESC_MIGRATING));
    assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				      TAB_RESIZE_DESC_PUBLISHING));
    clear_hook_L = L;
    lj_tab_test_set_resize_desc_clear_hook(table_clear_hook);
    assert(!lj_tab_resize_desc_clear(t, desc, acap));
    assert(lj_tab_resize_desc_flags_acq(desc) &
	   TAB_RESIZE_DESC_F_CUTOVER);
    clear_hook_L = NULL;
    lj_tab_test_set_resize_desc_clear_hook(NULL);
    lj_gc2_smr_read_leave(g);
    lua_rawgeti(L, -1, 1);
    assert(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "descriptor-clear");
    assert(lua_isnil(L, -1));
    lua_pop(L, 2);
  }
}

static void exercise_private_store_gates(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  TValue src[2];
  uint32_t acap, readers;

  lua_createtable(L, 8, 8);
  t = tabV(L->top - 1);
  lua_pushinteger(L, 1);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "existing");
  lua_pushvalue(L, -1);
  lua_setglobal(L, "__resize_desc_vm_table");
#if LJ_HASJIT
  /*
  ** Build a pre-MT trace while the raw mt_active word is exactly zero. Such a
  ** trace is allowed to contain stock raw ASTORE/HSTORE instructions, so the
  ** first descriptor guard must retire it before publishing table control.
  */
  assert(luaL_dostring(
    L, "jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1'); "
       "for i=1,200 do "
       "__resize_desc_vm_table[1]=i; "
       "__resize_desc_vm_table.existing=i "
       "end") == 0);
  assert(lj_trace_hasany(g));
#endif
  assert(mt_resize_guard_count_acq(g) == 0);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_gc2_smr_read_try(g));
  readers = gc2_smr_readers_acq(g);
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(gc2_smr_readers_acq(g) == readers);
  assert(mt_resize_guard_count_acq(g) == 1);
#if LJ_HASJIT
  assert(!lj_trace_hasany(g));
#endif

  /*
  ** Real interpreter TSETB/TSETS bytecode must also reject its raw private
  ** store while table control carries a descriptor tag.
  */
  lj_tab_test_reset_vm_array_store_calls();
  lj_tab_test_reset_vm_strhash_store_calls();
  assert(luaL_loadstring(
    L, "__resize_desc_vm_table[1] = 66; "
       "__resize_desc_vm_table.existing = 77") == 0);
  assert(lua_pcall(L, 0, 0, 0) == 0);
  assert(lj_tab_test_vm_array_store_calls() >= 1u);
  assert(lj_tab_test_vm_strhash_store_calls() >= 1u);

  /* Missing hash keys must bypass the private claimant while control is tagged. */
  lua_pushinteger(L, 33);
  lua_setfield(L, -2, "descriptor-newkey");

  setintV(&src[0], 44);
  setintV(&src[1], 55);
  lj_tab_test_reset_tsetm_fast_calls();
  lj_tab_storetvn_forvm_array(L, t, 3, src, 2);
  assert(lj_tab_test_tsetm_fast_calls() == 0);

  finish_installed(g, t, desc, acap);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  lua_getfield(L, -1, "descriptor-newkey");
  assert(lua_tointeger(L, -1) == 33);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 3);
  assert(lua_tointeger(L, -1) == 44);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 4);
  assert(lua_tointeger(L, -1) == 55);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 1);
  assert(lua_tointeger(L, -1) == 66);
  lua_pop(L, 1);
  lua_getfield(L, -1, "existing");
  assert(lua_tointeger(L, -1) == 77);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "__resize_desc_vm_table");
  lua_pop(L, 1);
}

static void exercise_vm_guard_token_contention(lua_State *L)
{
#if LJ_HASJIT
  global_State *g = G(L);
  jit_State *J = L2J(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;
  uint32_t tid = lj_tg_tid_acq(L2TG(L));

  assert(mt_active_acq(g) == 0);
  assert(mt_resize_guard_count_acq(g) == 0);
  assert(jit_owner_word_acq(g) == 0);
  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);

  /*
  ** Model a same-TG VM-event lifecycle reservation: the token lane is zero,
  ** but a waiting token acquisition would spin and may throw STOPREQ. The
  ** descriptor path must refuse boundedly, terminalize its claimed record and
  ** release both its internal reader and packed guard.
  */
  assert(lj_gc2_smr_read_try(g));
  jit_owner_test_rel(g, 0, tid);
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(mt_resize_guard_count_acq(g) == 0);
  assert((lj_tab_resize_desc_flags_acq(desc) &
	  (TAB_RESIZE_DESC_F_VM_GUARD |
	   TAB_RESIZE_DESC_F_VM_GUARD_RELEASED)) ==
	 (TAB_RESIZE_DESC_F_VM_GUARD |
	  TAB_RESIZE_DESC_F_VM_GUARD_RELEASED));
  jit_owner_test_rel(g, 0, 0);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);

  /*
  ** A same-state recorder owns the token legitimately. The bounded flush must
  ** refuse without aborting or dismantling its active compiler state.
  */
  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  lj_trace_state_store(J, LJ_TRACE_RECORD);
  assert(lj_gc2_smr_read_try(g));
  assert(!lj_tab_resize_desc_install(L, t, desc));
  assert(lj_trace_state_load(J) == LJ_TRACE_RECORD);
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_TERMINAL);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_owner_word_acq(g) == 0);
  lua_pop(L, 1);
#else
  UNUSED(L);
#endif
}

static void exercise_overlapping_vm_guards(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *first, *second;
  GCtab *t1, *t2;
  uint32_t acap1, acap2;

  assert(mt_resize_guard_count_acq(g) == 0);
  lua_createtable(L, 4, 4);
  t1 = tabV(L->top - 1);
  lua_createtable(L, 4, 4);
  t2 = tabV(L->top - 1);
  acap1 = (uint32_t)lj_tab_acap_acq(t1);
  acap2 = (uint32_t)lj_tab_acap_acq(t2);
  first = lj_tab_resize_desc_reserve(L, t1, acap1);
  second = lj_tab_resize_desc_reserve(L, t2, acap2);

  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t1, first));
  assert(mt_resize_guard_count_acq(g) == 1);
  assert(lj_tab_resize_desc_install(L, t2, second));
  assert(mt_resize_guard_count_acq(g) == 2);
  finish_installed(g, t1, first, acap1);
  assert(mt_resize_guard_count_acq(g) == 1);
  finish_installed(g, t2, second, acap2);
  assert(mt_resize_guard_count_acq(g) == 0);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 2);
}

static void exercise_reclamation(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc;
  GCtab *t;
  uint64_t id, retire_epoch;
  uint32_t acap;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  id = lj_tab_resize_desc_id_acq(desc);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_tab_resize_desc_install(L, t, desc));
  finish_installed(g, t, desc, acap);
  retire_epoch = lj_tab_resize_desc_epoch_acq(desc);
  lj_gc2_smr_read_leave(g);

  /*
  ** Exercise each ineligible age frontier without an outer reader. Epoch zero
  ** has no callable exact-zero frontier, so its first meaningful check is +1.
  */
  if (retire_epoch != 0) {
    (void)lj_gc2_reclaim_retired(g, retire_epoch);
    assert(lj_gc2_smr_read_try(g));
    assert(find_desc_held(g, t, id) == desc);
    lj_gc2_smr_read_leave(g);
  }
  (void)lj_gc2_reclaim_retired(g, retire_epoch + 1u);
  assert(lj_gc2_smr_read_try(g));
  assert(find_desc_held(g, t, id) == desc);
  lj_gc2_smr_read_leave(g);

  /*
  ** At the first eligible frontier, an independently held reader must still
  ** veto the exact reclaimer. Leaving that reader makes the same frontier
  ** sufficient to unlink and free the terminal record.
  */
  assert(lj_gc2_smr_read_try(g));
  (void)lj_gc2_reclaim_retired(
    g, retire_epoch + LJ_TAB_RETIRE_EPOCHS);
  assert(find_desc_held(g, t, id) == desc);
  lj_gc2_smr_read_leave(g);
  (void)lj_gc2_reclaim_retired(
    g, retire_epoch + LJ_TAB_RETIRE_EPOCHS);
  assert(lj_gc2_smr_read_try(g));
  assert(find_desc_held(g, t, id) == NULL);
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_registry_and_lifetime(lua_State *L)
{
  global_State *g = G(L);
  TabResizeDesc *desc, *discard, *busy;
  GCtab *t, *other;
  uint64_t id, busyid;
  uint64_t prior, expect, installing;
  uint32_t acap, owner;

  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  lua_createtable(L, 0, 0);
  other = tabV(L->top - 1);

  desc = lj_tab_resize_desc_reserve(L, t, acap);
  discard = lj_tab_resize_desc_reserve(L, t, acap);
  id = lj_tab_resize_desc_id_acq(desc);
  assert(id != 0 && id < lj_tab_resize_desc_id_acq(discard));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_PREPARED);
  assert(lj_tab_resize_desc_next_acq(desc) == NULL);
  assert(lj_tab_resize_desc_discard(g, discard));
  assert(lj_tab_resize_desc_phase_acq(discard) ==
	 TAB_RESIZE_DESC_TERMINAL);

  assert(lj_gc2_smr_read_try(g));
  assert(find_desc_held(g, t, id) == NULL);
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_flags_acq(desc) &
	 TAB_RESIZE_DESC_F_PUBLISHED);
  assert(find_desc_held(g, t, id) == desc);
  assert(find_desc_held(g, t, id + 1u) == discard);
  assert(find_desc_held(g, other, id) == NULL);

  assert(lj_tab_resize_desc_control_acq(t) == desc);
  assert(lj_tab_acap_acq(t) == acap);
  assert(lj_tab_struct_owner_acq(t) == LJ_TAB_STRUCT_DESC_OWNER);
  owner = 0;
  assert(!lj_tab_struct_owner_cas(t, &owner, 1));
  assert(owner == LJ_TAB_STRUCT_DESC_OWNER);
  assert(lj_tab_resize_desc_install(L, t, desc));

  /*
  ** Weak-record state and active resize capacity share one physical word.
  ** Semantic weak CAS must preserve the authoritative capacity bits.
  */
  prior = lj_tab_weak_record_acq(t);
  installing = lj_tab_weak_record_pack(
    lj_tab_weak_record_cycle(prior) + 1u, LJ_TAB_WEAK_RECORD_INSTALLING);
  expect = prior;
  assert(lj_tab_weak_record_cas(t, &expect, installing));
  assert(lj_tab_acap_acq(t) == acap);
  expect = installing;
  assert(lj_tab_weak_record_cas(t, &expect, prior));
  assert(lj_tab_acap_acq(t) == acap);

  assert(!lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_PREPARED,
				     TAB_RESIZE_DESC_INSTALLING));
  assert(!lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLING,
				     TAB_RESIZE_DESC_INSTALLED));
  assert(!lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_PREPARED,
				     TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_INSTALLED,
				    TAB_RESIZE_DESC_RETIRING));
  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_RETIRING,
				    TAB_RESIZE_DESC_MIGRATING));
  lj_gc2_smr_read_leave(g);

  /*
  ** Drop every ordinary table root. Registry traversal must retain both the
  ** raw descriptor and its semantic GCtab edge through a complete cycle.
  */
  lua_settop(L, 0);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lj_gc2_smr_read_try(g));
  assert(lj_gc2_mem_registered_known(g, desc));
  assert(lj_gc2_mem_registered_known(g, t));
  desc = find_desc_held(g, t, id);
  assert(desc != NULL);

  assert(lj_tab_resize_desc_advance(desc, TAB_RESIZE_DESC_MIGRATING,
				    TAB_RESIZE_DESC_PUBLISHING));
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(lj_tab_struct_owner_acq(t) == 0);
  assert(lj_tab_acap_acq(t) == acap);
  assert(lj_tab_resize_desc_clear(t, desc, acap));
  assert(lj_tab_resize_desc_terminal(g, desc,
				      TAB_RESIZE_DESC_TERMINATING));
  assert(lj_tab_resize_desc_phase_acq(desc) == TAB_RESIZE_DESC_TERMINAL);
  assert(find_desc_held(g, t, id) == desc);
  lj_gc2_smr_read_leave(g);

  /*
  ** Once install claims PREPARED, even a busy-table rejection is registry
  ** owned and terminal. Failure therefore has no ambiguous private record.
  */
  lua_createtable(L, 0, 0);
  other = tabV(L->top - 1);
  busy = lj_tab_resize_desc_reserve(
    L, other, (uint32_t)lj_tab_acap_acq(other));
  busyid = lj_tab_resize_desc_id_acq(busy);
  owner = 0;
  assert(lj_tab_struct_owner_cas(other, &owner, LJ_THREAD_STRUCT));
  assert(lj_gc2_smr_read_try(g));
  assert(!lj_tab_resize_desc_install(L, other, busy));
  lj_tab_struct_owner_rel(other, 0);
  assert(lj_tab_resize_desc_phase_acq(busy) == TAB_RESIZE_DESC_TERMINAL);
  assert(find_desc_held(g, other, busyid) == busy);
  assert(!lj_tab_resize_desc_clear(
    other, busy, (uint32_t)lj_tab_acap_acq(other)));
  lj_gc2_smr_read_leave(g);
  lua_pop(L, 1);
}

static void exercise_close_active_descriptor(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TabResizeDesc *desc;
  GCtab *t;
  uint32_t acap;

  assert(L != NULL);
  g = G(L);
  lua_createtable(L, 4, 4);
  t = tabV(L->top - 1);
  acap = (uint32_t)lj_tab_acap_acq(t);
  desc = lj_tab_resize_desc_reserve(L, t, acap);
  assert(lj_tab_resize_desc_install(L, t, desc));
  assert(lj_tab_resize_desc_phase_acq(desc) ==
	 TAB_RESIZE_DESC_INSTALLED);
  assert(lj_tab_resize_desc_control_acq(t) == desc);
  assert(mt_resize_guard_count_acq(g) == 1);
  /*
  ** Terminal teardown must detach descriptor control and release the packed
  ** guard before freeing either the descriptor or its semantic table edge.
  */
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  exercise_marker_encoding();
  exercise_packed_vm_guard_word();
  exercise_cross_universe_tracked_smr(L);
  exercise_install_pause_points(L);
  exercise_install_competing_generation(L);
  exercise_install_weak_update(L);
  exercise_stale_clear(L);
  exercise_distinct_capacity_cutover(L);
  exercise_displaced_clear(L);
  exercise_precutover_displaced_clear(L);
  exercise_structural_help(L);
  exercise_structural_mutation_gates(L);
  exercise_vm_guard_token_contention(L);
  exercise_private_store_gates(L);
  exercise_overlapping_vm_guards(L);
  exercise_reclamation(L);
  exercise_registry_and_lifetime(L);
  assert(mt_resize_guard_count_acq(G(L)) == 0);
  lua_close(L);
  exercise_close_active_descriptor();
  puts("t-tab-resize-descriptor OK: persistent ids survive GC and enter SMR retirement");
  return 0;
}
