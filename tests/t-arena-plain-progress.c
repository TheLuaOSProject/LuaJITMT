/* A plain writer must not deny readers of unrelated live allocations. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_buf.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_ARENA_TEST_HELPERS)
#error "t-arena-plain-progress requires LJ_ARENA_TEST_HELPERS"
#endif

typedef struct Publish {
  TGAlloc *alloc;
  void *p;
  int result;
} Publish;

static void *publish_free(void *arg)
{
  Publish *p = (Publish *)arg;
  p->result = lj_arena_remote_free_publish(p->alloc, p->p, 64);
  return NULL;
}

static void pause_at(int committed, int enabled)
{
  if (committed) lj_arena_test_remote_publish_pause(enabled);
  else lj_arena_test_plain_claim_pause(enabled);
}

static int paused_at(int committed)
{
  return committed ? lj_arena_test_remote_publish_paused() :
                     lj_arena_test_plain_claim_paused();
}

static void check_bounded_reader(global_State *g, GCArena *a, int committed)
{
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  uint32_t slot, count = lj_arena_hugetab_slot_count(registry);
  for (slot = 0; slot < count; slot++) {
    void *base;
    LJHugeInfo info;
    int result = lj_arena_hugetab_slot_snapshot_bounded(
      registry, slot, &base, &info);
    if (result == LJ_ARENA_HUGETAB_SLOT_PRESENT && base == a) {
      result = lj_arena_hugetab_rescue_slot_enter_bounded(
        registry, slot, a, &info);
      assert(result == (committed ? LJ_ARENA_RESCUE_COMMITTED :
                                    LJ_ARENA_RESCUE_BIT_ONLY));
      lj_arena_rescue_leave(a);
      return;
    }
  }
  assert(0 && "registered plain arena missing from directory");
}

static void check_raw_reader(lua_State *L, int committed)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  unsigned char *p = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  unsigned char *q = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  Publish work = { alloc, p, 0 };
  LJGC2Lease first, second, marked, target;
  pthread_t writer;
  int admitted, completed = 0;
  assert(p && q && lj_arena_of(p) == lj_arena_of(q));
  memset(q, 0x6d, 64);
  assert(lj_gc2_storage_lease_try(G(L), q, 64, &first, NULL));
  lj_gc2_lease_release(&first);
  pause_at(committed, 1);
  assert(pthread_create(&writer, NULL, publish_free, &work) == 0);
  while (!paused_at(committed)) sched_yield();
  admitted = lj_gc2_storage_lease_try(G(L), q, 64, &first, NULL);
  if (admitted) {
    assert(q[0] == 0x6d && q[63] == 0x6d);
    assert(!lj_gc2_storage_lease_try(G(L), p, 64, &target, NULL));
    assert(lj_gc2_mem_lease_acquire(G(L), p, &target) < 0);
    assert(lj_gc2_mem_lease_acquire(G(L), q, &marked) >= 0);
    assert(marked.arena == lj_arena_of(q));
    lj_gc2_lease_release(&marked);
    check_bounded_reader(G(L), lj_arena_of(q), committed);
  }
  assert(paused_at(committed));
  pause_at(committed, 0);
  assert(pthread_join(writer, NULL) == 0 && work.result == 1);
  if (admitted) {
    /* The first reader spans writer completion. A completed generation must
    ** admit another reader without waiting for that earlier reader to leave. */
    completed = lj_gc2_storage_lease_try(G(L), q, 64, &second, NULL);
    if (completed) lj_gc2_lease_release(&second);
    assert(q[0] == 0x6d && q[63] == 0x6d);
    lj_gc2_lease_release(&first);
  }
  (void)lj_arena_remote_free_drain(alloc);
  assert(!lj_arena_bm_get(lj_arena_of(p)->block, lj_arena_cellof(p)));
  lj_arena_free(alloc, q, 64);
  if (!admitted || !completed)
    fprintf(stderr, "unrelated reader denied: committed=%d first=%d completed=%d\n",
            committed, admitted, completed);
  assert(admitted && completed);
}

static void check_destructor_identity(lua_State *L)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  void *p = lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  unsigned char *q = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  unsigned char *live = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  GCArena *a = lj_arena_of(p);
  uint32_t qcell = lj_arena_cellof(q);
  LJGC2Lease lease;
  assert(p && q && live && lj_arena_of(q) == a && lj_arena_of(live) == a);
  memset(q, 0x35, 64); memset(live, 0x79, 64);
  /* A defensive interior-owner veto leaves q FREEING+late. That state must
  ** not let a later free borrow the distinct retained destructor for p. */
  assert(lj_arena_root_state_cas(a, qcell + 1u,
    LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_LINKING));
  lj_arena_free(alloc, q, 64);
  assert(lj_arena_late_get(a, qcell));
  assert(lj_arena_sweep_state_acq(a, qcell) == LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_root_state_cas(a, qcell + 1u,
    LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE));
  assert(lj_arena_destruct_acquire(p, 64) == LJ_ARENA_DESTRUCT_ACQUIRED);
  assert(lj_arena_destruct_acquire(p, 64) == LJ_ARENA_DESTRUCT_OWNED);
  assert(lj_gc2_storage_lease_try(G(L), live, 64, &lease, NULL));
  lj_arena_free(alloc, q, 64);
  assert((lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_STATE_MASK) ==
    (LJ_ARENA_REMOTE_SEALED | LJ_ARENA_REMOTE_PENDING));
  assert(lj_arena_bm_get(a->block, qcell) && q[0] == 0x35 && q[63] == 0x35);
  /* Duplicate late intent does not revoke p's exact semantic owner. */
  assert(lj_arena_free_deferred(alloc, p, 64));
  lj_arena_free(alloc, p, 64);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(p)));
  assert(!(lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_SEALED));
  assert(live[0] == 0x79 && live[63] == 0x79);
  lj_gc2_lease_release(&lease);
  (void)lj_arena_remote_free_drain(alloc);
  assert(!lj_arena_bm_get(a->block, qcell));
  lj_arena_free(alloc, live, 64);
}

typedef struct OwnerWork {
  TGAlloc *alloc;
  PRNGState *prng;
  void *p, *result;
  uint32_t drained;
} OwnerWork;

static void *drain_owner(void *arg)
{
  OwnerWork *w = (OwnerWork *)arg;
  w->drained = lj_arena_remote_free_drain(w->alloc);
  return NULL;
}

static void *shrink_owner(void *arg)
{
  OwnerWork *w = (OwnerWork *)arg;
  w->result = lj_arena_realloc(w->alloc, w->prng, w->p, 128, 64, 0);
  return NULL;
}

static void check_late_cohort(lua_State *L)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  void *p = lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  unsigned char *q = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  GCArena *a = lj_arena_of(p);
  LJGC2Lease lease;
  OwnerWork work = { alloc, &G(L)->prng, NULL, NULL, 0 };
  pthread_t owner;
  assert(p && q && lj_arena_of(q) == a);
  memset(q, 0x52, 64);
  assert(lj_gc2_storage_lease_try(G(L), p, 64, &lease, NULL));
  lj_arena_free(alloc, p, 64);
  assert(lj_arena_late_get(a, lj_arena_cellof(p)));
  lj_gc2_lease_release(&lease);
  lj_arena_test_plain_commit_pause(a, 1);
  /* Only the worker uses the owner allocator until join. The main thread
  ** uses registered body leases and the foreign-free publication API. */
  assert(pthread_create(&owner, NULL, drain_owner, &work) == 0);
  while (!lj_arena_test_plain_commit_paused()) sched_yield();
  assert(lj_gc2_storage_lease_try(G(L), q, 64, &lease, NULL));
  assert(lj_arena_remote_free_publish(alloc, q, 64));
  assert(lj_arena_late_get(a, lj_arena_cellof(q)));
  assert(lj_arena_test_plain_commit_paused());
  lj_arena_test_plain_commit_pause(a, 0);
  assert(pthread_join(owner, NULL) == 0 && work.drained == 1);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(p)));
  /* q became late after this drain committed. Its earlier admitted reader
  ** still owns the complete payload, even after that drain has returned. */
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(q)));
  assert(q[0] == 0x52 && q[63] == 0x52);
  assert(lj_arena_remote_pending_acq(alloc));
  assert(lj_arena_remote_free_drain(alloc) == 0);
  assert(q[0] == 0x52 && q[63] == 0x52);
  lj_gc2_lease_release(&lease);
  assert(lj_arena_remote_free_drain(alloc) == 1);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(q)));
}

static void check_shrink(lua_State *L)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  unsigned char *p = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 128, 0);
  unsigned char *q = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  GCArena *a = lj_arena_of(p);
  LJGC2Lease unrelated, target;
  OwnerWork work = { alloc, &G(L)->prng, p, NULL, 0 };
  pthread_t owner;
  size_t span;
  void *moved;
  assert(p && q && lj_arena_of(q) == a);
  memset(p, 0xa1, 128); memset(q, 0x6e, 64);
  lj_arena_test_plain_commit_pause(a, 1);
  assert(pthread_create(&owner, NULL, shrink_owner, &work) == 0);
  while (!lj_arena_test_plain_commit_paused()) sched_yield();
  assert(lj_gc2_storage_lease_try(G(L), q, 64, &unrelated, NULL));
  assert(!lj_gc2_storage_lease_try(G(L), p, 128, &target, NULL));
  lj_arena_test_plain_commit_pause(a, 0);
  assert(pthread_join(owner, NULL) == 0 && work.result == p);
  assert(lj_gc2_storage_lease_try(G(L), p, 64, &target, &span));
  assert(span == 64 && p[0] == 0xa1 && p[63] == 0xa1);
  assert(q[0] == 0x6e && q[63] == 0x6e);
  lj_gc2_lease_release(&target);
  assert(!lj_gc2_storage_lease_try(G(L), p, 128, &target, NULL));
  lj_gc2_lease_release(&unrelated);
  lj_arena_free(alloc, p, 64); lj_arena_free(alloc, q, 64);

  /* A reader which predates shrink keeps its original full extent. Shrink
  ** moves on admission conflict and returns while that reader stays held. */
  p = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 128, 0);
  assert(p); memset(p, 0xb2, 128);
  assert(lj_gc2_storage_lease_try(G(L), p, 128, &target, NULL));
  moved = lj_arena_realloc(alloc, &G(L)->prng, p, 128, 64, 0);
  assert(moved && moved != p && memcmp(moved, p, 64) == 0);
  assert(p[0] == 0xb2 && p[127] == 0xb2);
  lj_gc2_lease_release(&target);
  (void)lj_arena_remote_free_drain(alloc);
  assert(!lj_arena_bm_get(lj_arena_of(p)->block, lj_arena_cellof(p)));
  lj_arena_free(alloc, moved, 64);
}

#if LJ_HASBUFFER
static void check_public_buffer(lua_State *L)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  LJBuffer *buf;
  void *store, *p;
  Publish work;
  pthread_t writer;
  assert(lua_gettop(L) == 0);
  ljt_lua_dostring(L,
    "local b = require('string.buffer').new():put('before')\n"
    "return b, function(b)\n"
    "  assert(b:tostring() == 'before')\n"
    "  b:put('-after'); assert(b:get() == 'before-after')\n"
    "  b:free():put('fresh'); assert(b:get() == 'fresh')\n"
    "end\n");
  assert(lua_gettop(L) == 2 && tvisbuf(L->top - 2));
  buf = bufV(L->top - 2);
  store = (void *)(uintptr_t)la_load64_acq(&buf->range.lo);
  p = lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  assert(p && store && lj_arena_of(p) == lj_arena_of(store));
  work.alloc = alloc; work.p = p; work.result = 0;
  pause_at(1, 1);
  assert(pthread_create(&writer, NULL, publish_free, &work) == 0);
  while (!paused_at(1)) sched_yield();
  lua_pushvalue(L, 1);
  ljt_lua_pcall(L, 1, 0, "buffer access during unrelated plain free");
  assert(paused_at(1));
  pause_at(1, 0);
  assert(pthread_join(writer, NULL) == 0 && work.result == 1);
  (void)lj_arena_remote_free_drain(alloc);
  lua_settop(L, 0);
}
#endif

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  alarm(60);
  lua_gc(L, LUA_GCSTOP, 0);
  check_raw_reader(L, 0);
  check_raw_reader(L, 1);
  check_destructor_identity(L);
  check_late_cohort(L);
  check_shrink(L);
#if LJ_HASBUFFER
  check_public_buffer(L);
#endif
  lua_close(L);
  alarm(0);
  puts("t-arena-plain-progress OK: tentative, committed and completed writers");
  return 0;
}
