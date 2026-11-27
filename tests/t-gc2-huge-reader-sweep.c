/* A native reader can outlive a full collection on the calling pthread. */
#include <assert.h>
#include <stdio.h>

#include "lj_cdata.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

enum { BODY_SIZE = 1024 * 1024 };

static void discard(lua_State *L)
{
  lua_pushnil(L);
  lua_replace(L, 1);
  lua_pop(L, 1);
}

static void check_mapping(int cdata, int keep)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  HugeTab *ht = &L2TG(L)->huge;
  LJHugeReader reader = {0};
  LJHugeInfo hi;
  GCobj *o;
  void *base;
  unsigned char *bytes;
  uint64_t deferred;
  int i;

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  if (cdata) {
    ljt_lua_dostring(L,
      "local ffi = require('ffi'); return ffi.new('uint8_t[?]', 1024*1024)");
    o = gcV(L->top-1);
    assert(cdataisv(gco2cd(o)));
    base = memcdatav(gco2cd(o));
    bytes = (unsigned char *)cdataptr(gco2cd(o));
  } else {
    bytes = (unsigned char *)lua_newuserdata(L, BODY_SIZE);
    o = gcV(L->top-1);
    base = o;
  }
  bytes[0] = 23;
  bytes[BODY_SIZE-1] = 174;
  assert(lj_arena_hugetab_reader_acquire(ht, base, &reader, &hi) ==
         LJ_ARENA_HUGE_READER_ACQUIRED);
  assert(hi.readers == 1);
  if (!keep) discard(L);

  deferred = gc2_deferred_epoch_acq(g);
  for (i = 0; i < 2; i++) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    assert(gc2_deferred_epoch_acq(g) > deferred);
    deferred = gc2_deferred_epoch_acq(g);
    assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
    assert(lj_arena_hugetab_lookup(ht, base, &hi) == 1);
    assert(hi.readers == 1 && !(hi.flags & LJ_HUGEF_TICKET));
    assert(lj_arena_huge_root_state(hi.flags) == LJ_ARENA_ROOT_MEMBER);
    assert(bytes[0] == 23 && bytes[BODY_SIZE-1] == 174);
  }
  assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
         LJ_ARENA_HUGE_READER_RELEASED);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  if (keep) {
    assert(gcV(L->top-1) == o && lj_arena_hugetab_lookup(ht, base, &hi) == 1);
    assert(bytes[0] == 23 && bytes[BODY_SIZE-1] == 174);
    discard(L);
  }
  for (i = 0; i < 4 && lj_arena_hugetab_lookup(ht, base, NULL); i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_arena_hugetab_lookup(ht, base, NULL) == 0);
  lua_close(L);
}

#ifdef LJ_GC2_TEST_HELPERS
static GCobj *changing_root;
static int claims;

static void change_predecessor(global_State *g, GCobj *o, uint32_t path)
{
  lua_State *L = mainthread_acq(g);
  LJHugeInfo hi;
  if (path != LJ_GC_ROOT_STATE_TEST_UNLINKING || o != changing_root)
    return;
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, o, &hi) == 1);
  if (claims++ == 0) {
    assert(!(hi.flags & LJ_HUGEF_TICKET) && lj_obj_gcw_acq(obj2gco(L)) == o);
    *(unsigned char *)lua_newuserdata(L, 1) = 81;
    (void)lj_gc_flush_root_pending(g);
    assert(lj_obj_gcw_acq(obj2gco(L)) == gcV(L->top-1));
    lua_setfield(L, LUA_REGISTRYINDEX, "new root");
  } else {
    /* The sweep retried the edge after its first splice lost. */
    assert(hi.flags & LJ_HUGEF_TICKET);
    lj_gc_test_set_root_state_hook(NULL);
  }
}

static void check_splice_retry(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  unsigned char *bytes;
  GCobj *o;
  int seen = 0, links = 0;
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  bytes = (unsigned char *)lua_newuserdata(L, BODY_SIZE);
  bytes[0] = 157;
  changing_root = gcV(L->top-1);
  claims = 0;
  lj_gc_test_set_root_state_hook(change_predecessor);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(claims == 2 && gc2_phase_acq(g) == LJ_GC2_IDLE);
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o; o = lj_obj_gcw_acq(o)) {
    assert(++links < 10000);
    if (o == changing_root) seen++;
  }
  assert(seen == 1 && bytes[0] == 157);
  lua_getfield(L, LUA_REGISTRYINDEX, "new root");
  assert(*(unsigned char *)lua_touserdata(L, -1) == 81);
  lua_pop(L, 1);
  discard(L);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, changing_root, NULL) == 0);
  changing_root = NULL;
  lua_close(L);
}
#endif

int main(void)
{
  int cdata, keep;
  for (cdata = 0; cdata <= 1; cdata++)
    for (keep = 0; keep <= 1; keep++)
      check_mapping(cdata, keep);
#ifdef LJ_GC2_TEST_HELPERS
  check_splice_retry();
#endif
  puts("t-gc2-huge-reader-sweep OK");
  return 0;
}
