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
  (void)lj_arena_remote_free_drain_force(alloc);
  assert(lj_arena_bm_get(a->block, cell) && lj_arena_late_get(a, cell));
  check_bytes(p, size, 0xa6);
  check_bytes(live, 113, 0x5d);
  if (foreign) {
    la_store32_rel(&reader.release, 1);
    assert(pthread_join(thread, NULL) == 0);
  } else {
    lj_gc2_lease_release(&lease);
  }
  (void)lj_arena_remote_free_drain_force(alloc);
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
    "local function refill(b, s, n)\n"
    "  for _ = 1, n do b:reset():put(s); assert(b:get() == s) end\n"
    "end\n"
    "for _, mode in ipairs({false, true}) do\n"
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

int main(void)
{
  static const size_t sizes[] = { 17, 48, 4144, 8192 };
  lua_State *L = ljt_lua_newstate_openlibs();
  unsigned i;
  alarm(60);
  lua_gc(L, LUA_GCSTOP, 0);
  for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
    check_owner_drain(L, sizes[i], 0);
    check_owner_drain(L, sizes[i], 1);
  }
  check_refill_reuse(L);
  lua_close(L);
  alarm(0);
  puts("t-arena-plain-late OK: retained readers, unfreed raw storage and bounded refill reuse");
  return 0;
}
