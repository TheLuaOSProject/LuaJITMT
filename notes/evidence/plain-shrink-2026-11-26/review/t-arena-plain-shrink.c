/* A positive small raw shrink retains its original physical allocation.
** Readers and allocation failure must not turn it into a waiting operation. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"

#if !defined(LJ_ARENA_TEST_HELPERS)
#error "t-arena-plain-shrink requires LJ_ARENA_TEST_HELPERS"
#endif

static void init_alloc(lua_State *L, TGAlloc *alloc)
{
  lj_arena_alloc_init(alloc);
  lj_arena_alloc_owner_rel(alloc, lj_tg_tid_acq(L2TG(L)));
  lj_arena_alloc_owner_tg_rel(alloc, L2TG(L));
  lj_arena_alloc_set_registry(alloc,
    (HugeTab *)gc2_small_arena_tab_acq(G(L)));
}

static unsigned char *alloc_bytes(TGAlloc *alloc, PRNGState *rs,
                                  size_t size, unsigned char byte)
{
  unsigned char *p = (unsigned char *)lj_arena_alloc(alloc, rs, size, 0);
  assert(p != NULL);
  memset(p, byte, size);
  return p;
}

static void check_bytes(const void *p, size_t size, unsigned char byte)
{
  const unsigned char *s = (const unsigned char *)p;
  size_t i;
  for (i = 0; i < size; i++) assert(s[i] == byte);
}

static void check_free_extent(void *p, size_t physical)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  LJArenaFreeRun *run = (LJArenaFreeRun *)p;
  assert(!lj_arena_bm_get(a->block, cell));
  assert(lj_arena_bm_get(a->mark, cell));
  assert(!lj_arena_plain_oversize_acq(a, cell));
  assert(!lj_arena_late_get(a, cell));
  assert(run->start == cell && run->len == lj_arena_ncells(physical));
}

/* Exhaust only the current private window through actual allocations. The
** next allocation can then prove that the complete old extent is reusable. */
static void exhaust_bump(TGAlloc *alloc, PRNGState *rs)
{
  LJArenaBump *b = &alloc->bump[LJ_ARENAK_PLAIN];
  while (b->a && b->cell < b->end) {
    size_t size = (size_t)(b->end - b->cell) << LJ_CELL_SHIFT;
    void *next = lj_arena_cellptr(b->a, b->cell);
    if (size > LJ_HUGE_THRESHOLD) size = LJ_HUGE_THRESHOLD;
    assert(lj_arena_alloc(alloc, rs, size, 0) == next);
  }
}

static void check_reuse(TGAlloc *alloc, PRNGState *rs,
                        void *p, size_t physical)
{
  void *fresh;
  check_free_extent(p, physical);
  exhaust_bump(alloc, rs);
  fresh = lj_arena_alloc(alloc, rs, physical, 0);
  assert(fresh == p);
  assert(!lj_arena_plain_oversize_acq(lj_arena_of(fresh),
                                    lj_arena_cellof(fresh)));
  memset(fresh, 0x37, physical);
  lj_arena_free(alloc, fresh, physical);
}

typedef struct Shrink {
  TGAlloc *alloc;
  PRNGState *rs;
  void *p, *result;
  size_t oldsize, newsize;
} Shrink;

static void *shrink_thread(void *arg)
{
  Shrink *s = (Shrink *)arg;
  s->result = lj_arena_realloc(s->alloc, s->rs, s->p,
                              s->oldsize, s->newsize, 0);
  return NULL;
}

static void check_paused_shrink(lua_State *L, PRNGState *rs)
{
  TGAlloc alloc;
  unsigned char *p, *guard;
  GCArena *a;
  LJGC2Lease held, fresh, marked;
  size_t span = 0;
  pthread_t thread;
  Shrink s;
  init_alloc(L, &alloc);
  p = alloc_bytes(&alloc, rs, 4096, 0xa6);
  guard = alloc_bytes(&alloc, rs, 64, 0x5d);
  a = lj_arena_of(p);
  assert(lj_arena_of(guard) == a && guard == p + 4096);
  assert(lj_gc2_storage_lease_try(G(L), p, 4096, &held, &span));
  assert(span == 4096);
  s.alloc = &alloc; s.rs = rs; s.p = p; s.result = NULL;
  s.oldsize = 4096; s.newsize = 17;
  lj_arena_test_plain_shrink_pause(a, 1);
  assert(pthread_create(&thread, NULL, shrink_thread, &s) == 0);
  while (!lj_arena_test_plain_shrink_paused()) sched_yield();
  /* Both the old full-span lease and newly arriving body readers retain the
  ** same complete bytes while the shrinking owner remains stopped. */
  assert(lj_gc2_storage_lease_try(G(L), p, 4096, &fresh, &span));
  assert(span == 4096);
  assert(lj_gc2_mem_lease_acquire(G(L), p, &marked) >= 0);
  assert(!(lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_STATE_MASK));
  check_bytes(p, 4096, 0xa6);
  check_bytes(guard, 64, 0x5d);
  assert(lj_arena_test_plain_shrink_paused());
  lj_arena_test_plain_shrink_pause(a, 0);
  assert(pthread_join(thread, NULL) == 0 && s.result == p);
  lj_gc2_lease_release(&fresh);
  lj_gc2_lease_release(&marked);
  assert(lj_gc2_storage_lease_try(G(L), p, 4096, &fresh, &span));
  assert(span == 4096);
  lj_gc2_lease_release(&fresh);
  lj_arena_free(&alloc, p, 17);
  assert(lj_arena_remote_free_drain(&alloc) == 0);
  check_bytes(p, 4096, 0xa6);
  lj_gc2_lease_release(&held);
  assert(lj_arena_remote_free_drain(&alloc) == 1);
  check_reuse(&alloc, rs, p, 4096);
  check_bytes(guard, 64, 0x5d);
  lj_arena_alloc_fini(&alloc);
}

static void check_infallible_shrink(lua_State *L, PRNGState *rs,
                                    unsigned side)
{
  TGAlloc alloc;
  unsigned char *p, *guard, *gate;
  GCArena *a;
  LJGC2Lease held;
  uint32_t cell;
  uint64_t active, failures;
  init_alloc(L, &alloc);
  p = alloc_bytes(&alloc, rs, 4096, 0xa6);
  guard = alloc_bytes(&alloc, rs, 64, 0x5d);
  gate = alloc_bytes(&alloc, rs, 64, 0x73);
  a = lj_arena_of(p); cell = lj_arena_cellof(p);
  assert(lj_arena_of(gate) == a);
  if (side == 1)
    assert(lj_arena_root_state_cas(a, cell,
      LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_LINKING));
  if (side == 2)
    assert(lj_arena_recovery_state_cas(a, cell,
      LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING));
  assert(lj_arena_destruct_acquire(gate, 64) == LJ_ARENA_DESTRUCT_ACQUIRED);
  assert(lj_gc2_storage_lease_try(G(L), p, 4096, &held, NULL));
  active = lj_arena_remote_active_acq(a);
  lj_arena_test_alloc_fail(1);
  assert(lj_arena_alloc(&alloc, rs, 32, 0) == NULL);
  failures = lj_arena_test_alloc_failures();
  assert(failures != 0);
  assert(lj_arena_realloc(&alloc, rs, p, 4096, 1025, 0) == p);
  assert(lj_arena_realloc(&alloc, rs, p, 1025, 17, 0) == p);
  assert(lj_arena_realloc(&alloc, rs, p, 17, 1, 0) == p);
  assert(lj_arena_test_alloc_failures() == failures);
  lj_arena_test_alloc_fail(0);
  assert(lj_arena_remote_active_acq(a) == active);
  assert(!lj_arena_late_get(a, cell));
  check_bytes(p, 4096, 0xa6);
  lj_arena_free(&alloc, p, 1);
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_bm_get(a->block, cell));
  check_bytes(p, 4096, 0xa6);
  lj_arena_free(&alloc, gate, 64);
  assert(lj_arena_remote_free_drain(&alloc) == 0);
  if (side == 1)
    assert(lj_arena_root_state_cas(a, cell,
      LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE));
  if (side == 2)
    assert(lj_arena_recovery_state_cas(a, cell,
      LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_IDLE));
  lj_gc2_lease_release(&held);
  assert(lj_arena_remote_free_drain(&alloc) == 1);
  check_reuse(&alloc, rs, p, 4096);
  check_bytes(guard, 64, 0x5d);
  lj_arena_alloc_fini(&alloc);
}

typedef struct RemoteFree {
  TGAlloc *alloc;
  void *p;
  size_t size;
} RemoteFree;

static void *remote_free_thread(void *arg)
{
  RemoteFree *f = (RemoteFree *)arg;
  assert(lj_arena_remote_free_publish(f->alloc, f->p, f->size));
  return NULL;
}

static void check_free_routes(lua_State *L, PRNGState *rs)
{
  unsigned route;
  for (route = 0; route < 6; route++) {
    TGAlloc alloc;
    LJArenaAllocD ad;
    unsigned char *p, *guard;
    GCArena *a;
    init_alloc(L, &alloc);
    lj_arena_allocd_init(&ad, &alloc, rs, 0);
    p = alloc_bytes(&alloc, rs, 4096, 0xa6);
    guard = alloc_bytes(&alloc, rs, 64, 0x5d);
    a = lj_arena_of(p);
    assert(lj_arena_realloc(&alloc, rs, p, 4096, 17, 0) == p);
    switch (route) {
    case 0: lj_arena_free(&alloc, p, 17); break;
    case 1: {
      pthread_t producer;
      RemoteFree f = { &alloc, p, 17 };
      assert(pthread_create(&producer, NULL, remote_free_thread, &f) == 0);
      assert(pthread_join(producer, NULL) == 0);
      /* The foreign caller cannot infer the owner-private bump endpoint. */
      assert(la_loadptr_acq((void *const *)&a->hdr.remote_free) == NULL);
      assert(lj_arena_late_get(a, lj_arena_cellof(p)));
      check_bytes(p, 4096, 0xa6);
      assert(lj_arena_remote_free_drain(&alloc) == 1);
      break;
    }
    case 2:
      assert(lj_arena_free_deferred(&alloc, p, 17));
      check_bytes(p, 4096, 0xa6);
      assert(lj_arena_remote_free_drain(&alloc) == 1);
      break;
    case 3:
      assert(lj_arena_destruct_acquire(p, 17) == LJ_ARENA_DESTRUCT_ACQUIRED);
      memset(p, 0x84, 17);
      lj_arena_free(&alloc, p, 17);
      break;
    case 4: assert(lj_arena_allocf(&ad, p, 17, 0) == NULL); break;
    default: assert(lj_arena_realloc(&alloc, rs, p, 17, 0, 0) == NULL); break;
    }
    check_reuse(&alloc, rs, p, 4096);
    check_bytes(guard, 64, 0x5d);
    lj_arena_alloc_fini(&alloc);
  }
}

static void check_shrink_grow(lua_State *L, PRNGState *rs)
{
  TGAlloc alloc;
  unsigned char *p, *next;
  size_t size = 4096;
  unsigned round;
  init_alloc(L, &alloc);
  p = alloc_bytes(&alloc, rs, size, 0xa6);
  for (round = 0; round < 2; round++) {
    LJGC2Lease held;
    assert(lj_gc2_storage_lease_try(G(L), p, size, &held, NULL));
    assert(lj_arena_realloc(&alloc, rs, p, size, 17, 0) == p);
    next = (unsigned char *)lj_arena_realloc(&alloc, rs, p, 17, size * 2, 0);
    assert(next && next != p);
    check_bytes(next, 17, 0xa6);
    check_bytes(p, size, 0xa6);
    assert(lj_arena_late_get(lj_arena_of(p), lj_arena_cellof(p)));
    lj_gc2_lease_release(&held);
    assert(lj_arena_remote_free_drain(&alloc) == 1);
    check_free_extent(p, size);
    p = next; size *= 2;
    memset(p, 0xa6, size);
  }
  assert(lj_arena_realloc(&alloc, rs, p, size, 1, 0) == p);
  lj_arena_free(&alloc, p, 1);
  check_free_extent(p, size);
  lj_arena_alloc_fini(&alloc);
}

static void check_bump_extents(lua_State *L, PRNGState *rs)
{
  TGAlloc alloc;
  unsigned char *p, *early, *guard;
  GCArena *a, *reserved;
  uint32_t cell;
  init_alloc(L, &alloc);
  p = alloc_bytes(&alloc, rs, 4096, 0xa6);
  /* No later block or mark bounds this last allocation: its active owner
  ** cursor must recover the full physical extent after logical shrink. */
  assert(lj_arena_realloc(&alloc, rs, p, 4096, 17, 0) == p);
  lj_arena_free(&alloc, p, 17);
  check_reuse(&alloc, rs, p, 4096);
  lj_arena_alloc_fini(&alloc);

  init_alloc(L, &alloc);
  early = alloc_bytes(&alloc, rs, 4096, 0x73);
  p = alloc_bytes(&alloc, rs, 8192, 0xa6);
  guard = alloc_bytes(&alloc, rs, 64, 0x5d);
  a = lj_arena_of(p);
  exhaust_bump(&alloc, rs);
  lj_arena_free(&alloc, early, 4096);
  assert(lj_arena_reserve_bump(&alloc, rs, 0, 4, &reserved, &cell));
  assert(reserved == a && cell == lj_arena_cellof(early));
  lj_arena_block_set(a, cell);
  memset(early, 0x73, 64);
  assert(alloc.bump[LJ_ARENAK_PLAIN].cell < lj_arena_cellof(p));
  assert(lj_arena_realloc(&alloc, rs, p, 8192, 17, 0) == p);
  lj_arena_free(&alloc, p, 17);
  check_reuse(&alloc, rs, p, 8192);
  check_bytes(early, 64, 0x73);
  check_bytes(guard, 64, 0x5d);
  lj_arena_alloc_fini(&alloc);
}

static void check_transfer_refusal(lua_State *L, PRNGState *rs)
{
  TGAlloc src, dst;
  unsigned char *p;
  GCArena *a;
  uint32_t tail;
  init_alloc(L, &src); init_alloc(L, &dst);
  p = alloc_bytes(&src, rs, 4096, 0xa6);
  a = lj_arena_of(p); tail = src.bump[LJ_ARENAK_PLAIN].cell;
  assert(lj_arena_realloc(&src, rs, p, 4096, 17, 0) == p);
  /* A pending side owner refuses tail publication. Transfer still has to
  ** retain the old allocation's end before discarding its source cursor. */
  assert(lj_arena_root_state_cas(a, tail + 1,
    LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_LINKING));
  assert(lj_arena_alloc_transfer(&dst, &src) == 1);
  assert(src.bump[LJ_ARENAK_PLAIN].a == NULL);
  assert(lj_arena_root_state_acq(a, tail + 1) == LJ_ARENA_ROOT_LINKING);
  assert(lj_arena_bm_get(a->mark, tail));
  check_bytes(p, 4096, 0xa6);
  lj_arena_free(&dst, p, 17);
  check_free_extent(p, 4096);
  assert(lj_arena_root_state_cas(a, tail + 1,
    LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE));
  check_reuse(&dst, rs, p, 4096);
  lj_arena_alloc_fini(&src); lj_arena_alloc_fini(&dst);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  PRNGState rs;
  unsigned side;
  assert(L != NULL);
  alarm(60);
  lua_gc(L, LUA_GCSTOP, 0);
  lj_prng_seed_fixed(&rs);
  check_paused_shrink(L, &rs);
  for (side = 0; side < 3; side++) check_infallible_shrink(L, &rs, side);
  check_free_routes(L, &rs);
  check_shrink_grow(L, &rs);
  check_bump_extents(L, &rs);
  check_transfer_refusal(L, &rs);
  lua_close(L);
  alarm(0);
  puts("t-arena-plain-shrink OK: retained readers, infallible shrink and full extent reuse");
  return 0;
}
