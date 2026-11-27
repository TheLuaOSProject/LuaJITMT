/* Full GC from a C finalizer, including stack growth and native readers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

enum Mode { ORDINARY, TABLE_READER, HUGE_READER, REFUSE_ROOT };
static enum Mode mode;
static lua_State *caller;
static int outer_calls, inner_calls, in_outer, refuse, refused;

int __real_lj_tg_root_anchor_reserve_nothrow(lua_State *L, TGState *tg);
int __wrap_lj_tg_root_anchor_reserve_nothrow(lua_State *L, TGState *tg)
{
  if (refuse && L == caller && lj_gc2_finalizer_owned_by_current(G(L))) {
    refuse = 0;
    refused++;
    return 0;
  }
  return __real_lj_tg_root_anchor_reserve_nothrow(L, tg);
}

static void install_finalizer(lua_State *L, lua_CFunction fn)
{
  lua_newtable(L);
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

static int inner(lua_State *L)
{
  assert(L == caller && lua_gettop(L) == 1);
  assert(*(unsigned char *)lua_touserdata(L, 1) == 79);
  assert(in_outer == (mode != REFUSE_ROOT));
  inner_calls++;
  return 0;
}

static int outer(lua_State *L)
{
  unsigned char *body = (unsigned char *)lua_touserdata(L, 1);
  size_t i, size = lua_objlen(L, 1);
  LJGC2Lease lease = {0};
  int pinned = mode == TABLE_READER || mode == HUGE_READER;
  assert(L == caller && lua_gettop(L) == 1 && body && size > 0);
  assert(!outer_calls++ && !in_outer);
  assert(la_load32_acq(&G(L)->gc2.finalizer_rooted_depth) ==
         (uint32_t)(mode != REFUSE_ROOT));
  in_outer = 1;
  /* Overwrite the only Lua argument. C still uses its raw payload below. */
  lua_pushnil(L);
  lua_replace(L, 1);
  lua_settop(L, 0);
  assert(lua_checkstack(L, 4096));
  if (pinned) {
    lua_getfield(L, LUA_REGISTRYINDEX, "reader");
    assert(lj_gc2_obj_lease_acquire(G(L), gcV(L->top-1), 0, NULL, &lease) >= 0);
    assert(mode == HUGE_READER ? lease.huge.base != NULL : lease.arena != NULL);
  }
  *(unsigned char *)lua_newuserdata(L, 1) = 79;
  install_finalizer(L, inner);
  lua_gc(L, LUA_GCCOLLECT, 0);
  if (pinned) {
    lj_gc2_lease_release(&lease);
    lua_pop(L, 1);
    lua_gc(L, LUA_GCCOLLECT, 0);
  }
  assert(inner_calls == (mode != REFUSE_ROOT));
  for (i = 0; i < 4; i++) {
    memset(lua_newuserdata(L, size), 0xa9, size);
    lua_pop(L, 1);
    lua_gc(L, LUA_GCCOLLECT, 0);
  }
  for (i = 0; i < size; i++)
    assert(body[i] == (unsigned char)(i * 17u + 5u));
  in_outer = 0;
  return 0;
}

static void check(enum Mode current, int coroutine, size_t size)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg = L2TG(L);
  uint32_t top = lj_tg_root_anchor_top_acq(tg);
  unsigned char *body;
  size_t i;
  mode = current;
  outer_calls = inner_calls = in_outer = refuse = refused = 0;
  caller = coroutine ? lua_newthread(L) : L;
  lua_gc(caller, LUA_GCSTOP, 0);
  if (mode == TABLE_READER || mode == HUGE_READER) {
    if (mode == TABLE_READER) lua_newtable(caller);
    else (void)lua_newuserdata(caller, 1024 * 1024);
    lua_setfield(caller, LUA_REGISTRYINDEX, "reader");
  }
  body = (unsigned char *)lua_newuserdata(caller, size);
  for (i = 0; i < size; i++) body[i] = (unsigned char)(i * 17u + 5u);
  install_finalizer(caller, outer);
  refuse = mode == REFUSE_ROOT;
  lua_gc(caller, LUA_GCCOLLECT, 0);
  assert(outer_calls == 1 && !in_outer && refused == (mode == REFUSE_ROOT));
  assert(la_load32_acq(&G(L)->gc2.finalizer_rooted_depth) == 0);
  assert(lj_tg_root_anchor_top_acq(tg) == top);
  for (i = 0; i < 4; i++) lua_gc(caller, LUA_GCCOLLECT, 0);
  assert(outer_calls == 1 && inner_calls == 1 && lua_gettop(caller) == 0);
  lua_close(L);
  assert(outer_calls == 1 && inner_calls == 1);
}

int main(void)
{
  int m, co;
  for (m = ORDINARY; m <= REFUSE_ROOT; m++)
    for (co = 0; co <= 1; co++) {
      check((enum Mode)m, co, 128);
      check((enum Mode)m, co, 256 * 1024);
    }
  puts("t-gc-finalizer-major OK");
  return 0;
}
