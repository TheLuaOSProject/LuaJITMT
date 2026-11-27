/* A detached PLAIN owner can lose individual mappings during live SWEEP. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

enum { BlockSize = 512, ProbeBlocks = 24 };

static TGState *dead;
static GCArena *blocked, *empty, *late_arena;
static void *late_block;
static LJHugeReader directory_reader;
static LJGC2Lease body_reader;

static int all_free(GCArena *a)
{
  uint32_t w;
  for (w = 0; w < LJ_ARENA_WORDS; w++)
    if (la_load64_acq(&a->block[w]) || la_load64_acq(&a->late[w]))
      return 0;
  return 1;
}

static int seed_plain(lua_State *L)
{
  global_State *g = G(L);
  TGAlloc *alloc = &L2TG(L)->alloc;
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  void *probes[ProbeBlocks];
  unsigned count = 0;
  assert(dead == NULL);
  dead = L2TG(L);
  late_block = lj_mem_new(L, BlockSize);
  late_arena = lj_arena_of(late_block);
  memset(late_block, 0x5a, BlockSize);
  do {
    assert(count < ProbeBlocks);
    probes[count++] = lj_mem_new(L, LJ_HUGE_THRESHOLD);
    blocked = alloc->owned[LJ_ARENAK_PLAIN];
    empty = lj_arena_next_acq(blocked);
  } while (!empty || empty == late_arena);
  assert(blocked != late_arena);
  while (count != 0)
    lj_mem_free(g, probes[--count], LJ_HUGE_THRESHOLD);
  assert(all_free(blocked) && all_free(empty));
  /* Model a reader paused between directory admission and arena handoff. */
  assert(lj_arena_hugetab_reader_acquire(registry, blocked,
    &directory_reader, NULL) == LJ_ARENA_HUGE_READER_ACQUIRED);
  return 0;
}

static int hold_late(lua_State *L)
{
  global_State *g = G(L);
  assert(lj_tg_flags_test_acq(dead, TGF_DEAD));
  assert(all_free(blocked) && all_free(empty));
  assert(lj_gc2_small_lease_try(g, late_block, BlockSize, 0,
    &body_reader, NULL));
  lj_mem_free(g, late_block, BlockSize);
  assert(lj_arena_late_get(late_arena, lj_arena_cellof(late_block)));
  return 0;
}

static int unmapped(HugeTab *registry, GCArena *a)
{
  unsigned char resident;
  assert(lj_arena_hugetab_lookup(registry, a, NULL) == 0);
  errno = 0;
  return mincore(a, (size_t)sysconf(_SC_PAGESIZE), &resident) == -1 &&
    errno == ENOMEM;
}

static int check_blocked(lua_State *L)
{
  global_State *g = G(L);
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  LJHugeInfo hi;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_tg_flags_test_acq(dead, TGF_DEAD));
  assert(unmapped(registry, empty));
  assert(lj_arena_hugetab_lookup(registry, blocked, &hi) == 1);
  assert(hi.readers == 1 && all_free(blocked));
  assert(lj_arena_next_acq(blocked) == late_arena);
  assert(lj_arena_hugetab_reader_release(&directory_reader, NULL) ==
    LJ_ARENA_HUGE_READER_RELEASED);
  assert(lj_arena_late_get(late_arena, lj_arena_cellof(late_block)));
  for (unsigned i = 0; i < BlockSize; i++)
    assert(((unsigned char *)late_block)[i] == 0x5a);
  lj_gc2_lease_release(&body_reader);
  return 0;
}

static int check_released(lua_State *L)
{
  global_State *g = G(L);
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(unmapped(registry, blocked));
  assert(lj_arena_bm_get(late_arena->block, lj_arena_cellof(late_block)) == 0);
  assert(lj_arena_late_get(late_arena, lj_arena_cellof(late_block)) == 0);
  assert(dead->alloc.owned[LJ_ARENAK_PLAIN] == late_arena);
  assert(lj_arena_alloc_owned_count_acq(&dead->alloc, LJ_ARENAK_PLAIN) == 1);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  lua_register(L, "seed_plain", seed_plain);
  lua_register(L, "hold_late", hold_late);
  lua_register(L, "check_blocked", check_blocked);
  lua_register(L, "check_released", check_released);
  status = luaL_dostring(L,
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th, buffer = require('threading'), require('string.buffer')\n"
    "th.gcworkers(tonumber(os.getenv('POOL')) or 0)\n"
    "local parked, unpark = th.channel(1), th.channel(1)\n"
    "local sentinel = assert(th.spawn(function()\n"
    "  assert(parked:send(true, 10))\n"
    "  assert(unpark:recv(10))\n"
    "end))\n"
    "assert(parked:recv(10))\n"
    "local bytes = ('keep'):rep(200)\n"
    "local worker = assert(th.spawn(function()\n"
    "  local retained = buffer.new():put(bytes)\n"
    "  seed_plain()\n"
    "  return retained\n"
    "end))\n"
    "local ok, retained = worker:join(10)\n"
    "assert(ok, retained)\n"
    "hold_late()\n"
    "for _ = 1, 3 do collectgarbage('collect') end\n"
    "check_blocked()\n"
    "for _ = 1, 3 do collectgarbage('collect') end\n"
    "check_released()\n"
    "local joined, again = worker:join(10)\n"
    "assert(joined and again == retained and tostring(retained) == bytes)\n"
    "assert(retained:get() == bytes)\n"
    "assert(unpark:send(true, 10))\n"
    "assert(sentinel:join(10))\n"
    "th.gcworkers(0)\n"
    "retained:free()\n"
    "collectgarbage('restart')\n"
    "collectgarbage('collect')\n");
  if (status != 0)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == 0);
  lua_close(L);
  puts("dead plain arena trim passed");
  return 0;
}
