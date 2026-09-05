/*
** Focused runtime contract for the production GC2 scalar table-store guard.
**
** This fixture is built with LJ_GC2_TEST_HELPERS and LJ_TAB_TEST_HELPERS.
** Test-only pauses expose exact-CAS and post-store handoff linearization
** points. This covers the guard substrate, not keyed-CAS/lease integration.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif
#ifndef LJ_TAB_TEST_HELPERS
#define LJ_TAB_TEST_HELPERS
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct Fixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCtab *parent;
  GCtab *value_tab;
  TValue parent_tv;
  TValue key;
  TValue value;
} Fixture;

typedef struct ResourceSnap {
  uint64_t local_total;
  uint64_t alloc_total;
  uint64_t alloc_since;
  uint32_t table_waits;
} ResourceSnap;

static Fixture fixture_open(void)
{
  Fixture f;
  memset(&f, 0, sizeof(f));
  f.L = luaL_newstate();
  assert(f.L != NULL);
  f.g = G(f.L);
  f.tg = L2TG(f.L);
  assert(f.g != NULL && f.tg != NULL);

  lua_newtable(f.L);
  f.parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  f.value_tab = tabV(f.L->top - 1);
  settabV(f.L, &f.parent_tv, f.parent);
  setintV(&f.key, 37);
  settabV(f.L, &f.value, f.value_tab);
  assert(lj_tab_gc2_rescan_state_acq(f.parent) == LJ_TAB_RESCAN_NONE);
  assert(lj_gc2_rootdesc_snapshot(&f.tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return f;
}

static void fixture_close(Fixture *f)
{
  assert(lj_gc2_rootdesc_snapshot(&f->tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  lua_close(f->L);
  memset(f, 0, sizeof(*f));
}

static ResourceSnap resource_snapshot(const Fixture *f)
{
  ResourceSnap snap;
  snap.local_total = lj_tg_local_total_acq(f->tg);
  snap.alloc_total = gc2_alloc_total_bytes_acq(f->g);
  snap.alloc_since = lj_gc2_alloc_since_load(f->g);
  snap.table_waits = lj_tab_test_wait_no_l_calls();
  return snap;
}

static void assert_resources_unchanged(const Fixture *f,
                                       const ResourceSnap *before)
{
  ResourceSnap after = resource_snapshot(f);
  assert(after.local_total == before->local_total);
  assert(after.alloc_total == before->alloc_total);
  assert(after.alloc_since == before->alloc_since);
  assert(after.table_waits == before->table_waits);
}

static void assert_guard_resources_live(const LJGC2TableStoreGuard *guard,
                                        int key_lease, int value_lease,
                                        int weak)
{
  assert(guard->begun && !guard->finished && !guard->cleanup_failed);
  assert(guard->parent_lease_active);
  assert(guard->legacy_carrier ? !guard->tg_borrow.active :
                                 guard->tg_borrow.active);
  assert(!!guard->key_lease_active == !!key_lease);
  assert(!!guard->value_lease_active == !!value_lease);
  assert(!!guard->weak_active == !!weak);
}

static void assert_guard_resources_released(const LJGC2TableStoreGuard *guard)
{
  assert(guard->finished && !guard->cleanup_failed);
  assert(!guard->tg_borrow.active && !guard->parent_lease_active);
  assert(!guard->key_lease_active && !guard->value_lease_active);
  assert(!guard->weak_active);
}

static void repair_activation_idle(Fixture *f)
{
  LJGC2ActivationSnap activation =
    lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(lj_gc2_activation_init_unpublished(&f->g->gc2.activation,
                                             activation.mark_epoch,
                                             activation.generation,
                                             LJ_GC2_ACT_IDLE));
}

static void fixture_make_parent_weak(Fixture *f)
{
  lua_newtable(f->L);
  lua_pushliteral(f->L, "__mode");
  lua_pushliteral(f->L, "kv");
  lua_rawset(f->L, -3);
  assert(lua_setmetatable(f->L, 1));
  assert(lj_gc2_weak_write_candidate(f->L, f->parent) != 0);
}

static LJGC2ActivationSnap gate_move(global_State *g, uint8_t next_gate)
{
  LJGC2ActivationSnap from = lj_gc2_activation_snapshot(&g->gc2.activation);
  LJGC2ActivationSnap to;
  assert(lj_gc2_activation_try_gate(&g->gc2.activation, &from, next_gate,
                                     &to) == LJ_GC2_TRANSITION_OK);
  return to;
}

static void gate_reopen(global_State *g)
{
  LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(&g->gc2.activation);
  if (snap.gate != LJ_GC2_ROOT_GATE_OPEN)
    (void)gate_move(g, LJ_GC2_ROOT_GATE_OPEN);
  snap = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(snap.gate == LJ_GC2_ROOT_GATE_OPEN);
}

static void assert_descriptor_active_payload(const Fixture *f,
                                             uint64_t parent,
                                             uint64_t key,
                                             uint64_t value)
{
  LJGC2RootDescView view;
  uint32_t flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
                   LJ_GC2_ROOTDESC_F_AUX |
                   LJ_GC2_ROOTDESC_F_TABLE_STORE;
  memset(&view, 0, sizeof(view));
  assert(lj_gc2_rootdesc_snapshot(&f->tg->root_desc, &view) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(view.descriptor == &f->tg->root_desc);
  assert(view.flags == flags);
  assert(view.old_root == parent);
  assert(view.new_root == key);
  assert(view.aux_root == value);
}

static void assert_descriptor_idle(const Fixture *f)
{
  assert(lj_gc2_rootdesc_snapshot(&f->tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static void test_by_value_capture_and_open_admission(void)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  TValue key = f.key;
  TValue value = f.value;
  uint64_t parent_raw = tv_rawload(&f.parent_tv);
  uint64_t key_raw = tv_rawload(&key);
  uint64_t value_raw = tv_rawload(&value);
  ResourceSnap before;

  memset(&guard, 0xa5, sizeof(guard));
  lj_tab_test_reset_wait_no_l_calls();
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent, &key, &value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!guard.store_authorized && !guard.gate_admitted &&
         !guard.gate_revalidated);

  /* By-value capture plus retained physical/semantic authorities let the
  ** caller overwrite its scratch operands immediately after begin. */
  setnilV(&key);
  setnilV(&value);
  assert(guard.g == f.g && guard.tg == f.tg && guard.parent_tab == f.parent);
  assert(tv_rawload(&guard.parent) == parent_raw);
  assert(tv_rawload(&guard.key) == key_raw);
  assert(tv_rawload(&guard.value) == value_raw);
  assert(guard.active && guard.tg_borrow.active && guard.parent_lease_active &&
         guard.value_lease_active && !guard.key_lease_active);
  assert_descriptor_active_payload(&f, parent_raw, key_raw, value_raw);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && guard.gate_revalidated &&
         guard.store_authorized);
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_descriptor_idle(&f);
  assert(lj_tab_gc2_rescan_state_acq(f.parent) == LJ_TAB_RESCAN_NONE);
  assert_resources_unchanged(&f, &before);
  fixture_close(&f);
}

static void run_admission_at_gate(uint8_t initial_gate)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  ResourceSnap before;
  LJGC2ActivationSnap snap;

  if (initial_gate == LJ_GC2_ROOT_GATE_PENDING) {
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_CLOSING);
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_PENDING);
  } else if (initial_gate == LJ_GC2_ROOT_GATE_CLOSING) {
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_CLOSING);
  } else if (initial_gate == LJ_GC2_ROOT_GATE_COMMIT) {
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_CLOSING);
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_COMMIT);
  } else {
    assert(initial_gate == LJ_GC2_ROOT_GATE_OPEN);
  }

  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!guard.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  snap = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  if (initial_gate == LJ_GC2_ROOT_GATE_CLOSING ||
      initial_gate == LJ_GC2_ROOT_GATE_COMMIT)
    assert(snap.gate == LJ_GC2_ROOT_GATE_PENDING);
  else
    assert(snap.gate == initial_gate);
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_resources_unchanged(&f, &before);
  gate_reopen(f.g);
  fixture_close(&f);
}

static void run_revalidation_at_gate(uint8_t gate)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  ResourceSnap before = resource_snapshot(&f);
  LJGC2ActivationSnap snap;

  assert(gate == LJ_GC2_ROOT_GATE_CLOSING ||
         gate == LJ_GC2_ROOT_GATE_COMMIT);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!guard.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.store_authorized);
  (void)gate_move(f.g, LJ_GC2_ROOT_GATE_CLOSING);
  if (gate == LJ_GC2_ROOT_GATE_COMMIT)
    (void)gate_move(f.g, LJ_GC2_ROOT_GATE_COMMIT);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_revalidated && guard.store_authorized);
  snap = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(snap.gate == LJ_GC2_ROOT_GATE_PENDING);
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_resources_unchanged(&f, &before);
  gate_reopen(f.g);
  fixture_close(&f);
}

static void wait_gate_pause(void)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_table_store_gate_pause_waiting())
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"table-store guard gate hook did not pause");
}

typedef struct GateRaceCtx {
  global_State *g;
} GateRaceCtx;

static void *gate_replacement_thread(void *ud)
{
  GateRaceCtx *ctx = (GateRaceCtx *)ud;
  wait_gate_pause();
  /* This peer owns no Lua/TG state. It changes only the exact global gate. */
  (void)gate_move(ctx->g, LJ_GC2_ROOT_GATE_COMMIT);
  lj_gc2_test_table_store_gate_pause_release();
  return NULL;
}

static void run_lost_gate_retry(int revalidate)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  GateRaceCtx ctx;
  pthread_t thread;
  ResourceSnap before = resource_snapshot(&f);
  LJGC2ActivationSnap snap;
  LJGC2TableStoreGuardResult result;

  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!guard.store_authorized);
  if (revalidate)
    assert(lj_gc2_table_store_admit(f.L, &guard) ==
           LJ_GC2_TABLE_STORE_GUARD_OK);
  if (revalidate)
    assert(guard.gate_admitted && !guard.store_authorized);
  (void)gate_move(f.g, LJ_GC2_ROOT_GATE_CLOSING);
  ctx.g = f.g;

  lj_gc2_test_table_store_gate_pause_arm();
  assert(pthread_create(&thread, NULL, gate_replacement_thread, &ctx) == 0);
  /* The established L/TG owner executes the guard operation. The test hook
  ** pauses it after sampling CLOSING; the peer replaces only global authority.
  ** Its first CLOSING->PENDING CAS must then lose, refresh, and route
  ** COMMIT->PENDING. */
  result = revalidate ?
    lj_gc2_table_store_revalidate(f.L, &guard) :
    lj_gc2_table_store_admit(f.L, &guard);
  assert(pthread_join(thread, NULL) == 0);
  assert(result == LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!!guard.store_authorized == !!revalidate);
  snap = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(snap.gate == LJ_GC2_ROOT_GATE_PENDING);
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_resources_unchanged(&f, &before);
  gate_reopen(f.g);
  fixture_close(&f);
}

static void test_ticket_pin_before_revalidate(void)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  LJGC2ActivationSnap activation;
  ResourceSnap before = resource_snapshot(&f);
  uint64_t desc_control;
  uint64_t desc_generation;

  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.active && !guard.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  desc_control = guard.ticket.control;
  assert(desc_control != 0 &&
         la_load64_acq(&f.tg->root_desc.control) == desc_control);

  assert(lj_gc2_rootdesc_pin(&f.tg->root_desc, desc_control) ==
         LJ_GC2_ROOTDESC_PINNED);
  assert(lj_gc2_rootdesc_snapshot(&f.tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  activation = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  /* Documented conservative divergence: loss of the exact descriptor may
  ** authorize only after the owner observes sticky global NO_RECLAIM while
  ** every physical/weak authority remains retained. */
  assert(guard.gate_admitted && guard.gate_revalidated &&
         guard.store_authorized && guard.globally_pinned);
  assert_guard_resources_live(&guard, 0, 1, 0);

  /* No heap CAS was attempted in this fixture, so every post-begin exit still
  ** closes through the original owner with cas_committed=false. */
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert_guard_resources_released(&guard);
  assert(!guard.active);
  assert(lj_gc2_rootdesc_snapshot(&f.tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_NO_RECLAIM);
  assert_resources_unchanged(&f, &before);

  desc_generation = lj_gc2_rootdesc_generation(
    la_load64_acq(&f.tg->root_desc.control));
  assert(lj_gc2_rootdesc_init_unpublished(&f.tg->root_desc,
                                           desc_generation));
  repair_activation_idle(&f);
  fixture_close(&f);
}

typedef struct ForeignRevalidateCtx {
  lua_State *L;
  LJGC2TableStoreGuard *guard;
  LJGC2TableStoreGuardResult result_null_raw;
  LJGC2TableStoreGuardResult result_other_raw;
} ForeignRevalidateCtx;

static void *foreign_revalidate_thread(void *ud)
{
  ForeignRevalidateCtx *ctx = (ForeignRevalidateCtx *)ud;
  lua_State *other;
  assert(lj_thr_get_tg() == NULL);
  ctx->result_null_raw =
    lj_gc2_table_store_revalidate(ctx->L, ctx->guard);
  other = luaL_newstate();
  assert(other != NULL && lj_thr_get_tg() == G(other)->main_tg);
  ctx->result_other_raw =
    lj_gc2_table_store_revalidate(ctx->L, ctx->guard);
  lua_close(other);
  assert(lj_thr_get_tg() == NULL);
  return NULL;
}

static void test_wrong_owner_revalidate_cleanup(void)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard, guard_before;
  ForeignRevalidateCtx ctx;
  LJGC2ActivationSnap activation;
  ResourceSnap before;
  pthread_t thread;
  uint64_t desc_control;
  uint32_t weak_before;

  fixture_make_parent_weak(&f);
  weak_before = gc2_weak_write_active_acq(f.g);
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_guard_resources_live(&guard, 0, 1, 1);
  assert(gc2_weak_write_active_acq(f.g) == weak_before + 1u);
  assert(!guard.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  desc_control = guard.ticket.control;
  memcpy(&guard_before, &guard, sizeof(guard_before));

  memset(&ctx, 0, sizeof(ctx));
  ctx.L = f.L;
  ctx.guard = &guard;
  assert(pthread_create(&thread, NULL, foreign_revalidate_thread, &ctx) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.result_null_raw == LJ_GC2_TABLE_STORE_GUARD_INVALID);
  assert(ctx.result_other_raw == LJ_GC2_TABLE_STORE_GUARD_INVALID);
  /* The foreign actor may pin only global authority. It cannot write even a
  ** status bit in the established owner's linear guard. */
  assert(memcmp(&guard, &guard_before, sizeof(guard)) == 0);
  activation = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(la_load64_acq(&f.tg->root_desc.control) == desc_control);
  assert(lj_gc2_rootdesc_snapshot(&f.tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(gc2_weak_write_active_acq(f.g) == weak_before + 1u);

  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert_guard_resources_released(&guard);
  assert(!guard.store_authorized && !guard.active);
  assert(gc2_weak_write_active_acq(f.g) == weak_before);
  assert_descriptor_idle(&f);
  assert_resources_unchanged(&f, &before);
  repair_activation_idle(&f);
  fixture_close(&f);
}

static void finish_rejected_stage(Fixture *f, LJGC2TableStoreGuard *guard,
                                  const ResourceSnap *before)
{
  assert(!guard->store_authorized && guard->globally_pinned);
  assert(lj_gc2_table_store_finish(f->L, guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert_guard_resources_released(guard);
  assert_descriptor_idle(f);
  assert_resources_unchanged(f, before);
  repair_activation_idle(f);
  fixture_close(f);
}

static void test_stage_order_rejection(void)
{
  Fixture f;
  LJGC2TableStoreGuard guard;
  ResourceSnap before;

  f = fixture_open();
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!guard.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_INVALID);
  assert(!guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  finish_rejected_stage(&f, &guard, &before);

  f = fixture_open();
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.gate_admitted && !guard.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_INVALID);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  finish_rejected_stage(&f, &guard, &before);

  f = fixture_open();
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(guard.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_INVALID);
  assert(guard.gate_admitted && guard.gate_revalidated &&
         !guard.store_authorized);
  finish_rejected_stage(&f, &guard, &before);
}

static void assert_rejected_begin(Fixture *f, LJGC2TableStoreGuard *guard,
                                  const ResourceSnap *before)
{
  assert(guard->globally_pinned && !guard->begun && !guard->active);
  assert(!guard->gate_admitted && !guard->gate_revalidated &&
         !guard->store_authorized);
  assert_guard_resources_released(guard);
  assert_descriptor_idle(f);
  assert_resources_unchanged(f, before);
  repair_activation_idle(f);
  fixture_close(f);
}

static void test_malformed_parent_and_tvalue_rejected(void)
{
  Fixture f;
  LJGC2TableStoreGuard guard;
  ResourceSnap before;
  GCstr *not_table;
  TValue forged;

  f = fixture_open();
  lua_pushliteral(f.L, "guard parent type mismatch");
  not_table = strV(f.L->top - 1);
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, (GCtab *)not_table,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_RETRY);
  assert_rejected_begin(&f, &guard, &before);

  f = fixture_open();
  lua_pushliteral(f.L, "guard TValue type mismatch");
  not_table = strV(f.L->top - 1);
  setgcVraw(&forged, obj2gco(not_table), LJ_TTAB);
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &forged, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_RETRY);
  assert_rejected_begin(&f, &guard, &before);
}

static void wait_table_rescan_pause(uint32_t stage)
{
  uint32_t spin;
  for (spin = 0; spin < 1000000u; spin++) {
    if (lj_gc2_test_table_rescan_paused() == stage)
      return;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"table rescan handoff hook did not pause");
}

typedef struct FinishObserveCtx {
  Fixture *fixture;
  uint64_t stamp_state;
  uint8_t saw_active;
  uint8_t saw_installing;
} FinishObserveCtx;

static void *successful_finish_observer(void *ud)
{
  FinishObserveCtx *ctx = (FinishObserveCtx *)ud;
  wait_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  /* Remote inspection is helper-shaped and never mutates owner-local SSB. */
  ctx->saw_active =
    lj_gc2_rootdesc_snapshot(&ctx->fixture->tg->root_desc, NULL) ==
      LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE;
  ctx->saw_installing =
    lj_tab_gc2_rescan_state_acq(ctx->fixture->parent) ==
      LJ_TAB_RESCAN_INSTALLING;
  ctx->stamp_state = la_load64_acq(
    &lj_arena_gc2_stamp_acq(ctx->fixture->parent)->state);
  lj_gc2_test_table_rescan_release();
  return NULL;
}

static void test_finish_ordering(void)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard failed, committed;
  FinishObserveCtx ctx;
  pthread_t thread;
  ResourceSnap before = resource_snapshot(&f);
  LJGC2TableStoreGuardResult result;
  LJGC2TabStamp *stamp = lj_arena_gc2_stamp_acq(f.parent);
  uint32_t dirty_before;
  uint64_t stamp_before;
  uint64_t stamp_committed;

  assert(stamp != NULL);
  dirty_before = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty_before != UINT32_MAX);
  stamp_before = (UINT64_C(23) << 32) | dirty_before;
  stamp_committed = (uint64_t)(dirty_before + 1u);
  la_store64_rel(&stamp->state, stamp_before);

  /* A failed heap CAS created no edge, so it may clear ACTIVE without entering
  ** the conservative parent-rescan handoff. */
  assert(lj_gc2_table_store_begin(f.L, &failed, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!failed.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &failed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!failed.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &failed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(failed.store_authorized);
  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(lj_gc2_table_store_finish(f.L, &failed, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(lj_gc2_test_table_rescan_paused() == 0);
  assert_descriptor_idle(&f);
  assert(lj_tab_gc2_rescan_state_acq(f.parent) == LJ_TAB_RESCAN_NONE);
  assert(la_load64_acq(&stamp->state) == stamp_before);
  lj_gc2_test_table_rescan_release();

  /* A successful heap CAS retains ACTIVE until its forced parent rescan is
  ** durably installing. The pause is inside that real handoff, before finish. */
  assert(lj_gc2_table_store_begin(f.L, &committed, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!committed.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &committed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!committed.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &committed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(committed.store_authorized);
  memset(&ctx, 0, sizeof(ctx));
  ctx.fixture = &f;
  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(pthread_create(&thread, NULL, successful_finish_observer, &ctx) == 0);
  result = lj_gc2_table_store_finish(f.L, &committed, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(result == LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(ctx.saw_active && ctx.saw_installing);
  assert(ctx.stamp_state == stamp_committed);
  assert_descriptor_idle(&f);
  assert(lj_tab_gc2_rescan_state_acq(f.parent) == LJ_TAB_RESCAN_COUNTED);
  assert(la_load64_acq(&stamp->state) == stamp_committed);
  assert_resources_unchanged(&f, &before);

  /* Consume the exact legacy migration token before normal state teardown. */
  assert(lj_gc2_test_table_rescan_clear(f.g, f.parent) == 1);
  fixture_close(&f);
}

static LJGC2ActivationSnap enter_sweep_fixture(Fixture *f)
{
  LJGC2ActivationSnap idle, mark, weak, sweep;
  uint64_t epoch;

  assert(gc2_phase_acq(f->g) == LJ_GC2_IDLE);
  idle = lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(idle.state == LJ_GC2_ACT_IDLE &&
         idle.gate == LJ_GC2_ROOT_GATE_OPEN);
  assert(idle.mark_epoch != UINT64_MAX);
  epoch = idle.mark_epoch + 1u;
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &idle,
           epoch, LJ_GC2_ACT_MARK, &mark) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_MARK);

  /* A real cycle reaches SWEEP only after roots are retained. Mark the two
  ** empty fixture tables so the SWEEP store exercises dirty/rescan ordering,
  ** rather than manufacturing unrelated recovery work. */
  (void)lj_gc2_markobj(f->g, obj2gco(f->parent));
  (void)lj_gc2_markobj(f->g, obj2gco(f->value_tab));
  assert(lj_gc2_ismarked(f->g, obj2gco(f->parent)) == 1);
  assert(lj_gc2_ismarked(f->g, obj2gco(f->value_tab)) == 1);

  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &mark,
           epoch, LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_WEAK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &weak,
           epoch, LJ_GC2_ACT_SWEEP_OPEN, &sweep) ==
         LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_SWEEP);
  assert(gc2_phase_acq(f->g) == LJ_GC2_SWEEP);
  return sweep;
}

static void leave_sweep_fixture(Fixture *f,
                                const LJGC2ActivationSnap *sweep)
{
  LJGC2ActivationSnap idle;
  assert(gc2_phase_acq(f->g) == LJ_GC2_SWEEP);
  gc2_phase_rel(f->g, LJ_GC2_IDLE);
  assert(lj_gc2_activation_try_abandon_sweep_open(
           &f->g->gc2.activation, sweep, &idle) == LJ_GC2_TRANSITION_OK);
  assert(idle.state == LJ_GC2_ACT_IDLE &&
         idle.gate == LJ_GC2_ROOT_GATE_OPEN);
}

static void test_sweep_finish_dirty_ordering(void)
{
  Fixture f = fixture_open();
  LJGC2ActivationSnap sweep = enter_sweep_fixture(&f);
  LJGC2TableStoreGuard failed, committed;
  FinishObserveCtx ctx;
  LJGC2TableStoreGuardResult result;
  LJGC2TabStamp *stamp = lj_arena_gc2_stamp_acq(f.parent);
  ResourceSnap before;
  pthread_t thread;
  uint32_t dirty_before;
  uint64_t stamp_before;
  uint64_t stamp_admitted;
  uint64_t stamp_committed;

  assert(stamp != NULL);
  dirty_before = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty_before < UINT32_MAX - 6u);
  stamp_before = (UINT64_C(71) << 32) | dirty_before;
  la_store64_rel(&stamp->state, stamp_before);
  before = resource_snapshot(&f);

  assert(lj_gc2_table_store_begin(f.L, &failed, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!failed.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &failed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!failed.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &failed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(failed.store_authorized);
  /* Begin semantically publishes the parent twice: its object lease, then
  ** the by-value root preceding ACTIVE. Each invalidates before its SSB LP.
  ** Failed finish itself must still leave the admitted stamp unchanged. */
  stamp_admitted = (uint64_t)(dirty_before + 2u);
  assert(la_load64_acq(&stamp->state) == stamp_admitted);
  assert(lj_gc2_table_store_finish(f.L, &failed, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(la_load64_acq(&stamp->state) == stamp_admitted);
  assert_descriptor_idle(&f);

  assert(lj_gc2_table_store_begin(f.L, &committed, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!committed.store_authorized);
  assert(lj_gc2_table_store_admit(f.L, &committed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(!committed.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &committed) ==
         LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(committed.store_authorized);
  stamp_admitted = (uint64_t)(dirty_before + 4u);
  assert(la_load64_acq(&stamp->state) == stamp_admitted);
  /* Finish dirties after the committed store, then the independent public
  ** SWEEP barrier invalidates before publishing its rescue request. Both
  ** precede INSTALLING; neither may retain the old covered cycle. */
  stamp_committed = stamp_admitted + 2u;

  memset(&ctx, 0, sizeof(ctx));
  ctx.fixture = &f;
  lj_gc2_test_table_rescan_pause(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  assert(pthread_create(&thread, NULL, successful_finish_observer, &ctx) == 0);
  result = lj_gc2_table_store_finish(f.L, &committed, 1);
  assert(pthread_join(thread, NULL) == 0);
  assert(result == LJ_GC2_TABLE_STORE_GUARD_OK);
  assert(ctx.saw_active && ctx.saw_installing);
  /* SWEEP rescue is additional authority. It must not preserve the covered
  ** cycle or skip the dirty epoch increment. */
  assert(ctx.stamp_state == stamp_committed);
  assert(la_load64_acq(&stamp->state) == stamp_committed);
  assert_descriptor_idle(&f);
  assert(lj_tab_gc2_rescan_state_acq(f.parent) == LJ_TAB_RESCAN_COUNTED);
  assert_resources_unchanged(&f, &before);
  assert(lj_gc2_test_table_rescan_clear(f.g, f.parent) == 1);

  leave_sweep_fixture(&f, &sweep);
  fixture_close(&f);
}

static void run_legacy_carrier_guard(int incomplete_trigger,
                                     int shadow_missed_trigger)
{
  Fixture f = fixture_open();
  LJGC2TableStoreGuard guard;
  LJGC2ActivationSnap activation;
  ResourceSnap before;
  uint32_t saved_incomplete = gc2_tg_registry_incomplete_acq(f.g);
  uint8_t saved_shadow_missed =
    lj_tg_registry_shadow_missed_acq(f.tg);

  assert((incomplete_trigger != 0) != (shadow_missed_trigger != 0));
  assert(saved_incomplete == 0 && saved_shadow_missed == 0);
  if (incomplete_trigger)
    gc2_tg_registry_incomplete_store_rlx(f.g, 1);
  if (shadow_missed_trigger)
    lj_tg_registry_shadow_missed_rel(f.tg, 1);
  before = resource_snapshot(&f);

  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert(guard.legacy_carrier && guard.globally_pinned && !guard.active);
  assert(!guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  assert_guard_resources_live(&guard, 0, 1, 0);
  assert_descriptor_idle(&f);
  activation = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);

  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  assert(!guard.tg_borrow.active);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert(guard.gate_admitted && guard.gate_revalidated &&
         guard.store_authorized);
  assert(!guard.tg_borrow.active);

  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert_guard_resources_released(&guard);
  assert(!guard.active);
  assert_descriptor_idle(&f);
  assert_resources_unchanged(&f, &before);

  /* These predicates are sticky in production. This single-threaded fixture
  ** restores its synthetic trigger only after every fallback authority has
  ** been released, so normal state teardown can exercise its ordinary path. */
  lj_tg_registry_shadow_missed_rel(f.tg, saved_shadow_missed);
  gc2_tg_registry_incomplete_store_rlx(f.g, saved_incomplete);
  repair_activation_idle(&f);
  fixture_close(&f);
}

static void test_legacy_carrier_without_registry_borrow(void)
{
  /* Test each side of the production OR independently. The shadow-only case
  ** is a test reconstruction: a real shadow miss also makes the universe bit
  ** sticky, but independent coverage prevents either predicate disappearing. */
  run_legacy_carrier_guard(1, 0);
  run_legacy_carrier_guard(0, 1);
}

typedef struct SuspendedStoreObserveCtx {
  Fixture *fixture;
  lua_State *co;
  uint8_t saw_owner;
  uint8_t saw_hint;
  uint8_t saw_main_cur_L;
  uint8_t saw_active_descriptor;
} SuspendedStoreObserveCtx;

static void assert_suspended_state(const Fixture *f, const lua_State *co)
{
  assert(lj_state_owner_acq(co) == 0);
  assert(co->tg_hint == NULL);
  assert(lj_tg_load_cur_L(f->tg) == f->L);
}

static void *suspended_store_observer(void *ud)
{
  SuspendedStoreObserveCtx *ctx = (SuspendedStoreObserveCtx *)ud;
  wait_gate_pause();
  /* The pause's release/acquire handshake makes the temporary non-atomic
  ** tg_hint stable until this observer releases the admitted owner. */
  ctx->saw_owner =
    lj_state_owner_acq(ctx->co) == lj_tg_tid_acq(ctx->fixture->tg);
  ctx->saw_hint = ctx->co->tg_hint == ctx->fixture->tg;
  ctx->saw_main_cur_L =
    lj_tg_load_cur_L(ctx->fixture->tg) == ctx->fixture->L;
  ctx->saw_active_descriptor =
    lj_gc2_rootdesc_snapshot(&ctx->fixture->tg->root_desc, NULL) ==
      LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE;
  lj_gc2_test_table_store_gate_pause_release();
  return NULL;
}

static void run_suspended_public_store(Fixture *f, lua_State *co,
                                       int hash_store)
{
  SuspendedStoreObserveCtx ctx;
  LJGC2ActivationSnap activation;
  pthread_t observer;

  assert_suspended_state(f, co);
  assert(lj_gc2_activation_snapshot(&f->g->gc2.activation).gate ==
         LJ_GC2_ROOT_GATE_OPEN);
  (void)gate_move(f->g, LJ_GC2_ROOT_GATE_CLOSING);
  memset(&ctx, 0, sizeof(ctx));
  ctx.fixture = f;
  ctx.co = co;
  lj_gc2_test_table_store_gate_pause_arm();
  assert(pthread_create(&observer, NULL, suspended_store_observer, &ctx) == 0);
  if (hash_store)
    lua_rawset(co, 1);
  else
    lua_rawseti(co, 1, 1);
  assert(pthread_join(observer, NULL) == 0);

  assert(ctx.saw_owner && ctx.saw_hint && ctx.saw_main_cur_L &&
         ctx.saw_active_descriptor);
  assert_suspended_state(f, co);
  assert_descriptor_idle(f);
  activation = lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_IDLE &&
         activation.gate == LJ_GC2_ROOT_GATE_PENDING);
  gate_reopen(f->g);
}

static void test_suspended_coroutine_public_api_stores(void)
{
  Fixture f = fixture_open();
  lua_State *co = lua_newthread(f.L);
  GCtab *target;

  assert(co != NULL);
  assert_suspended_state(&f, co);
  lua_newtable(co);
  assert_suspended_state(&f, co);
  assert(lua_gettop(co) == 1);
  target = tabV(co->top - 1);

  /* A collectable array value enters the guarded scalar store while the
  ** suspended state is carried only by resumeclaim's temporary tg_hint. */
  lua_newtable(co);
  assert_suspended_state(&f, co);
  assert(lua_gettop(co) == 2);
  run_suspended_public_store(&f, co, 0);
  assert(lua_gettop(co) == 1);
  lua_rawgeti(co, 1, 1);
  assert(lua_istable(co, -1));
  lua_pop(co, 1);
  assert_suspended_state(&f, co);

  /* The hash path retains both a collectable string key and table value. */
  lua_pushliteral(co, "gc2-hash-value");
  lua_newtable(co);
  assert_suspended_state(&f, co);
  assert(lua_gettop(co) == 3);
  run_suspended_public_store(&f, co, 1);
  assert(lua_gettop(co) == 1);
  lua_pushliteral(co, "gc2-hash-value");
  lua_rawget(co, 1);
  assert(lua_istable(co, -1));
  lua_pop(co, 1);
  assert_suspended_state(&f, co);

  assert(lj_tab_gc2_rescan_state_acq(target) == LJ_TAB_RESCAN_COUNTED);
  assert(lj_gc2_test_table_rescan_clear(f.g, target) == 1);
  lua_settop(co, 0);
  assert_suspended_state(&f, co);
  fixture_close(&f);
}

static void test_second_universe_public_api_stores(void)
{
  Fixture f = fixture_open();
  lua_State *L2;
  global_State *g2;
  TGState *tg2;
  GCtab *target;

  assert(lj_thr_get_tg() == f.tg);
  L2 = luaL_newstate();
  assert(L2 != NULL);
  g2 = G(L2);
  tg2 = G2TG(g2);
  assert(tg2 != NULL && tg2 != f.tg && L2->tg_hint == tg2);
  assert(lj_thr_get_tg() == f.tg);

  /* Library initialization performs many GC-valued table publications. It
  ** must use universe 2's canonical main-TG carrier without replacing the
  ** first universe's raw TLS binding. */
  luaL_openlibs(L2);
  assert(lj_thr_get_tg() == f.tg);

  lua_newtable(L2);
  target = tabV(L2->top - 1);
  lua_newtable(L2);
  lua_rawseti(L2, 1, 1);
  lua_rawgeti(L2, 1, 1);
  assert(lua_istable(L2, -1));
  lua_pop(L2, 1);

  lua_pushliteral(L2, "second-universe-value");
  lua_newtable(L2);
  lua_rawset(L2, 1);
  lua_pushliteral(L2, "second-universe-value");
  lua_rawget(L2, 1);
  assert(lua_istable(L2, -1));
  lua_pop(L2, 1);

  assert(lj_thr_get_tg() == f.tg);
  assert(lj_gc2_rootdesc_snapshot(&tg2->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(lj_tab_gc2_rescan_state_acq(target) == LJ_TAB_RESCAN_COUNTED);
  assert(lj_gc2_test_table_rescan_clear(g2, target) == 1);
  lua_close(L2);
  assert(lj_thr_get_tg() == f.tg);
  fixture_close(&f);
}

static LJGC2RootDescSpec dummy_store_spec(const Fixture *f)
{
  LJGC2RootDescSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.flags = LJ_GC2_ROOTDESC_F_OLD | LJ_GC2_ROOTDESC_F_NEW |
               LJ_GC2_ROOTDESC_F_AUX | LJ_GC2_ROOTDESC_F_TABLE_STORE;
  spec.old_root = tv_rawload(&f->parent_tv);
  spec.new_root = tv_rawload(&f->key);
  spec.aux_root = tv_rawload(&f->value);
  return spec;
}

static void test_busy_descriptor_pins_global(void)
{
  Fixture f = fixture_open();
  LJGC2RootDescSpec spec = dummy_store_spec(&f);
  LJGC2RootDescTicket ticket;
  LJGC2TableStoreGuard guard;
  LJGC2ActivationSnap activation;
  ResourceSnap before;

  assert(lj_gc2_rootdesc_publish(&f.tg->root_desc, &spec, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  before = resource_snapshot(&f);
  assert(lj_gc2_table_store_begin(f.L, &guard, f.parent,
                                   &f.key, &f.value) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  /* A begin-stage PINNED result alone never authorizes the semantic CAS. The
  ** conservative authorities remain live so final revalidation can do so. */
  assert(!guard.finished && !guard.active && guard.globally_pinned);
  assert(!guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  assert_guard_resources_live(&guard, 0, 1, 0);
  assert(lj_gc2_table_store_admit(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert(guard.gate_admitted && !guard.gate_revalidated &&
         !guard.store_authorized);
  assert(lj_gc2_table_store_revalidate(f.L, &guard) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert(guard.gate_admitted && guard.gate_revalidated &&
         guard.store_authorized);
  activation = lj_gc2_activation_snapshot(&f.g->gc2.activation);
  assert(activation.state == LJ_GC2_ACT_NO_RECLAIM);
  assert(activation.gate == LJ_GC2_ROOT_GATE_OPEN);
  /* Nested fallback must not poison or replace the outer owner's ticket. */
  assert(la_load64_acq(&f.tg->root_desc.control) == ticket.control);
  assert(lj_gc2_rootdesc_snapshot(&f.tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(lj_gc2_table_store_finish(f.L, &guard, 0) ==
         LJ_GC2_TABLE_STORE_GUARD_PINNED);
  assert_guard_resources_released(&guard);
  assert(la_load64_acq(&f.tg->root_desc.control) == ticket.control);
  assert_resources_unchanged(&f, &before);

  assert(lj_gc2_rootdesc_finish(&f.tg->root_desc, &ticket) ==
         LJ_GC2_ROOTDESC_OK);
  /* Single-threaded repair of absorbing global test authority. */
  repair_activation_idle(&f);
  fixture_close(&f);
}

int main(void)
{
  test_by_value_capture_and_open_admission();
  run_admission_at_gate(LJ_GC2_ROOT_GATE_OPEN);
  run_admission_at_gate(LJ_GC2_ROOT_GATE_PENDING);
  run_admission_at_gate(LJ_GC2_ROOT_GATE_CLOSING);
  run_admission_at_gate(LJ_GC2_ROOT_GATE_COMMIT);
  run_revalidation_at_gate(LJ_GC2_ROOT_GATE_CLOSING);
  run_revalidation_at_gate(LJ_GC2_ROOT_GATE_COMMIT);
  run_lost_gate_retry(0);
  run_lost_gate_retry(1);
  test_ticket_pin_before_revalidate();
  test_wrong_owner_revalidate_cleanup();
  test_stage_order_rejection();
  test_malformed_parent_and_tvalue_rejected();
  test_finish_ordering();
  test_sweep_finish_dirty_ordering();
  test_legacy_carrier_without_registry_borrow();
  test_suspended_coroutine_public_api_stores();
  test_second_universe_public_api_stores();
  test_busy_descriptor_pins_global();
  puts("t-gc2-table-store-guard OK: root-gate store guard substrate");
  return 0;
}
