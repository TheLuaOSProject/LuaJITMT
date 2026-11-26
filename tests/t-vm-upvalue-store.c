/* A peer redirects a Lua upvalue while a bytecode store has lost access to its
** original cell. A captured frame local instead resumes on the same cell. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_tab.h"
#include "lj_thr.h"

static GCupval *old_cell;
static uint32_t ready, start, done;

static int before_store(lua_State *L)
{
  old_cell = (GCupval *)lua_upvalueid(L, 1, 1);
  assert(old_cell && mref_acq(old_cell->v, TValue) == &old_cell->tv);
  la_store32_rel(&ready, 1);
  while (!la_load32_acq(&start))
    (void)lj_thr_retry_yield(L);
  return 0;
}

static int after_store(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&done, 1);
  return 0;
}

static int release_writer(lua_State *L)
{
  GCArena *arena;
  uint32_t cell, waits;
  uint64_t before;
  int redirect = lua_isfunction(L, 1);
  while (!la_load32_acq(&ready))
    (void)lj_thr_retry_yield(L);
  arena = lj_arena_of(old_cell);
  cell = lj_arena_cellof(old_cell);
  before = tv_rawload_acq(&old_cell->tv);
  assert(lj_arena_lifetime_state_cas(arena, cell, LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_DESTRUCT));
  waits = lj_tab_test_wait_l_calls();
  la_store32_rel(&start, 1);
  while (lj_tab_test_wait_l_calls() == waits && !la_load32_acq(&done))
    (void)lj_thr_retry_yield(L);
  assert(!la_load32_acq(&done));
  assert(tv_rawload_acq(&old_cell->tv) == before);
  if (redirect) {
    lua_upvaluejoin(L, 1, 1, 2, 1);
    while (!la_load32_acq(&done))
      (void)lj_thr_retry_yield(L);
    assert(tv_rawload_acq(&old_cell->tv) == before);
  }
  assert(lj_arena_lifetime_state_cas(arena, cell, LJ_ARENA_LIFETIME_DESTRUCT,
				     LJ_ARENA_LIFETIME_LIVE));
  return 0;
}

static int reset(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&ready, 0);
  la_store32_rel(&start, 0);
  la_store32_rel(&done, 0);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  lua_register(L, "before_store", before_store);
  lua_register(L, "after_store", after_store);
  lua_register(L, "release_writer", release_writer);
  lua_register(L, "reset_store", reset);
  if (luaL_dostring(L,
    "local th = require('threading')\n"
    "jit.off(); jit.flush(); collectgarbage('stop')\n"
    "local function cells(x)\n"
    "  return {function(v) x=v end, function() x='stored' end,\n"
    "          function() x=271.25 end, function() x=true end,\n"
    "          function() x=nil end}, function() return x end\n"
    "end\n"
    "local function write(setter, v, before, after)\n"
    "  before(setter); setter(v); after()\n"
    "end\n"
    "local values = {{123}, 'stored', 271.25, true}\n"
    "for i=1,5 do\n"
    "  local setters, old = cells(42)\n"
    "  local _, new = cells(99)\n"
    "  reset_store()\n"
    "  local w = th.spawn(write, setters[i], values[i], before_store, after_store)\n"
    "  release_writer(setters[i], new); assert(w:join(10))\n"
    "  collectgarbage(); assert(old()==42 and new()==values[i])\n"
    "end\n"
    "local function localwrite(v, before, after)\n"
    "  local x=42; local function read() return x end\n"
    "  before(read); x=v; after(); return read()\n"
    "end\n"
    "reset_store()\n"
    "local w=th.spawn(localwrite, values[1], before_store, after_store)\n"
    "release_writer(); local ok, v=w:join(10); assert(ok and v==values[1])\n")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);
  puts("t-vm-upvalue-store OK");
  return 0;
}
