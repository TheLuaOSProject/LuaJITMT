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

static void check_raw_reader(lua_State *L, int committed)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  unsigned char *p = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  unsigned char *q = (unsigned char *)lj_arena_alloc(alloc, &G(L)->prng, 64, 0);
  Publish work = { alloc, p, 0 };
  LJGC2Lease first, second;
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
  if (admitted) assert(q[0] == 0x6d && q[63] == 0x6d);
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
#if LJ_HASBUFFER
  check_public_buffer(L);
#endif
  lua_close(L);
  alarm(0);
  puts("t-arena-plain-progress OK: tentative, committed and completed writers");
  return 0;
}
