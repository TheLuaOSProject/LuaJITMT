/*
** Deterministic races for the owner-only monotonic shared new-key protocol.
**
** A collision claimant publishes its real key before it links the node. This
** makes the claimed node usable as a physical hash anchor, even while it is
** not yet reachable from the claimant's own bucket. The protocol must:
**
**  * let a same-key inserter finish without waiting for the paused claimant,
**    then make the claimant converge on that inserter's live slot; and
**  * preserve a collision chain concurrently appended through the claimed
**    node while the claimant publishes into its own target chain.
**
** The FINREG collision helper uses the same exact-tail CAS. A third schedule
** pauses an ordinary publisher after its tail snapshot, lets FINREG win that
** tail, and proves the ordinary publisher converges instead of linking a
** duplicate live slot.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_thr.h"

#include "lib/test_sleep.h"
#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-newkey-monotonic requires LJ_TAB_TEST_HELPERS"
#endif

#define HASH_SLOTS	64u
#define WAIT_STEPS	50000u
#define WAIT_STEP_NS	100000L

#define STAGE_BIT(stage)	(1u << (stage))
#define COLLISION_REQUIRED_STAGES \
  (STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_KEY) | \
   STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_LINK))
#define COLLISION_ALLOWED_STAGES \
  (COLLISION_REQUIRED_STAGES | \
   STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT))

typedef struct InsertCtx {
  lua_State *L;
  GCtab *table;
  GCstr *key;
  int32_t value;
  volatile uint32_t ready;
  volatile uint32_t start;
  volatile uint32_t done;
  TValue *slot;
  int status;
} InsertCtx;

typedef struct PublishRace {
  GCtab *table;
  Node *nodebase;
  MSize hmask;
  InsertCtx *worker[2];
  uint32_t pause_stage;
  uint32_t ignore_after_release;
  volatile uint32_t pause_reached;
  volatile uint32_t pause_release;
  volatile uint32_t stage_mask[2];
  volatile uint32_t stage_count[2];
  volatile uint32_t last_stage[2];
  Node *anchor[2];
  Node *claimed[2];
} PublishRace;

typedef struct KeyStats {
  MSize physical;
  MSize live;
  MSize nil;
  Node *live_node;
} KeyStats;

static PublishRace *active_race;

static uint32_t load_flag(volatile uint32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_flag(volatile uint32_t *p, uint32_t value)
{
  __atomic_store_n(p, value, __ATOMIC_RELEASE);
}

static int wait_for_flag(volatile uint32_t *p, uint32_t value)
{
  uint32_t i;
  for (i = 0; i < WAIT_STEPS; i++) {
    if (load_flag(p) == value)
      return 1;
    sleep_ns(WAIT_STEP_NS);
  }
  return load_flag(p) == value;
}

static int node_in_vector(Node *nodebase, MSize hmask, Node *node)
{
  uintptr_t base = (uintptr_t)(void *)nodebase;
  uintptr_t addr = (uintptr_t)(void *)node;
  uintptr_t span = (uintptr_t)hmask * sizeof(*nodebase);
  uintptr_t offset;
  if (addr < base)
    return 0;
  offset = addr - base;
  return offset <= span && offset % sizeof(*nodebase) == 0;
}

static int node_in_chain(Node *anchor, Node *want, MSize hmask)
{
  Node *n;
  MSize steps = 0;
  for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
    assert(steps++ <= hmask);
    if (n == want)
      return 1;
  }
  return 0;
}

static void assert_completed_collision_stages(PublishRace *race, int id)
{
  uint32_t mask = load_flag(&race->stage_mask[id]);
  uint32_t count = load_flag(&race->stage_count[id]);
  assert((mask & COLLISION_REQUIRED_STAGES) ==
	 COLLISION_REQUIRED_STAGES);
  assert((mask & ~COLLISION_ALLOWED_STAGES) == 0);
  assert(count == ((mask & STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT)) ?
		   3u : 2u));
}

static void assert_unlinked_duplicate_stages(PublishRace *race, int id)
{
  uint32_t mask = load_flag(&race->stage_mask[id]);
  uint32_t count = load_flag(&race->stage_count[id]);
  assert((mask & STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_KEY)) != 0);
  assert((mask & STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_LINK)) == 0);
  assert((mask & ~COLLISION_ALLOWED_STAGES) == 0);
  assert(count == ((mask & STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT)) ?
		   2u : 1u));
}

static void assert_real_worker_key(InsertCtx *worker, Node *claimed)
{
  TValue key;
  lj_tv_load_acq(&key, &claimed->key);
  assert(!tviskeylock(&key));
  assert(tvisstr(&key));
  assert(strV(&key) == worker->key);
}

static void newkey_publish_hook(lua_State *L, GCtab *t, Node *nodebase,
				Node *anchor, Node *claimed, uint32_t stage)
{
  PublishRace *race = active_race;
  InsertCtx *worker;
  uint32_t bit, oldmask, previous;
  int id;

  assert(race != NULL);
  if (race->ignore_after_release && load_flag(&race->pause_release))
    return;
  if (L == race->worker[0]->L)
    id = 0;
  else {
    assert(L == race->worker[1]->L);
    id = 1;
  }
  worker = race->worker[id];
  assert(t == race->table);
  assert(nodebase == race->nodebase);
  assert(lj_tab_node_acq(t) == nodebase);
  assert(lj_tab_node_hmask_acq(nodebase) == race->hmask);
  assert(!lj_tab_node_is_retiring(nodebase));
  assert(stage >= LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY);
  assert(stage <= LJ_TAB_NEWKEY_HOOK_COLLISION_LINK);
  assert(anchor == hashstr_node(nodebase, race->hmask, worker->key));
  assert(claimed != NULL);
  if (stage == LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY)
    assert(claimed == anchor);
  else
    assert(claimed != anchor);
  assert(node_in_vector(nodebase, race->hmask, claimed));
  assert_real_worker_key(worker, claimed);

  bit = STAGE_BIT(stage);
  oldmask = __atomic_fetch_or(&race->stage_mask[id], bit, __ATOMIC_ACQ_REL);
  assert((oldmask & bit) == 0 ||
	 stage == LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT);
  previous = __atomic_exchange_n(&race->last_stage[id], stage,
				 __ATOMIC_ACQ_REL);
  if (stage == LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT)
    assert(previous == LJ_TAB_NEWKEY_HOOK_COLLISION_KEY ||
	   previous == LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT);
  else
    assert(previous == 0 || previous < stage);
  (void)__atomic_add_fetch(&race->stage_count[id], 1, __ATOMIC_ACQ_REL);

  if (stage == LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY ||
      stage == LJ_TAB_NEWKEY_HOOK_COLLISION_KEY) {
    assert(race->anchor[id] == NULL && race->claimed[id] == NULL);
    race->anchor[id] = anchor;
    race->claimed[id] = claimed;
  } else {
    assert(race->anchor[id] == anchor);
    assert(race->claimed[id] == claimed);
  }

  if (stage == LJ_TAB_NEWKEY_HOOK_COLLISION_LINK)
    assert(node_in_chain(anchor, claimed, race->hmask));

  if (id == 0 && stage == race->pause_stage) {
    store_flag(&race->pause_reached, 1);
    while (!load_flag(&race->pause_release))
      sched_yield();
  }
}

static void *insert_worker(void *arg)
{
  InsertCtx *ctx = (InsertCtx *)arg;
  TValue value;

  if (!lj_threading_attach(ctx->L)) {
    ctx->status = 1;
    store_flag(&ctx->done, 1);
    return NULL;
  }
  if (!lua_istable(ctx->L, 1)) {
    ctx->status = 2;
    lj_threading_detach(ctx->L, 1);
    store_flag(&ctx->done, 1);
    return NULL;
  }

  store_flag(&ctx->ready, 1);
  while (!load_flag(&ctx->start))
    sched_yield();

  assert(ctx->key != NULL);
  ctx->slot = lj_tab_setstr(ctx->L, ctx->table, ctx->key);
  assert(ctx->slot != NULL);
  setintV(&value, ctx->value);
  ctx->slot = lj_tab_storetv_forvm_strhash(ctx->L, ctx->table, ctx->slot,
					   &value, ctx->key);
  assert(ctx->slot != NULL);
  ctx->status = 0;
  store_flag(&ctx->done, 1);
  lj_threading_detach(ctx->L, 1);
  return NULL;
}

static lua_State *new_child_with_table(lua_State *L)
{
  lua_State *child = lua_newthread(L);
  assert(child != NULL);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  assert(lua_gettop(child) == 1 && lua_istable(child, 1));
  return child;
}

static void root_str(lua_State *L, GCstr *s)
{
  setstrV(L, L->top, s);
  lj_state_stack_pubtv(L, L, L->top);
  incr_top(L);
}

static GCstr *new_bucket_key(lua_State *L, const char *prefix, MSize hmask,
			     MSize bucket, uint32_t *seq)
{
  GCstr *s = tabfwd_find_sid_bucket(L, prefix, (uint32_t)hmask,
				    (uint32_t)bucket, seq);
  root_str(L, s);
  return s;
}

static void preintern_bucket_keys(lua_State *L, const char *prefix,
				  MSize hmask, GCstr **keys, uint32_t *seq)
{
  MSize i;
  for (i = 0; i <= hmask; i++)
    keys[i] = new_bucket_key(L, prefix, hmask, i, seq);
}

static void init_worker(InsertCtx *ctx, lua_State *L, GCtab *t, GCstr *key,
			int32_t value)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->L = L;
  ctx->table = t;
  ctx->key = key;
  ctx->value = value;
  ctx->status = -1;
}

static void init_race(PublishRace *race, GCtab *t, Node *nodebase,
		      MSize hmask, InsertCtx *first, InsertCtx *second,
		      uint32_t pause_stage)
{
  memset(race, 0, sizeof(*race));
  race->table = t;
  race->nodebase = nodebase;
  race->hmask = hmask;
  race->worker[0] = first;
  race->worker[1] = second;
  race->pause_stage = pause_stage;
  active_race = race;
  lj_tab_test_set_newkey_publish_hook(newkey_publish_hook);
}

static void finish_race(PublishRace *race)
{
  lj_tab_test_set_newkey_publish_hook(NULL);
  assert(active_race == race);
  active_race = NULL;
}

static KeyStats scan_string_key(Node *nodebase, MSize hmask, GCstr *key)
{
  KeyStats stats;
  MSize i;
  memset(&stats, 0, sizeof(stats));
  for (i = 0; i <= hmask; i++) {
    TValue nk, nv;
    lj_tv_load_acq(&nk, &nodebase[i].key);
    assert(!tviskeylock(&nk));
    lj_tv_load_acq(&nv, &nodebase[i].val);
    assert(!tvisforward(&nv));
    if (tvisstr(&nk) && strV(&nk) == key) {
      stats.physical++;
      if (tvisnil(&nv)) {
	stats.nil++;
      } else {
	stats.live++;
	stats.live_node = &nodebase[i];
      }
    }
  }
  return stats;
}

static void assert_quiescent_string_graph(GCtab *t)
{
  MSize hmask, i;
  Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  MSize live = 0;

  for (i = 0; i <= hmask; i++) {
    Node *n;
    MSize steps = 0;
    TValue key, val;
    for (n = &nodebase[i]; n != NULL; n = lj_tab_nextnode_acq(n)) {
      assert(node_in_vector(nodebase, hmask, n));
      assert(steps++ <= hmask);
    }

    lj_tv_load_acq(&key, &nodebase[i].key);
    lj_tv_load_acq(&val, &nodebase[i].val);
    assert(!tviskeylock(&key));
    assert(!tvisforward(&val));
    if (!tvisnil(&val)) {
      Node *anchor, *p;
      int found = 0;
      assert(tvisstr(&key));
      anchor = hashstr_node(nodebase, hmask, strV(&key));
      steps = 0;
      for (p = anchor; p != NULL; p = lj_tab_nextnode_acq(p)) {
	assert(node_in_vector(nodebase, hmask, p));
	assert(steps++ <= hmask);
	if (p == &nodebase[i])
	  found = 1;
      }
      assert(found);
      live++;
    }
  }
  assert((MSize)tabfwd_count_next_visible(t) == live);
}

static int string_key_absent(GCtab *t, GCstr *key)
{
  cTValue *tv = lj_tab_getstr(t, key);
  return tv == NULL || tvisnil(tv);
}

static void assert_generation_unchanged(GCtab *t, Node *nodebase, MSize hmask)
{
  MSize current_hmask;
  assert(lj_tab_node_snapshot_acq(t, &current_hmask) == nodebase);
  assert(current_hmask == hmask);
  assert(lj_tab_node_nextgen_acq(nodebase) == NULL);
  assert(!lj_tab_node_is_retiring(nodebase));
}

static void exercise_private_to_shared_tail(lua_State *L)
{
  const MSize target_bucket = 3u;
  GCstr *key[6];
  GCtab *t;
  Node *nodebase, *oldchain[4], *n;
  MSize hmask, freecount0, freecount1, i;
  KeyStats first_shared, second_shared;
  InsertCtx first, second;
  pthread_t first_thread, second_thread;
  uint32_t seq = 0;

  lua_settop(L, 0);
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask == HASH_SLOTS-1u);

  for (i = 0; i < 6u; i++)
    key[i] = new_bucket_key(L, "newkey-monotonic-private-shared", hmask,
			    target_bucket, &seq);
  for (i = 0; i < 4u; i++)
    tabfwd_set_str_i32(L, t, key[i], (int32_t)(80u+i));

  n = hashstr_node(nodebase, hmask, key[0]);
  for (i = 0; i < 4u; i++) {
    assert(n != NULL);
    oldchain[i] = n;
    n = lj_tab_nextnode_acq(n);
  }
  assert(n == NULL);
  freecount0 = lj_tab_node_freecount_acq(nodebase);

  init_worker(&first, new_child_with_table(L), t, key[4], 84);
  assert(pthread_create(&first_thread, NULL, insert_worker, &first) == 0);
  assert(wait_for_flag(&first.ready, 1));
  store_flag(&first.start, 1);
  assert(pthread_join(first_thread, NULL) == 0);
  assert(first.status == 0);

  init_worker(&second, new_child_with_table(L), t, key[5], 85);
  assert(pthread_create(&second_thread, NULL, insert_worker, &second) == 0);
  assert(wait_for_flag(&second.ready, 1));
  store_flag(&second.start, 1);
  assert(pthread_join(second_thread, NULL) == 0);
  assert(second.status == 0);

  assert_generation_unchanged(t, nodebase, hmask);
  first_shared = scan_string_key(nodebase, hmask, key[4]);
  second_shared = scan_string_key(nodebase, hmask, key[5]);
  assert(first_shared.physical == 1u && first_shared.live == 1u);
  assert(second_shared.physical == 1u && second_shared.live == 1u);
  for (i = 0; i < 3u; i++)
    assert(lj_tab_nextnode_acq(oldchain[i]) == oldchain[i+1u]);
  assert(lj_tab_nextnode_acq(oldchain[3]) == first_shared.live_node);
  assert(lj_tab_nextnode_acq(first_shared.live_node) ==
	 second_shared.live_node);
  assert(lj_tab_nextnode_acq(second_shared.live_node) == NULL);
  freecount1 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 - freecount1 == 2u);
  for (i = 0; i < 6u; i++)
    tabfwd_assert_str_i32(t, key[i], (int32_t)(80u+i));
  assert(tabfwd_count_next_visible(t) == 6);
  assert_quiescent_string_graph(t);
}

static void exercise_anchor_same_key_resize(lua_State *L)
{
  const MSize target_bucket = 5u;
  GCtab *t;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask, freecount0;
  GCstr *shared_key;
  TValue oldval;
  KeyStats stats;
  InsertCtx a, b;
  PublishRace race;
  pthread_t a_thread, b_thread;
  uint32_t seq = 0;

  lua_settop(L, 0);
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_snapshot_acq(t, &oldhmask);
  assert(oldhmask == HASH_SLOTS-1u);
  shared_key = new_bucket_key(L, "newkey-monotonic-anchor", oldhmask,
			      target_bucket, &seq);
  freecount0 = lj_tab_node_freecount_acq(oldnode);

  init_worker(&a, new_child_with_table(L), t, shared_key, 111);
  init_worker(&b, new_child_with_table(L), t, shared_key, 222);
  init_race(&race, t, oldnode, oldhmask, &a, &b,
	    LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY);
  race.ignore_after_release = 1;
  assert(pthread_create(&a_thread, NULL, insert_worker, &a) == 0);
  assert(pthread_create(&b_thread, NULL, insert_worker, &b) == 0);
  assert(wait_for_flag(&a.ready, 1));
  assert(wait_for_flag(&b.ready, 1));

  store_flag(&a.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  assert(load_flag(&race.stage_mask[0]) ==
	 STAGE_BIT(LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY));
  assert(race.anchor[0] == race.claimed[0]);
  assert(lj_tv_isnil_acq(&race.claimed[0]->val));

  store_flag(&b.start, 1);
  assert(wait_for_flag(&b.done, 1));
  assert(pthread_join(b_thread, NULL) == 0);
  assert(b.status == 0 && !load_flag(&a.done));
  assert(load_flag(&race.stage_mask[1]) == 0);
  assert(b.slot == &race.claimed[0]->val);
  tabfwd_assert_str_i32(t, shared_key, 222);

  lj_tab_resize(L, t, t->asize, lj_fls((uint32_t)oldhmask) + 2u);
  newnode = lj_tab_node_snapshot_acq(t, &newhmask);
  assert(newnode != oldnode && newhmask > oldhmask);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_node_is_retiring(oldnode));
  assert(freecount0 - lj_tab_node_freecount_acq(oldnode) == 1u);
  lj_tv_load_acq(&oldval, &race.claimed[0]->val);
  assert(tvisforward(&oldval));
  tabfwd_assert_str_i32(t, shared_key, 222);
  assert(tabfwd_count_next_visible(t) == 1);

  store_flag(&race.pause_release, 1);
  assert(pthread_join(a_thread, NULL) == 0);
  finish_race(&race);
  assert(a.status == 0);
  assert(load_flag(&race.stage_mask[0]) ==
	 STAGE_BIT(LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY));
  assert(load_flag(&race.stage_count[0]) == 1u);
  assert_generation_unchanged(t, newnode, newhmask);
  stats = scan_string_key(newnode, newhmask, shared_key);
  assert(stats.physical == 1u && stats.live == 1u && stats.nil == 0u);
  assert(a.slot == &stats.live_node->val);
  tabfwd_assert_str_i32(t, shared_key, 111);
  assert(tabfwd_count_next_visible(t) == 1);
  assert_quiescent_string_graph(t);
}

static void exercise_same_key_convergence(lua_State *L)
{
  const MSize target_bucket = 7u;
  GCtab *t;
  Node *nodebase;
  MSize hmask, freecount0, freecount1;
  GCstr *anchor_key, *shared_key;
  KeyStats stats;
  InsertCtx a, b;
  PublishRace race;
  pthread_t a_thread, b_thread;
  uint32_t seq = 0;
  int b_finished_while_a_paused;

  lua_settop(L, 0);
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask == HASH_SLOTS-1u);

  anchor_key = new_bucket_key(L, "newkey-monotonic-same-anchor", hmask,
			      target_bucket, &seq);
  shared_key = new_bucket_key(L, "newkey-monotonic-same-key", hmask,
			      target_bucket, &seq);
  assert(anchor_key != shared_key);
  tabfwd_set_str_i32(L, t, anchor_key, 17);
  assert_generation_unchanged(t, nodebase, hmask);
  freecount0 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 > 2u);

  init_worker(&a, new_child_with_table(L), t, shared_key, 101);
  init_worker(&b, new_child_with_table(L), t, shared_key, 202);
  init_race(&race, t, nodebase, hmask, &a, &b,
	    LJ_TAB_NEWKEY_HOOK_COLLISION_KEY);
  assert(pthread_create(&a_thread, NULL, insert_worker, &a) == 0);
  assert(pthread_create(&b_thread, NULL, insert_worker, &b) == 0);
  assert(wait_for_flag(&a.ready, 1));
  assert(wait_for_flag(&b.ready, 1));

  store_flag(&a.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  assert(load_flag(&race.stage_mask[0]) ==
	 STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_KEY));
  assert(race.anchor[0] == hashstr_node(nodebase, hmask, shared_key));
  assert(lj_tab_nextnode_acq(race.claimed[0]) == NULL);

  store_flag(&b.start, 1);
  b_finished_while_a_paused = wait_for_flag(&b.done, 1);
  if (b_finished_while_a_paused) {
    assert(b.status == 0);
    assert(!load_flag(&a.done));
    assert_completed_collision_stages(&race, 1);
    assert(race.claimed[1] != race.claimed[0]);
    assert(node_in_chain(race.anchor[1], race.claimed[1], hmask));
    tabfwd_assert_str_i32(t, shared_key, 202);
  }

  store_flag(&race.pause_release, 1);
  assert(pthread_join(a_thread, NULL) == 0);
  assert(pthread_join(b_thread, NULL) == 0);
  finish_race(&race);
  assert(b_finished_while_a_paused);
  assert(a.status == 0 && b.status == 0);
  assert_unlinked_duplicate_stages(&race, 0);
  assert_completed_collision_stages(&race, 1);

  assert_generation_unchanged(t, nodebase, hmask);
  assert(race.anchor[0] == race.anchor[1]);
  assert(race.claimed[0] != race.claimed[1]);
  assert(!node_in_chain(race.anchor[0], race.claimed[0], hmask));
  assert(node_in_chain(race.anchor[0], race.claimed[1], hmask));
  assert(a.slot == &race.claimed[1]->val);
  assert(b.slot == &race.claimed[1]->val);
  assert(lj_tv_isnil_acq(&race.claimed[0]->val));

  stats = scan_string_key(nodebase, hmask, shared_key);
  assert(stats.physical == 2u);
  assert(stats.live == 1u && stats.nil == 1u);
  assert(stats.live_node == race.claimed[1]);
  freecount1 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 - freecount1 == stats.physical);
  tabfwd_assert_str_i32(t, anchor_key, 17);
  tabfwd_assert_str_i32(t, shared_key, 101);
  assert(tabfwd_count_next_visible(t) == 2);
  assert_quiescent_string_graph(t);
}

static void exercise_tail_cas_loss_reuses_claim(lua_State *L)
{
  const MSize target_bucket = 13u;
  GCtab *t;
  Node *nodebase;
  MSize hmask, freecount0, freecount1;
  GCstr *anchor_key, *a_key, *b_key;
  KeyStats a_stats, b_stats;
  InsertCtx a, b;
  PublishRace race;
  pthread_t a_thread, b_thread;
  uint32_t seq = 0;

  lua_settop(L, 0);
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask == HASH_SLOTS-1u);

  anchor_key = new_bucket_key(L, "newkey-monotonic-tail-anchor", hmask,
			      target_bucket, &seq);
  a_key = new_bucket_key(L, "newkey-monotonic-tail-a", hmask,
			 target_bucket, &seq);
  b_key = new_bucket_key(L, "newkey-monotonic-tail-b", hmask,
			 target_bucket, &seq);
  assert(anchor_key != a_key && anchor_key != b_key && a_key != b_key);
  tabfwd_set_str_i32(L, t, anchor_key, 29);
  freecount0 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 > 2u);

  init_worker(&a, new_child_with_table(L), t, a_key, 303);
  init_worker(&b, new_child_with_table(L), t, b_key, 404);
  init_race(&race, t, nodebase, hmask, &a, &b,
	    LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT);
  assert(pthread_create(&a_thread, NULL, insert_worker, &a) == 0);
  assert(pthread_create(&b_thread, NULL, insert_worker, &b) == 0);
  assert(wait_for_flag(&a.ready, 1));
  assert(wait_for_flag(&b.ready, 1));

  store_flag(&a.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  assert(load_flag(&race.stage_count[0]) == 2u);
  assert(!node_in_chain(race.anchor[0], race.claimed[0], hmask));

  store_flag(&b.start, 1);
  assert(wait_for_flag(&b.done, 1));
  assert(b.status == 0 && !load_flag(&a.done));
  assert_completed_collision_stages(&race, 1);
  assert(race.anchor[0] == race.anchor[1]);
  assert(race.claimed[0] != race.claimed[1]);
  assert(node_in_chain(race.anchor[0], race.claimed[1], hmask));
  assert(!node_in_chain(race.anchor[0], race.claimed[0], hmask));

  store_flag(&race.pause_release, 1);
  assert(pthread_join(a_thread, NULL) == 0);
  assert(pthread_join(b_thread, NULL) == 0);
  finish_race(&race);
  assert(a.status == 0 && b.status == 0);
  assert(load_flag(&race.stage_mask[0]) == COLLISION_ALLOWED_STAGES);
  assert(load_flag(&race.stage_count[0]) == 4u);
  assert_completed_collision_stages(&race, 1);

  assert_generation_unchanged(t, nodebase, hmask);
  assert(node_in_chain(race.anchor[0], race.claimed[0], hmask));
  assert(node_in_chain(race.anchor[0], race.claimed[1], hmask));
  assert(lj_tab_nextnode_acq(race.anchor[0]) == race.claimed[1]);
  assert(lj_tab_nextnode_acq(race.claimed[1]) == race.claimed[0]);
  assert(lj_tab_nextnode_acq(race.claimed[0]) == NULL);
  assert(a.slot == &race.claimed[0]->val);
  assert(b.slot == &race.claimed[1]->val);
  a_stats = scan_string_key(nodebase, hmask, a_key);
  b_stats = scan_string_key(nodebase, hmask, b_key);
  assert(a_stats.physical == 1u && a_stats.live == 1u &&
	 a_stats.nil == 0u && a_stats.live_node == race.claimed[0]);
  assert(b_stats.physical == 1u && b_stats.live == 1u &&
	 b_stats.nil == 0u && b_stats.live_node == race.claimed[1]);
  freecount1 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 - freecount1 == 2u);
  tabfwd_assert_str_i32(t, anchor_key, 29);
  tabfwd_assert_str_i32(t, a_key, 303);
  tabfwd_assert_str_i32(t, b_key, 404);
  assert(tabfwd_count_next_visible(t) == 3);
  assert_quiescent_string_graph(t);
}

static void exercise_finreg_tail_arbitration(lua_State *L)
{
  const MSize target_bucket = 19u;
  GCtab *t;
  Node *nodebase;
  MSize hmask, freecount0, freecount1;
  GCstr *anchor_key, *shared_key;
  TValue key, claim, *fin_slot = NULL;
  KeyStats stats;
  InsertCtx ordinary, unused;
  PublishRace race;
  pthread_t ordinary_thread;
  uint32_t seq = 0;
  int rc;

  lua_settop(L, 0);
  assert(lua_checkstack(L, 32));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask == HASH_SLOTS-1u);

  anchor_key = new_bucket_key(L, "newkey-monotonic-finreg-anchor", hmask,
			      target_bucket, &seq);
  shared_key = new_bucket_key(L, "newkey-monotonic-finreg-key", hmask,
			      target_bucket, &seq);
  assert(anchor_key != shared_key);
  tabfwd_set_str_i32(L, t, anchor_key, 43);
  freecount0 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 > 2u);

  init_worker(&ordinary, new_child_with_table(L), t, shared_key, 505);
  init_worker(&unused, L, t, shared_key, 0);
  init_race(&race, t, nodebase, hmask, &ordinary, &unused,
	    LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT);
  assert(pthread_create(&ordinary_thread, NULL, insert_worker, &ordinary) == 0);
  assert(wait_for_flag(&ordinary.ready, 1));

  store_flag(&ordinary.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  assert(load_flag(&race.stage_mask[0]) ==
	 (STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_KEY) |
	  STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT)));
  assert(!node_in_chain(race.anchor[0], race.claimed[0], hmask));

  setstrV(L, &key, shared_key);
  setintV(&claim, 0x533);
  rc = lj_tab_try_newkey_chain(L, t, &key, &claim, &fin_slot);
  assert(rc == 1);
  assert(fin_slot != NULL && tabfwd_tv_i32(fin_slot) == 0x533);
  assert((TValue *)lj_tab_getstr(t, shared_key) == fin_slot);
  assert(!node_in_chain(race.anchor[0], race.claimed[0], hmask));

  store_flag(&race.pause_release, 1);
  assert(pthread_join(ordinary_thread, NULL) == 0);
  finish_race(&race);
  assert(ordinary.status == 0);
  assert_unlinked_duplicate_stages(&race, 0);
  assert(ordinary.slot == fin_slot);

  assert_generation_unchanged(t, nodebase, hmask);
  assert(lj_tv_isnil_acq(&race.claimed[0]->val));
  stats = scan_string_key(nodebase, hmask, shared_key);
  assert(stats.physical == 2u);
  assert(stats.live == 1u && stats.nil == 1u);
  assert(&stats.live_node->val == fin_slot);
  freecount1 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 - freecount1 == stats.physical);
  tabfwd_assert_str_i32(t, anchor_key, 43);
  tabfwd_assert_str_i32(t, shared_key, 505);
  assert(tabfwd_count_next_visible(t) == 2);
  assert_quiescent_string_graph(t);
}

static void exercise_claimed_node_anchor(lua_State *L)
{
  const MSize target_bucket = 11u;
  GCstr *bucket_key[HASH_SLOTS];
  GCtab *t;
  Node *nodebase;
  MSize hmask, claimed_index, freecount0, freecount1;
  GCstr *anchor_key, *a_key, *c_key;
  KeyStats a_stats, c_stats;
  InsertCtx a, c;
  PublishRace race;
  pthread_t a_thread, c_thread;
  uint32_t seq = 0;
  int c_finished_while_a_paused;

  lua_settop(L, 0);
  assert(lua_checkstack(L, HASH_SLOTS + 32u));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask == HASH_SLOTS-1u);

  preintern_bucket_keys(L, "newkey-monotonic-bucket", hmask, bucket_key,
			&seq);
  anchor_key = bucket_key[target_bucket];
  a_key = new_bucket_key(L, "newkey-monotonic-claimed-a", hmask,
			 target_bucket, &seq);
  assert(anchor_key != a_key);
  tabfwd_set_str_i32(L, t, anchor_key, 31);
  assert_generation_unchanged(t, nodebase, hmask);
  freecount0 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 > 2u);

  init_worker(&a, new_child_with_table(L), t, a_key, 303);
  init_worker(&c, new_child_with_table(L), t, NULL, 404);
  init_race(&race, t, nodebase, hmask, &a, &c,
	    LJ_TAB_NEWKEY_HOOK_COLLISION_KEY);
  assert(pthread_create(&a_thread, NULL, insert_worker, &a) == 0);
  assert(pthread_create(&c_thread, NULL, insert_worker, &c) == 0);
  assert(wait_for_flag(&a.ready, 1));
  assert(wait_for_flag(&c.ready, 1));

  store_flag(&a.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  assert(load_flag(&race.stage_mask[0]) ==
	 STAGE_BIT(LJ_TAB_NEWKEY_HOOK_COLLISION_KEY));
  assert(lj_tab_nextnode_acq(race.claimed[0]) == NULL);
  claimed_index = (MSize)(race.claimed[0] - nodebase);
  assert(claimed_index <= hmask && claimed_index != target_bucket);
  c_key = bucket_key[claimed_index];
  assert(c_key != anchor_key && c_key != a_key);
  assert(hashstr_node(nodebase, hmask, c_key) == race.claimed[0]);
  c.key = c_key;  /* Published to C by the following release-store to start. */

  store_flag(&c.start, 1);
  c_finished_while_a_paused = wait_for_flag(&c.done, 1);
  if (c_finished_while_a_paused) {
    assert(c.status == 0);
    assert(!load_flag(&a.done));
    assert_completed_collision_stages(&race, 1);
    assert(race.anchor[1] == race.claimed[0]);
    assert(race.claimed[1] != race.claimed[0]);
    assert(lj_tab_nextnode_acq(race.claimed[0]) == race.claimed[1]);
    assert(string_key_absent(t, a_key));
    tabfwd_assert_str_i32(t, c_key, 404);
  }

  store_flag(&race.pause_release, 1);
  assert(pthread_join(a_thread, NULL) == 0);
  assert(pthread_join(c_thread, NULL) == 0);
  finish_race(&race);
  assert(c_finished_while_a_paused);
  assert(a.status == 0 && c.status == 0);
  assert_completed_collision_stages(&race, 0);
  assert_completed_collision_stages(&race, 1);

  assert_generation_unchanged(t, nodebase, hmask);
  assert(race.anchor[0] ==
	 hashstr_node(nodebase, hmask, a_key));
  assert(race.anchor[1] == race.claimed[0]);
  assert(lj_tab_nextnode_acq(race.claimed[0]) == race.claimed[1]);
  assert(node_in_chain(race.anchor[0], race.claimed[0], hmask));
  assert(node_in_chain(race.claimed[0], race.claimed[1], hmask));
  assert(a.slot == &race.claimed[0]->val);
  assert(c.slot == &race.claimed[1]->val);

  a_stats = scan_string_key(nodebase, hmask, a_key);
  c_stats = scan_string_key(nodebase, hmask, c_key);
  assert(a_stats.physical == 1u && a_stats.live == 1u &&
	 a_stats.nil == 0u && a_stats.live_node == race.claimed[0]);
  assert(c_stats.physical == 1u && c_stats.live == 1u &&
	 c_stats.nil == 0u && c_stats.live_node == race.claimed[1]);
  freecount1 = lj_tab_node_freecount_acq(nodebase);
  assert(freecount0 - freecount1 ==
	 a_stats.physical + c_stats.physical);
  tabfwd_assert_str_i32(t, anchor_key, 31);
  tabfwd_assert_str_i32(t, a_key, 303);
  tabfwd_assert_str_i32(t, c_key, 404);
  assert(tabfwd_count_next_visible(t) == 3);
  assert_quiescent_string_graph(t);
}

static void exercise_claimed_node_resize(lua_State *L)
{
  const MSize target_bucket = 27u;
  GCstr *bucket_key[HASH_SLOTS];
  GCtab *t;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask, claimed_index;
  GCstr *anchor_key, *a_key, *c_key;
  KeyStats a_stats, c_stats;
  InsertCtx a, c;
  PublishRace race;
  pthread_t a_thread, c_thread;
  uint32_t seq = 0;

  lua_settop(L, 0);
  assert(lua_checkstack(L, HASH_SLOTS + 32u));
  lua_createtable(L, 0, HASH_SLOTS);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_snapshot_acq(t, &oldhmask);
  assert(oldhmask == HASH_SLOTS-1u);

  preintern_bucket_keys(L, "newkey-monotonic-resize-bucket", oldhmask,
			bucket_key, &seq);
  anchor_key = bucket_key[target_bucket];
  a_key = new_bucket_key(L, "newkey-monotonic-resize-a", oldhmask,
			 target_bucket, &seq);
  assert(anchor_key != a_key);
  tabfwd_set_str_i32(L, t, anchor_key, 61);

  init_worker(&a, new_child_with_table(L), t, a_key, 606);
  init_worker(&c, new_child_with_table(L), t, NULL, 707);
  init_race(&race, t, oldnode, oldhmask, &a, &c,
	    LJ_TAB_NEWKEY_HOOK_COLLISION_KEY);
  race.ignore_after_release = 1;
  assert(pthread_create(&a_thread, NULL, insert_worker, &a) == 0);
  assert(pthread_create(&c_thread, NULL, insert_worker, &c) == 0);
  assert(wait_for_flag(&a.ready, 1));
  assert(wait_for_flag(&c.ready, 1));

  store_flag(&a.start, 1);
  assert(wait_for_flag(&race.pause_reached, 1));
  claimed_index = (MSize)(race.claimed[0] - oldnode);
  assert(claimed_index <= oldhmask && claimed_index != target_bucket);
  c_key = bucket_key[claimed_index];
  assert(c_key != anchor_key && c_key != a_key);
  c.key = c_key;

  store_flag(&c.start, 1);
  assert(wait_for_flag(&c.done, 1));
  assert(pthread_join(c_thread, NULL) == 0);
  assert(c.status == 0 && !load_flag(&a.done));
  assert_completed_collision_stages(&race, 1);
  assert(race.anchor[1] == race.claimed[0]);
  assert(lj_tab_nextnode_acq(race.claimed[0]) == race.claimed[1]);
  tabfwd_assert_str_i32(t, c_key, 707);
  assert(string_key_absent(t, a_key));

  lj_tab_resize(L, t, t->asize, lj_fls((uint32_t)oldhmask) + 2u);
  newnode = lj_tab_node_snapshot_acq(t, &newhmask);
  assert(newnode != oldnode && newhmask > oldhmask);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_node_is_retiring(oldnode));
  {
    TValue old_a, old_c;
    lj_tv_load_acq(&old_a, &race.claimed[0]->val);
    lj_tv_load_acq(&old_c, &race.claimed[1]->val);
    assert(tvisnil(&old_a));
    assert(tvisforward(&old_c));
  }
  tabfwd_assert_str_i32(t, anchor_key, 61);
  tabfwd_assert_str_i32(t, c_key, 707);
  assert(string_key_absent(t, a_key));
  assert(tabfwd_count_next_visible(t) == 2);

  store_flag(&race.pause_release, 1);
  assert(pthread_join(a_thread, NULL) == 0);
  finish_race(&race);
  assert(a.status == 0);
  assert_unlinked_duplicate_stages(&race, 0);
  assert_completed_collision_stages(&race, 1);

  assert_generation_unchanged(t, newnode, newhmask);
  a_stats = scan_string_key(newnode, newhmask, a_key);
  c_stats = scan_string_key(newnode, newhmask, c_key);
  assert(a_stats.physical == 1u && a_stats.live == 1u &&
	 a_stats.nil == 0u);
  assert(c_stats.physical == 1u && c_stats.live == 1u &&
	 c_stats.nil == 0u);
  tabfwd_assert_str_i32(t, anchor_key, 61);
  tabfwd_assert_str_i32(t, a_key, 606);
  tabfwd_assert_str_i32(t, c_key, 707);
  assert(tabfwd_count_next_visible(t) == 3);
  assert_quiescent_string_graph(t);
}

int main(void)
{
  lua_State *L = luaL_newstate();

  assert(L != NULL);
  exercise_private_to_shared_tail(L);
  exercise_anchor_same_key_resize(L);
  exercise_same_key_convergence(L);
  exercise_tail_cas_loss_reuses_claim(L);
  exercise_finreg_tail_arbitration(L);
  exercise_claimed_node_anchor(L);
  exercise_claimed_node_resize(L);
  lua_close(L);
  printf("t-tab-newkey-monotonic OK: ordinary publication is monotonic under a paused owner\n");
  return 0;
}
