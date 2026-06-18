/*
** M8 close-time finalizer drain coverage.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int cdata_finalized;
static int udata_finalized;
static int alternating_cdata_finalized;
static int alternating_udata_finalized;
static int order_cdata_finalized[3];
static int order_cdata_count;
static int growth_triggered;
static int growth_cdata_finalized;

static int close_udata_finalizer(lua_State *L);
static int close_alternating_udata_finalizer(lua_State *L);
static void push_close_udata(lua_State *L);
static void push_alternating_udata(lua_State *L);

#define ALTERNATING_CLOSE_CHAIN 6
#define CLOSE_FINREG_GROWTH_BATCH 96

static int close_cdata_finalizer(lua_State *L)
{
  cdata_finalized++;
  if (cdata_finalized == 1) {
    push_close_udata(L);
    lua_getglobal(L, "m8_close_chain_cdata");
    assert(lua_isfunction(L, -1));
    assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  }
  return 0;
}

static int close_order_cdata_finalizer(lua_State *L, int id)
{
  (void)L;
  assert(order_cdata_count < 3);
  order_cdata_finalized[order_cdata_count++] = id;
  return 0;
}

static int close_order_cdata_finalizer_1(lua_State *L)
{
  return close_order_cdata_finalizer(L, 1);
}

static int close_order_cdata_finalizer_2(lua_State *L)
{
  return close_order_cdata_finalizer(L, 2);
}

static int close_order_cdata_finalizer_3(lua_State *L)
{
  return close_order_cdata_finalizer(L, 3);
}

static int close_growth_trigger_finalizer(lua_State *L)
{
  growth_triggered++;
  assert(growth_triggered == 1);
  lua_getglobal(L, "m8_close_register_growth_cdata");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, CLOSE_FINREG_GROWTH_BATCH);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
  return 0;
}

static int close_growth_cdata_finalizer(lua_State *L)
{
  (void)L;
  growth_cdata_finalized++;
  return 0;
}

static int close_udata_finalizer(lua_State *L)
{
  udata_finalized++;
  if (udata_finalized == 1) {
    lua_newuserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, close_udata_finalizer);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
  }
  return 0;
}

static int close_alternating_cdata_finalizer(lua_State *L)
{
  alternating_cdata_finalized++;
  if (alternating_cdata_finalized <= ALTERNATING_CLOSE_CHAIN)
    push_alternating_udata(L);
  return 0;
}

static int close_alternating_udata_finalizer(lua_State *L)
{
  alternating_udata_finalized++;
  if (alternating_udata_finalized < ALTERNATING_CLOSE_CHAIN) {
    lua_getglobal(L, "m8_close_chain_alternating_cdata");
    assert(lua_isfunction(L, -1));
    assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  }
  return 0;
}

static void push_close_udata(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, close_udata_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

static void push_alternating_udata(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, close_alternating_udata_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  lua_pushcfunction(L, close_cdata_finalizer);
  lua_setglobal(L, "m8_close_cdata_finalizer");
  lua_pushcfunction(L, close_alternating_cdata_finalizer);
  lua_setglobal(L, "m8_close_alternating_cdata_finalizer");
  lua_pushcfunction(L, close_order_cdata_finalizer_1);
  lua_setglobal(L, "m8_close_order_cdata_finalizer_1");
  lua_pushcfunction(L, close_order_cdata_finalizer_2);
  lua_setglobal(L, "m8_close_order_cdata_finalizer_2");
  lua_pushcfunction(L, close_order_cdata_finalizer_3);
  lua_setglobal(L, "m8_close_order_cdata_finalizer_3");
  lua_pushcfunction(L, close_growth_trigger_finalizer);
  lua_setglobal(L, "m8_close_growth_trigger_finalizer");
  lua_pushcfunction(L, close_growth_cdata_finalizer);
  lua_setglobal(L, "m8_close_growth_cdata_finalizer");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m8_close_fin_t;\n"
    "typedef struct { int x; } lj_m8_close_alt_fin_t;\n"
    "typedef struct { int x; } lj_m8_close_order_fin_t;\n"
    "typedef struct { int x; } lj_m8_close_growth_trigger_fin_t;\n"
    "typedef struct { int x; } lj_m8_close_growth_fin_t;\n"
    "]]\n"
    "local keep = {}\n"
    "function m8_close_chain_cdata()\n"
    "  ffi.gc(ffi.new('lj_m8_close_fin_t'), m8_close_cdata_finalizer)\n"
    "end\n"
    "function m8_close_chain_alternating_cdata()\n"
    "  ffi.gc(ffi.new('lj_m8_close_alt_fin_t'), m8_close_alternating_cdata_finalizer)\n"
    "end\n"
    "function m8_close_register_growth_cdata(n)\n"
    "  for i = 1, n do\n"
    "    ffi.gc(ffi.new('lj_m8_close_growth_fin_t'), m8_close_growth_cdata_finalizer)\n"
    "  end\n"
    "end\n"
    "for i = 1, 3 do\n"
    "  keep[i] = ffi.gc(ffi.new('lj_m8_close_fin_t'), m8_close_cdata_finalizer)\n"
    "end\n"
    "keep.order1 = ffi.gc(ffi.new('lj_m8_close_order_fin_t'), m8_close_order_cdata_finalizer_1)\n"
    "keep.order2 = ffi.gc(ffi.new('lj_m8_close_order_fin_t'), m8_close_order_cdata_finalizer_2)\n"
    "keep.order3 = ffi.gc(ffi.new('lj_m8_close_order_fin_t'), m8_close_order_cdata_finalizer_3)\n"
    "keep.growth = ffi.gc(ffi.new('lj_m8_close_growth_trigger_fin_t'), m8_close_growth_trigger_finalizer)\n"
    "m8_close_chain_alternating_cdata()\n") == LUA_OK);
  push_close_udata(L);

  lua_close(L);
  assert(cdata_finalized == 4);
  assert(udata_finalized == 3);
  assert(alternating_cdata_finalized == ALTERNATING_CLOSE_CHAIN);
  assert(alternating_udata_finalized == ALTERNATING_CLOSE_CHAIN);
  assert(order_cdata_count == 3);
  assert(order_cdata_finalized[0] == 3);
  assert(order_cdata_finalized[1] == 2);
  assert(order_cdata_finalized[2] == 1);
  assert(growth_triggered == 1);
  assert(growth_cdata_finalized == CLOSE_FINREG_GROWTH_BATCH);
  printf("t-m8-close-finalizers OK: lua_close drains alternating cdata/userdata finalizers to fixed point\n");
  return 0;
}
