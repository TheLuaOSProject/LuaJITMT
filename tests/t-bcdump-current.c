/*
** Focused regression test for lockless bytecode dump/load behavior.
**
** This fixture intentionally treats bytecode as an opaque public artifact:
** it exercises dump/load round trips and observable closure behavior, but does
** not parse bytes, patch opcodes, or assert exact dump spelling.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/lua_fixture_helpers.h"

typedef struct DumpBuf {
  char *p;
  size_t n;
  size_t cap;
} DumpBuf;

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
  DumpBuf *b = (DumpBuf *)ud;
  (void)L;
  if (sz > b->cap - b->n) {
    size_t cap = b->cap ? b->cap : 256;
    char *np;
    while (sz > cap - b->n)
      cap *= 2;
    np = (char *)realloc(b->p, cap);
    assert(np != NULL);
    b->p = np;
    b->cap = cap;
  }
  memcpy(b->p + b->n, p, sz);
  b->n += sz;
  return 0;
}

static void dump_reset(DumpBuf *b)
{
  b->n = 0;
}

static void dump_free(DumpBuf *b)
{
  free(b->p);
  b->p = NULL;
  b->n = b->cap = 0;
}

static int load_dump(lua_State *L, const DumpBuf *b)
{
  assert(b->n > 0);
  return luaL_loadbufferx(L, b->p, b->n, "=(bcdump-compat)", "b");
}

static void dump_stack_function(lua_State *L, DumpBuf *b, const char *what)
{
  dump_reset(b);
  assert(lua_isfunction(L, -1));
  assert(lua_dump(L, dump_writer, b) == 0);
  assert(b->n > 0);
  ljt_lua_assert_ok(L, load_dump(L, b), what);
}

static void assert_chunk_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };

  ljt_lua_assert_ok(L, luaL_loadstring(L, "return 42"), "load simple chunk");
  dump_stack_function(L, &dump, "load dumped simple chunk");
  lua_remove(L, -2);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "run dumped simple chunk");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_free(&dump);
}

static void assert_closure_cell_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "local x = 0\n"
    "return function()\n"
    "  x = x + 1\n"
    "  return x\n"
    "end";

  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "load cell closure chunk");
  dump_stack_function(L, &dump, "load dumped cell closure chunk");
  lua_remove(L, -2);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "create dumped cell closure");

  lua_pushvalue(L, -1);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "run dumped cell closure #1");
  assert(lua_tointeger(L, -1) == 1);
  lua_pop(L, 1);
  lua_pushvalue(L, -1);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "run dumped cell closure #2");
  assert(lua_tointeger(L, -1) == 2);
  lua_pop(L, 2);

  dump_free(&dump);
}

static void assert_self_capture_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "local keep\n"
    "for i = 1, 2 do\n"
    "  local function f() return f end\n"
    "  keep = f\n"
    "end\n"
    "return keep";

  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "load self capture chunk");
  dump_stack_function(L, &dump, "load dumped self capture chunk");
  lua_remove(L, -2);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0),
		    "create dumped self capture closure");

  lua_pushvalue(L, -1);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "run self capture closure");
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);

  dump_free(&dump);
}

static void assert_nested_child_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "return function()\n"
    "  local x = 7\n"
    "  return function() return x end\n"
    "end";

  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "load nested child chunk");
  dump_stack_function(L, &dump, "load dumped nested child chunk");
  lua_remove(L, -2);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "create dumped child closure");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "create dumped nested child");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "run dumped child closure");
  assert(lua_tointeger(L, -1) == 7);
  lua_pop(L, 1);

  dump_free(&dump);
}

static void assert_jit_patched_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "return function(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end";
  int i;

  ljt_lua_assert_ok(L, luaL_dostring(L,
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1')"),
    "enable hot JIT");
  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "load traced roundtrip function");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "create traced roundtrip function");
  for (i = 0; i < 40; i++) {
    lua_pushvalue(L, -1);
    lua_pushinteger(L, 100);
    ljt_lua_assert_ok(L, lua_pcall(L, 1, 1, 0), "run traced roundtrip function");
    assert(lua_tointeger(L, -1) == 5050);
    lua_pop(L, 1);
  }

  dump_stack_function(L, &dump, "load dumped JIT function");
  lua_remove(L, -2);
  lua_pushinteger(L, 100);
  ljt_lua_assert_ok(L, lua_pcall(L, 1, 1, 0), "run dumped JIT function");
  assert(lua_tointeger(L, -1) == 5050);
  lua_pop(L, 1);

  dump_free(&dump);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  assert_chunk_roundtrip(L);
  assert_closure_cell_roundtrip(L);
  assert_self_capture_roundtrip(L);
  assert_nested_child_roundtrip(L);
  assert_jit_patched_roundtrip(L);

  lua_close(L);
  printf("t-bcdump-current OK: bytecode dump/load behavior verified\n");
  return 0;
}
