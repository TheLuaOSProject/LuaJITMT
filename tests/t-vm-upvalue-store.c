/* A peer redirects a Lua upvalue while a bytecode store has lost access to its
** original cell. A captured frame local instead resumes on the same cell. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static int join_upvalues(lua_State *L)
{
  lua_upvaluejoin(L, 1, luaL_checkint(L, 2), 3, luaL_checkint(L, 4));
  return 0;
}

static int get_upvalue(lua_State *L)
{
  int top = lua_gettop(L);
  const char *name = lua_getupvalue(L, 1, 1);
  assert(name && !strcmp(name, "x") && lua_gettop(L) == top + 1);
  return 1;
}

static int set_upvalue(lua_State *L)
{
  int top = lua_gettop(L);
  const char *name = lua_setupvalue(L, 1, 1);
  assert(name && !strcmp(name, "x") && lua_gettop(L) == top - 1);
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

static void check_c_closure(lua_State *L)
{
  int top;
  const char *name;
  ptrdiff_t capacity;
  lua_pushinteger(L, 23);
  lua_pushcclosure(L, after_store, 1);
  while (L->top + 1 < tvref(L->maxstack))
    lua_pushnil(L);
  top = lua_gettop(L);
  capacity = tvref(L->maxstack) - tvref(L->stack);
  assert(lua_getupvalue(L, -top, 2) == NULL && lua_gettop(L) == top);
  assert(lua_setupvalue(L, -top, 2) == NULL && lua_gettop(L) == top);
  assert(tvref(L->maxstack) - tvref(L->stack) == capacity);
  name = lua_getupvalue(L, -top, 1);
  assert(name && !*name && lua_tointeger(L, -1) == 23);
  assert(tvref(L->maxstack) - tvref(L->stack) > capacity);
  lua_settop(L, 1);
  lua_pushvalue(L, 1);
  name = lua_setupvalue(L, -1, 1);
  assert(name && !*name && lua_gettop(L) == 1);
  name = lua_getupvalue(L, -1, 1);
  assert(name && !*name && lua_rawequal(L, 1, -1));
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  check_c_closure(L);
  lua_register(L, "before_store", before_store);
  lua_register(L, "after_store", after_store);
  lua_register(L, "release_writer", release_writer);
  lua_register(L, "reset_store", reset);
  lua_register(L, "join_upvalues", join_upvalues);
  lua_register(L, "get_upvalue", get_upvalue);
  lua_register(L, "set_upvalue", set_upvalue);
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
    "local function join(target,donor,before,after,op)\n"
    "  before(donor); op(target,1,donor,1); after()\n"
    "end\n"
    "for _,op in ipairs{join_upvalues,debug.upvaluejoin} do\n"
    "  local ds,old=cells(55); local donor=ds[1]\n"
    "  local _,target=cells(0); local _,replacement=cells(71)\n"
    "  reset_store()\n"
    "  local w=th.spawn(join,target,donor,before_store,after_store,op)\n"
    "  release_writer(donor,replacement); assert(w:join(10))\n"
    "  collectgarbage(); assert(target()==71 and old()==55)\n"
    "end\n"
    "local function access(f,v,before,after,op)\n"
    "  before(f); local result=op(f,v); after(); return result\n"
    "end\n"
    "local function get(f) return select(2,debug.getupvalue(f,1)) end\n"
    "local function set(f,v) assert(debug.setupvalue(f,1,v)=='x') end\n"
    "for i,op in ipairs{get_upvalue,get,set_upvalue,set} do\n"
    "  local fs,old=cells(42); local f=fs[1]\n"
    "  local _,new=cells(values[1]); local v={}\n"
    "  reset_store()\n"
    "  local w=th.spawn(access,f,v,before_store,after_store,op)\n"
    "  release_writer(f,new); local ok,result=w:join(10); assert(ok)\n"
    "  collectgarbage(); assert(old()==42)\n"
    "  if i<=2 then assert(result==values[1]) else assert(new()==v) end\n"
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
