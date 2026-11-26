/* Explicitly freed raw storage must become reusable after its readers leave.
** Plain storage without a free intent remains live independently of GC marks. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

typedef struct Reader {
  global_State *g;
  unsigned char *p;
  size_t size;
  uint32_t held, release;
} Reader;

static void check_bytes(const void *p, size_t size, unsigned char byte)
{
  const unsigned char *s = (const unsigned char *)p;
  size_t i;
  for (i = 0; i < size; i++) assert(s[i] == byte);
}

static void *hold_reader(void *arg)
{
  Reader *r = (Reader *)arg;
  LJGC2Lease lease;
  assert(lj_gc2_storage_lease_try(r->g, r->p, r->size, &lease, NULL));
  la_store32_rel(&r->held, 1);
  while (!la_load32_acq(&r->release)) sched_yield();
  check_bytes(r->p, r->size, 0xa6);
  lj_gc2_lease_release(&lease);
  return NULL;
}

static void check_owner_drain(lua_State *L, size_t size, int foreign)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  unsigned char *live = (unsigned char *)lj_mem_new_nothrow(L, 113);
  unsigned char *p = (unsigned char *)lj_mem_new_nothrow(L, size);
  GCArena *a;
  uint32_t cell;
  LJGC2Lease lease;
  Reader reader;
  pthread_t thread;
  assert(live && p);
  memset(live, 0x5d, 113);
  memset(p, 0xa6, size);
  a = lj_arena_of(p); cell = lj_arena_cellof(p);
  assert(!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE));
  if (foreign) {
    memset(&reader, 0, sizeof(reader));
    reader.g = G(L); reader.p = p; reader.size = size;
    assert(pthread_create(&thread, NULL, hold_reader, &reader) == 0);
    while (!la_load32_acq(&reader.held)) sched_yield();
  } else {
    assert(lj_gc2_storage_lease_try(G(L), p, size, &lease, NULL));
  }
  lj_mem_free(G(L), p, size);
  assert(lj_arena_late_get(a, cell));
  assert(lj_arena_remote_pending_acq(alloc));
  (void)lj_arena_remote_free_drain(alloc);
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  assert(lj_arena_remote_pending_acq(alloc));
  check_bytes(p, size, 0xa6);
  check_bytes(live, 113, 0x5d);
  if (foreign) {
    la_store32_rel(&reader.release, 1);
    assert(pthread_join(thread, NULL) == 0);
  } else {
    lj_gc2_lease_release(&lease);
  }
  (void)lj_arena_remote_free_drain(alloc);
  assert(!lj_arena_bm_get(a->block, cell));
  assert(!lj_arena_late_get(a, cell));
  check_bytes(live, 113, 0x5d);
  /* The raw live block has no Lua root or mark publication. Its lifetime is
  ** still owned by this C caller, even across complete collections. */
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  check_bytes(live, 113, 0x5d);
  lj_mem_free(G(L), live, 113);
}

static void check_extent_boundaries(void)
{
  TGAlloc alloc;
  PRNGState rs;
  GCArena *a, *reserved;
  unsigned char *p, *q, *guard;
  uint32_t cell, qcell, end;
  LJArenaFreeRun *run;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  p = (unsigned char *)lj_arena_alloc(&alloc, &rs, 4096, 0);
  q = (unsigned char *)lj_arena_alloc(&alloc, &rs, 8192, 0);
  guard = (unsigned char *)lj_arena_alloc(&alloc, &rs, 64, 0);
  assert(p && q && guard);
  a = lj_arena_of(p); qcell = lj_arena_cellof(q);
  assert(lj_arena_of(q) == a && lj_arena_of(guard) == a);
  memset(q, 0xa6, 8192); memset(guard, 0x5d, 64);
  /* Fill the tail through the real allocator, then reserve an earlier freed
  ** run as an interior bump window. The later q extent must use guard's block
  ** boundary, independently of the earlier bump cursor. */
  while (alloc.bump[LJ_ARENAK_PLAIN].cell <
         alloc.bump[LJ_ARENAK_PLAIN].end) {
    size_t size = (size_t)(alloc.bump[LJ_ARENAK_PLAIN].end -
                          alloc.bump[LJ_ARENAK_PLAIN].cell) * LJ_CELL_SIZE;
    if (size > LJ_HUGE_THRESHOLD) size = LJ_HUGE_THRESHOLD;
    assert(lj_arena_alloc(&alloc, &rs, size, 0));
  }
  lj_arena_free(&alloc, p, 4096);
  assert(lj_arena_reserve_bump(&alloc, &rs, 0, 4, &reserved, &cell));
  assert(reserved == a && cell == lj_arena_cellof(p));
  lj_arena_block_set(a, cell);  /* Publish the reserved raw allocation. */
  memset(p, 0x73, 64);
  assert(alloc.bump[LJ_ARENAK_PLAIN].cell < qcell);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  lj_arena_free(&alloc, q, 8192);
  lj_arena_rescue_leave(a);
  /* An interior ownership veto must preserve the complete allocation. */
  end = qcell + lj_arena_ncells(8192);
  lj_arena_bm_set(a->ready, end - 1u);
  assert(lj_arena_remote_free_drain(&alloc) == 0);
  assert(lj_arena_bm_get(a->block, qcell) && lj_arena_late_get(a, qcell));
  check_bytes(q, 8192, 0xa6);
  lj_arena_bm_clear(a->ready, end - 1u);
  assert(lj_arena_remote_free_drain(&alloc) == 1);
  assert(!lj_arena_bm_get(a->block, qcell) && !lj_arena_late_get(a, qcell));
  run = (LJArenaFreeRun *)q;
  assert(run->start == qcell && run->len == lj_arena_ncells(8192));
  check_bytes(guard, 64, 0x5d); check_bytes(p, 64, 0x73);
  lj_arena_alloc_fini(&alloc);
}

static void check_late_api_wakes(void)
{
  unsigned api, side_owner;
  PRNGState rs;
  lj_prng_seed_fixed(&rs);
  for (api = 0; api < 5; api++) {
    for (side_owner = 0; side_owner < 2; side_owner++) {
      TGAlloc alloc;
      LJArenaAllocD ad;
      GCArena *a;
      unsigned char *p, *guard;
      uint32_t cell;
      lj_arena_alloc_init(&alloc);
      lj_arena_allocd_init(&ad, &alloc, &rs, 0);
      p = (unsigned char *)lj_arena_alloc(&alloc, &rs, 64, 0);
      guard = (unsigned char *)lj_arena_alloc(&alloc, &rs, 48, 0);
      assert(p && guard);
      a = lj_arena_of(p); cell = lj_arena_cellof(p);
      memset(p, 0xa6, 64); memset(guard, 0x5d, 48);
      if (side_owner)
        assert(lj_arena_root_state_cas(a, cell,
          LJ_ARENA_ROOT_NONE, LJ_ARENA_ROOT_LINKING));
      else
        assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
      switch (api) {
      case 0: lj_arena_free(&alloc, p, 64); break;
      case 1: assert(lj_arena_remote_free_publish(&alloc, p, 64)); break;
      case 2: assert(lj_arena_free_deferred(&alloc, p, 64)); break;
      case 3: assert(lj_arena_allocf(&ad, p, 64, 0) == NULL); break;
      default:
        assert(lj_arena_realloc(&alloc, &rs, p, 64, 0, 0) ==
               (side_owner ? p : NULL));
        break;
      }
      assert(lj_arena_remote_pending_acq(&alloc));
      assert(lj_arena_late_get(a, cell));
      assert(lj_arena_remote_free_drain(&alloc) == 0);
      assert(lj_arena_remote_pending_acq(&alloc));
      assert(lj_arena_bm_get(a->block, cell));
      check_bytes(p, 64, 0xa6);
      if (side_owner)
        assert(lj_arena_root_state_cas(a, cell,
          LJ_ARENA_ROOT_LINKING, LJ_ARENA_ROOT_NONE));
      else
        lj_arena_rescue_leave(a);
      assert(lj_arena_remote_free_drain(&alloc) == 1);
      assert(!lj_arena_bm_get(a->block, cell) && !lj_arena_late_get(a, cell));
      check_bytes(guard, 48, 0x5d);
      lj_arena_alloc_fini(&alloc);
    }
  }
}

#if defined(LJ_ARENA_TEST_HELPERS)
typedef struct DrainRace {
  TGAlloc *alloc;
  void *p;
  uint32_t count;
  int result;
} DrainRace;

static void *drain_owner(void *arg)
{
  DrainRace *r = (DrainRace *)arg;
  r->count = lj_arena_remote_free_drain(r->alloc);
  return NULL;
}

static void *publish_late(void *arg)
{
  DrainRace *r = (DrainRace *)arg;
  r->result = lj_arena_remote_free_publish(r->alloc, r->p, 64);
  return NULL;
}

static void check_drain_publication_race(void)
{
  TGAlloc alloc;
  PRNGState rs;
  GCArena *a;
  unsigned char *p, *q;
  void *other;
  DrainRace drain = {0}, publish = {0};
  pthread_t owner, producer;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  p = (unsigned char *)lj_arena_alloc(&alloc, &rs, 64, 0);
  q = (unsigned char *)lj_arena_alloc(&alloc, &rs, 64, 0);
  assert(p && q);
  a = lj_arena_of(p); assert(lj_arena_of(q) == a);
  memset(p, 0xa6, 64); memset(q, 0x5d, 64);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  lj_arena_free(&alloc, p, 64);
  lj_arena_rescue_leave(a);
  drain.alloc = publish.alloc = &alloc; publish.p = q;
  /* The owner has closed admission, but a publisher arrives before writer
  ** commit and pauses before storing its late bit. The drain must abort. */
  lj_arena_test_plain_claim_pause(1);
  assert(pthread_create(&owner, NULL, drain_owner, &drain) == 0);
  while (!lj_arena_test_plain_claim_paused()) sched_yield();
  lj_arena_test_plain_late_pause(1);
  assert(pthread_create(&producer, NULL, publish_late, &publish) == 0);
  while (!lj_arena_test_plain_late_paused()) sched_yield();
  lj_arena_test_plain_claim_pause(0);
  assert(pthread_join(owner, NULL) == 0);
  assert(drain.count == 0 && lj_arena_remote_pending_acq(&alloc));
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(p)));
  assert(lj_arena_bm_get(a->block, lj_arena_cellof(q)));
  check_bytes(p, 64, 0xa6); check_bytes(q, 64, 0x5d);
  /* Owner use resumes after join. A stalled publisher must allow allocation
  ** elsewhere, preserving the abandoned tail boundary in the old arena. */
  other = lj_arena_alloc(&alloc, &rs, 64, 0);
  assert(other && lj_arena_of(other) != a);
  assert(lj_arena_remote_free_drain(&alloc) == 0);
  lj_arena_test_plain_late_pause(0);
  assert(pthread_join(producer, NULL) == 0 && publish.result == 1);
  assert(lj_arena_remote_active_acq(a) == 0);
  assert(lj_arena_remote_pending_acq(&alloc));
  assert(lj_arena_remote_free_drain(&alloc) == 2);
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(p)));
  assert(!lj_arena_bm_get(a->block, lj_arena_cellof(q)));
  assert(!lj_arena_late_get(a, lj_arena_cellof(p)));
  assert(!lj_arena_late_get(a, lj_arena_cellof(q)));
  assert(((LJArenaFreeRun *)q)->len == 4);
  lj_arena_alloc_fini(&alloc);
}

static void check_live_growth_skips_drain(void)
{
  TGAlloc alloc;
  PRNGState rs;
  unsigned i;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  lj_arena_test_remote_stats_reset();
  for (i = 0; i < 32; i++)
    assert(lj_arena_alloc(&alloc, &rs, 16000, 0));
  assert(lj_arena_test_remote_fast_skips() > 0);
  assert(lj_arena_test_remote_arena_probes() == 0);
  lj_arena_alloc_fini(&alloc);
}

static void check_late_publish_after_clear(void)
{
  TGAlloc alloc;
  PRNGState rs;
  DrainRace drain = {0};
  pthread_t owner;
  GCArena *a;
  void *p, *q;
  lj_prng_seed_fixed(&rs);
  lj_arena_alloc_init(&alloc);
  p = lj_arena_alloc(&alloc, &rs, 64, 0);
  q = lj_arena_alloc(&alloc, &rs, 64, 0);
  assert(p && q && lj_arena_of(p) == lj_arena_of(q));
  a = lj_arena_of(p);
  assert(lj_arena_rescue_enter(a) == LJ_ARENA_RESCUE_FULL);
  lj_arena_free(&alloc, p, 64);
  drain.alloc = &alloc;
  lj_arena_test_remote_drain_pause(1);
  assert(pthread_create(&owner, NULL, drain_owner, &drain) == 0);
  while (!lj_arena_test_remote_drain_paused()) sched_yield();
  assert(!lj_arena_remote_pending_acq(&alloc));
  assert(lj_arena_remote_free_publish(&alloc, q, 64));
  assert(lj_arena_remote_pending_acq(&alloc));
  lj_arena_test_remote_drain_pause(0);
  assert(pthread_join(owner, NULL) == 0);
  assert(drain.count == 0 && lj_arena_remote_pending_acq(&alloc));
  lj_arena_rescue_leave(a);
  assert(lj_arena_remote_free_drain(&alloc) == 2);
  assert(!lj_arena_late_get(a, lj_arena_cellof(p)));
  assert(!lj_arena_late_get(a, lj_arena_cellof(q)));
  lj_arena_alloc_fini(&alloc);
}
#endif

#if LJ_HASBUFFER
static int plain_arenas(lua_State *L)
{
  TGAlloc *alloc = &L2TG(L)->alloc;
  GCArena *heads[4] = { alloc->owned[LJ_ARENAK_PLAIN],
    alloc->needsweep[LJ_ARENAK_PLAIN], alloc->quarantine[LJ_ARENAK_PLAIN],
    alloc->reclaimed[LJ_ARENAK_PLAIN] };
  uint32_t i, count = 0;
  for (i = 0; i < 4; i++) {
    GCArena *a;
    for (a = heads[i]; a; a = lj_arena_next_acq(a)) {
      assert(++count < 8192);
      assert(!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE));
    }
  }
  lua_pushinteger(L, count);
  return 1;
}

static void check_refill_reuse(lua_State *L)
{
  lua_register(L, "plain_arenas", plain_arenas);
  ljt_lua_dostring(L,
    "local buffer = require('string.buffer')\n"
    "local havejit, jit = pcall(require, 'jit')\n"
    "local modes = {false}; if havejit and jit.status() then modes[2] = true end\n"
    "local function refill(b, s, n)\n"
    "  for _ = 1, n do b:reset():put(s); assert(b:get() == s) end\n"
    "end\n"
    "for _, mode in ipairs(modes) do\n"
    "  if havejit then if mode then jit.on() else jit.off() end end\n"
    "  collectgarbage('collect'); collectgarbage('stop')\n"
    "  local b, s = buffer.new(1024), string.rep('R', 1024)\n"
    "  refill(b, s, 1000)\n"
    "  local first = plain_arenas()\n"
    "  for round = 1, 5 do\n"
    "    refill(b, s, 5000)\n"
    "    assert(plain_arenas() <= first + 8, 'raw refill storage keeps growing')\n"
    "  end\n"
    "  assert(#b == 0); b:free()\n"
    "end\n");
}
#endif

int main(void)
{
  static const size_t sizes[] = { 17, 48, 4144, 8192 };
  lua_State *L = ljt_lua_newstate_openlibs();
  unsigned i;
  alarm(60);
  lua_gc(L, LUA_GCSTOP, 0);
  check_extent_boundaries();
  check_late_api_wakes();
#if defined(LJ_ARENA_TEST_HELPERS)
  check_drain_publication_race();
  check_live_growth_skips_drain();
  check_late_publish_after_clear();
#endif
  for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
    check_owner_drain(L, sizes[i], 0);
    check_owner_drain(L, sizes[i], 1);
  }
#if LJ_HASBUFFER
  check_refill_reuse(L);
#endif
  lua_close(L);
  alarm(0);
  puts("t-arena-plain-late OK: retained readers, unfreed raw storage and bounded refill reuse");
  return 0;
}
