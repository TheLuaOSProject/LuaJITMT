#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct Identity {
  GCobj *object;
  uint32_t id, kind, seen;
} Identity;
typedef struct Payload { uint64_t id, inverse; } Payload;
static global_State *probe_g;
static uint32_t armed, eof_calls, prune_calls;
static uint64_t eof_ns, eof_flushed;
static Identity *identities;
static size_t slots;

static size_t pointer_slot(GCobj *o)
{
  uintptr_t x = (uintptr_t)o >> 4;
  x ^= x >> 17;
  x *= UINT64_C(0x9e3779b97f4a7c15);
  return (size_t)x & (slots - 1);
}
static Identity *identity(GCobj *o)
{
  size_t p = pointer_slot(o);
  while (identities[p].object && identities[p].object != o)
    p = (p + 1) & (slots - 1);
  return &identities[p];
}
static int graph_ssb_empty(global_State *g, TGState *tg)
{
  return gc2_ssb_head_acq(g) == NULL && gc2_ssb_drain_acq(g) == NULL &&
         gc2_ssb_consumer_active_acq(g) == 0 &&
         gc2_ssb_items_published_acq(g) == gc2_ssb_items_drained_acq(g) &&
         lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg);
}
static void enter_real_sweep(lua_State *L)
{
  global_State *g = G(L);
  unsigned i;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  for (i = 0; i < 10000 && gc2_phase_acq(g) == LJ_GC2_MARK; i++) {
    (void)lj_gc2_worker_drain(g, 64);
    if (lj_gc2_mark_complete(g, L, 1, 64))
      lj_gc2_mark_to_weak(g);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  for (i = 0; i < 10000 && gc2_phase_acq(g) == LJ_GC2_WEAK; i++) {
    if (lj_gc2_weak_complete(g, L, NULL, 64))
      lj_gc2_weak_to_sweep(g, L);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_sweep_root_scanned_acq(g) == 0);
  assert(gc2_sweep_root_done_acq(g) == 0);
  assert(gc2_sweep_bridge_ready_acq(g) == 0);
}

extern uint32_t __real_lj_gc_sweep_gc2_unmarked(global_State *);
uint32_t __wrap_lj_gc_sweep_gc2_unmarked(global_State *g)
{
  uint64_t before, start, ns, delta;
  uint32_t n, cycle;
  GCRef *cursor0;
  if (g != probe_g || !armed)
    return __real_lj_gc_sweep_gc2_unmarked(g);
  assert(gc2_worker_active_acq(g) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  cycle = gc2_cycle_acq(g);
  cursor0 = gc2_sweep_root_cursor_acq(g);
  before = gc2_pending_root_flushed_acq(g);
  start = lj_thr_now_ns();
  n = __real_lj_gc_sweep_gc2_unmarked(g);
  ns = lj_thr_now_ns() - start;
  delta = gc2_pending_root_flushed_acq(g) - before;
  assert(gc2_worker_active_acq(g) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_cycle_acq(g) == cycle);
  prune_calls++;
  if (delta || gc2_sweep_root_done_acq(g)) {
    eof_calls++;
    eof_ns = ns;
    eof_flushed = delta;
  }
  printf("{\"stage\":\"real_prune_unit\",\"cycle\":%u,\"ns\":%" PRIu64
         ",\"unlinked\":%u,\"pending_flushed\":%" PRIu64
         ",\"root_done\":%u,\"cursor_changed\":%u}\n",
         cycle, ns, n, delta, gc2_sweep_root_done_acq(g),
         cursor0 != gc2_sweep_root_cursor_acq(g));
  return n;
}

static uint32_t check_chain(GCobj *head, uint32_t cap, int after_main,
                            uint32_t *found, uint32_t *found_after_main)
{
  uint32_t n = 0;
  GCobj *o;
  for (o = head; o; o = lj_obj_gcw_acq(o)) {
    Identity *id;
    assert(++n <= cap);
    if (o == obj2gco(mainthread_acq(probe_g))) after_main = 1;
    id = identity(o);
    if (id->object) {
      assert(id->seen == 0);
      id->seen = 1;
      if (id->kind) {
        Payload *p = (Payload *)uddata(gco2ud(o));
        assert(after_main);
        assert(p->id == id->id && p->inverse == ~p->id);
        (*found_after_main)++;
      } else {
        cTValue *v = lj_tab_getint(gco2tab(o), 1);
        assert(v && tvisnumber(v) && numberVnum(v) == (lua_Number)id->id);
      }
      (*found)++;
    }
  }
  return n;
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  uint32_t count, mode, i, cycle, pending_n, pending_after_n;
  uint32_t found = 0, found_after = 0, root_n, expected_after = 0;
  uint64_t flushed0, completed;
  GCobj *pending, *pending_after;
  assert(argc == 3);
  count = (uint32_t)strtoul(argv[1], NULL, 10);
  mode = (uint32_t)strtoul(argv[2], NULL, 10);
  assert(count <= 262144 && mode <= 2);
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(45);
  slots = 1;
  while (slots < (size_t)count * 2 + 16) slots <<= 1;
  identities = (Identity *)calloc(slots, sizeof(*identities));
  assert(identities);
  L = luaL_newstate(); assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_createtable(L, 1024, 0);
  for (i = 1; i <= 1024; i++) {
    lua_createtable(L, 1, 0);
    lua_pushinteger(L, i); lua_rawseti(L, -2, 1);
    lua_rawseti(L, 1, (int)i);
  }
  lua_createtable(L, 32, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L); tg = L2TG(L); probe_g = g;
  enter_real_sweep(L);
  cycle = gc2_cycle_acq(g);
  completed = gc2_sweep_to_idle_acq(g);
  lj_gc2_sweep_prepare_bridge_boundary(g, NULL);
  assert(gc2_sweep_root_scanned_acq(g) == 1);
  assert(gc2_sweep_root_done_acq(g) == 0);
  assert(gc2_sweep_bridge_ready_acq(g) == 0);
  assert(lj_gc2_sweep_needs_prepare(g) == 0);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  for (i = 1; i <= count; i++) {
    uint32_t kind = mode == 1 || (mode == 2 && (i & 1));
    GCobj *o;
    Identity *id;
    if (kind) {
      Payload *p = (Payload *)lua_newuserdata(L, sizeof(Payload));
      p->id = i; p->inverse = ~(uint64_t)i;
      o = obj2gco(udataV(L->top - 1));
      expected_after++;
    } else {
      lua_createtable(L, 1, 0);
      lua_pushinteger(L, i); lua_rawseti(L, -2, 1);
      o = obj2gco(tabV(L->top - 1));
    }
    id = identity(o);
    assert(id->object == NULL);
    id->object = o; id->id = i; id->kind = kind;
    lua_rawseti(L, 2, (int)((i - 1) % 32 + 1));
  }
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_cycle_acq(g) == cycle);
  /* Drain actual allocation/barrier recovery before asking the root boundary
  ** to admit. It deliberately refuses live recovery; this setup must not
  ** bypass that gate or mistake graph work for an EOF cost. No pending-root
  ** flush is permitted during this graph-only setup. */
  flushed0 = gc2_pending_root_flushed_acq(g);
  for (i = 0; i < (count + 2048u) / 8u; i++) {
    if (gc2_recovery_items_acq(g) == 0 && graph_ssb_empty(g, tg) &&
        gc2_grey_top_acq(g) == gc2_grey_bottom_acq(g) &&
        gc2_table_rescan_pending_acq(g) == 0)
      break;
    (void)lj_gc2_worker_drain(g, 64);
  }
  assert(gc2_recovery_items_acq(g) == 0 && graph_ssb_empty(g, tg));
  assert(gc2_grey_top_acq(g) == gc2_grey_bottom_acq(g));
  assert(gc2_table_rescan_pending_acq(g) == 0);
  assert(gc2_pending_root_flushed_acq(g) == flushed0);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_cycle_acq(g) == cycle);
  pending = lj_tg_gcroot_pending_acq(tg);
  pending_after = lj_tg_gcroot_pending_after_main_acq(tg);
  pending_n = check_chain(pending, count + 1, 0, &found, &found_after);
  pending_after_n = check_chain(pending_after, count + 1, 1, &found, &found_after);
  assert(pending_n + pending_after_n == count);
  assert(found == count && found_after == expected_after);
  for (i = 0; i < slots; i++) identities[i].seen = 0;
  found = found_after = 0;
  flushed0 = gc2_pending_root_flushed_acq(g);
  printf("{\"stage\":\"long_chain_ready\",\"count\":%u,\"mode\":%u,"
         "\"ordinary\":%u,\"after_main\":%u,\"cycle\":%u,"
         "\"root_scanned\":%u,\"root_done\":%u,\"ready\":%u,"
         "\"needscan\":%u,\"workers\":%u}\n",
         count, mode, pending_n, pending_after_n, cycle,
         gc2_sweep_root_scanned_acq(g), gc2_sweep_root_done_acq(g),
         gc2_sweep_bridge_ready_acq(g), gc2_thread_scan_needscan_pending_acq(g),
         gc2_n_workers_acq(g));
  armed = 1;
  for (i = 0; i < 64 && eof_calls == 0; i++)
    lj_gc2_sweep_prepare_bridge_boundary(g, NULL);
  armed = 0;
  assert(eof_calls == 1);
  assert(eof_flushed == count);
  assert(gc2_pending_root_flushed_acq(g) - flushed0 == count);
  assert(lj_tg_gcroot_pending_acq(tg) == NULL);
  assert(lj_tg_gcroot_pending_after_main_acq(tg) == NULL);
  if (count) {
    assert(gc2_sweep_root_done_acq(g) == 0);
    assert(gc2_sweep_bridge_ready_acq(g) == 0);
    assert(gc2_sweep_root_cursor_acq(g) == lj_gc_root_ref(g));
  }
  root_n = check_chain(lj_gc_root_acq(g), count + 2048, 0, &found, &found_after);
  assert(found == count && found_after == expected_after);
  printf("{\"stage\":\"verified_eof_unit\",\"count\":%u,\"mode\":%u,"
         "\"eof_ns\":%" PRIu64 ",\"flushed\":%" PRIu64
         ",\"prune_calls\":%u,\"root_nodes\":%u,"
         "\"exact_identities\":%u,\"after_main_identities\":%u}\n",
         count, mode, eof_ns, eof_flushed, prune_calls, root_n, found, found_after);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_sweep_to_idle_acq(g) > completed);
  for (i = 1; i <= 1024; i++) {
    lua_rawgeti(L, 1, (int)i); assert(lua_istable(L, -1));
    lua_rawgeti(L, -1, 1); assert(lua_tointeger(L, -1) == (lua_Integer)i);
    lua_pop(L, 2);
  }
  if (count >= 32) {
    for (i = 1; i <= 32; i++) {
      uint32_t value = count - 32 + i;
      uint32_t kind = mode == 1 || (mode == 2 && (value & 1));
      lua_rawgeti(L, 2, (int)i);
      if (kind) {
        Payload *p = (Payload *)lua_touserdata(L, -1);
        assert(p && p->id == value && p->inverse == ~(uint64_t)value);
      } else {
        assert(lua_istable(L, -1));
        lua_rawgeti(L, -1, 1); assert(lua_tointeger(L, -1) == (lua_Integer)value);
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  lua_close(L);
  free(identities);
  puts("PASS exact pending chain and graph survived real EOF flush and completed GC");
  return 0;
}
