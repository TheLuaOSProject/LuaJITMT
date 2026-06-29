#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifdef LUA_GCGENERATIONAL
#error LUA_GCGENERATIONAL is not part of the stock LuaJIT public C API.
#endif

#ifdef LUA_GCINCREMENTAL
#error LUA_GCINCREMENTAL is not part of the stock LuaJIT public C API.
#endif

typedef struct ReaderState {
  const char *chunk;
  int done;
} ReaderState;

typedef struct WriterState {
  size_t bytes;
} WriterState;

static int one(lua_State *L)
{
  lua_pushinteger(L, 1);
  return 1;
}

static const luaL_Reg stock_funcs[] = {
  { "one", one },
  { NULL, NULL }
};

static const char *chunk_reader(lua_State *L, void *ud, size_t *sz)
{
  ReaderState *rs = (ReaderState *)ud;
  (void)L;
  if (rs->done) {
    *sz = 0;
    return NULL;
  }
  rs->done = 1;
  *sz = strlen(rs->chunk);
  return rs->chunk;
}

static int chunk_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
  WriterState *ws = (WriterState *)ud;
  (void)L;
  (void)p;
  ws->bytes += sz;
  return 0;
}

static int typerror_probe(lua_State *L)
{
  return luaL_typerror(L, 1, "stock-api-surface");
}

static void check_stack_api(lua_State *L)
{
  lua_pushliteral(L, "abcd");
  assert(lua_strlen(L, -1) == 4);
  assert(lua_objlen(L, -1) == 4);
  lua_pop(L, 1);

  lua_pushinteger(L, 7);
  assert(luaL_checkint(L, -1) == 7);
  assert(luaL_checklong(L, -1) == 7);
  lua_pop(L, 1);

  lua_pushnil(L);
  assert(luaL_optint(L, -1, 11) == 11);
  assert(luaL_optlong(L, -1, 13) == 13);
  lua_pop(L, 1);

  lua_pushinteger(L, 1);
  lua_pushinteger(L, 2);
  assert(lua_lessthan(L, -2, -1));
  assert(!lua_equal(L, -2, -1));
  lua_pop(L, 2);
}

static void check_module_api(lua_State *L)
{
  luaL_register(L, "stock_api_surface", stock_funcs);
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "one");
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 1);
  lua_pop(L, 2);

  lua_newtable(L);
  luaL_openlib(L, NULL, stock_funcs, 0);
  lua_getfield(L, -1, "one");
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 1);
  lua_pop(L, 2);
}

static void check_buffer_api(lua_State *L)
{
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_putchar(&b, 'x');
  luaL_addchar(&b, 'y');
  luaL_pushresult(&b);
  assert(strcmp(lua_tostring(L, -1), "xy") == 0);
  lua_pop(L, 1);
}

static void check_chunk_api(lua_State *L)
{
  ReaderState rs = { "return 42", 0 };
  WriterState ws = { 0 };
  lua_Chunkreader reader = chunk_reader;
  lua_Chunkwriter writer = chunk_writer;

  assert(lua_load(L, reader, &rs, "stock-api-reader") == 0);
  assert(lua_dump(L, writer, &ws) == 0);
  assert(ws.bytes > 0);
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
}

static void check_gc_api(lua_State *L)
{
  const char *opts[] = { "stats", "workers", "generational", "incremental" };
  size_t i;
  assert(lua_gc(L, 10, 0) == -1);
  assert(lua_gc(L, 11, 0) == -1);
  for (i = 0; i < sizeof(opts)/sizeof(opts[0]); i++) {
    lua_getglobal(L, "collectgarbage");
    lua_pushstring(L, opts[i]);
    assert(lua_pcall(L, 1, 1, 0) != 0);
    lua_pop(L, 1);
  }
}

static void check_registry_surface(lua_State *L)
{
  lua_getregistry(L);
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "_REQUIRE_INPROGRESS");
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, -1, "_LOADLIB_INPROGRESS");
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
}

int main(void)
{
  lua_State *L = lua_open();
  int status;
  const char *msg;

  assert(L != NULL);
  luaL_openlibs(L);

  lua_getregistry(L);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);
  assert(lua_getgccount(L) >= 0);

  check_registry_surface(L);
  check_stack_api(L);
  check_module_api(L);
  check_buffer_api(L);
  check_chunk_api(L);
  check_gc_api(L);

  status = lua_cpcall(L, typerror_probe, NULL);
  assert(status != 0);
  msg = lua_tostring(L, -1);
  assert(msg != NULL && strstr(msg, "stock-api-surface expected") != NULL);
  lua_pop(L, 1);

  lua_close(L);
  return 0;
}
