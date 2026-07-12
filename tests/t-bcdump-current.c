/*
** Focused regression test for lockless bytecode dump/load behavior.
**
** This fixture intentionally treats bytecode as an opaque public artifact:
** it exercises dump/load round trips and observable closure behavior, but does
** not parse bytes, patch opcodes, or compare exact dump blobs.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"

#include "lib/lua_fixture_helpers.h"

typedef struct DumpBuf {
  char *p;
  size_t n;
  size_t cap;
} DumpBuf;

typedef struct GCChunkReader {
  const DumpBuf *dump;
  size_t pos;
  unsigned calls;
} GCChunkReader;

static const char *gc_chunk_reader(lua_State *L, void *ud, size_t *sz)
{
  GCChunkReader *r = (GCChunkReader *)ud;
  size_t left = r->dump->n - r->pos;
  size_t n = left > 17u ? 17u : left;
  if (n == 0) {
    *sz = 0;
    return NULL;
  }
  /* Complete a major cycle between small reader chunks. This deliberately
  ** intersects prototype bytecode/KGC/template-table construction. */
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  *sz = n;
  r->calls++;
  r->pos += n;
  return r->dump->p + r->pos - n;
}

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

static void assert_gc_chunked_table_roundtrip(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  GCChunkReader reader;
  const char *src =
    "return { alpha='aaaaaaaaaaaaaaaa', beta='bbbbbbbbbbbbbbbb',\n"
    "  gamma='cccccccccccccccc', delta='dddddddddddddddd',\n"
    "  [17]='seventeen', [31]='thirty-one', true, false, 12345 }";

  ljt_lua_assert_ok(L, luaL_loadstring(L, src),
		    "load forced-GC table-constant chunk");
  dump_reset(&dump);
  assert(lua_dump(L, dump_writer, &dump) == 0 && dump.n > 0);
  reader.dump = &dump;
  reader.pos = 0;
  reader.calls = 0;
  ljt_lua_assert_ok(L,
		    lua_loadx(L, gc_chunk_reader, &reader,
			      "=(forced-gc-bcdump)", "b"),
		    "load forced-GC chunked bytecode");
  assert(reader.calls > 4);
  lua_remove(L, -2);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0),
		    "run forced-GC table-constant chunk");
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "alpha");
  assert(strcmp(lua_tostring(L, -1), "aaaaaaaaaaaaaaaa") == 0);
  lua_pop(L, 1);
  lua_rawgeti(L, -1, 31);
  assert(strcmp(lua_tostring(L, -1), "thirty-one") == 0);
  lua_pop(L, 2);
  dump_free(&dump);
}

static void run_mutated_load_pass(lua_State *L, const DumpBuf *dump,
				  char *copy, unsigned *errors)
{
  size_t i;
  for (i = 5; i < dump->n; i++) {
    int status;
    memcpy(copy, dump->p, dump->n);
    copy[i] ^= (char)0xa5;
    status = luaL_loadbufferx(L, copy, dump->n, "=(mutated-bcdump)", "b");
    if (status != LUA_OK)
      (*errors)++;
    lua_pop(L, 1);  /* Error object or successfully loaded function. */
    if ((i & 15u) == 0)
      (void)lua_gc(L, LUA_GCCOLLECT, 0);
  }
}

static void assert_malformed_body_cancellation(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "local x = { a='alpha', b='beta', c='gamma', [19]='nineteen' }\n"
    "return function(n) local s=0; for i=1,n do s=s+i end; return s,x end";
  char *copy;
  uint64_t total;
  unsigned errors = 0;

  ljt_lua_assert_ok(L, luaL_loadstring(L, src),
		    "load malformed-cancellation source");
  assert(lua_dump(L, dump_writer, &dump) == 0 && dump.n > 16);
  lua_pop(L, 1);
  copy = (char *)malloc(dump.n);
  assert(copy != NULL);

  /* The first pass warms error strings and any dynamically extended TG anchor
  ** blocks. The second pass must not accumulate cancelled READY=0 prototypes. */
  run_mutated_load_pass(L, &dump, copy, &errors);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  total = lj_gc_total_load(G(L));
  run_mutated_load_pass(L, &dump, copy, &errors);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(errors >= 8);
  assert(lj_gc_total_load(G(L)) <= total + 16384u);

  free(copy);
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
  assert_gc_chunked_table_roundtrip(L);
  assert_malformed_body_cancellation(L);
  assert_jit_patched_roundtrip(L);

  lua_close(L);
  printf("t-bcdump-current OK: bytecode dump/load behavior verified\n");
  return 0;
}
