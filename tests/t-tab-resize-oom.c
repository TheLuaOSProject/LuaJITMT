/* Refuse each built-in arena allocation during a public shared table grow. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"

enum { ARRAY_LEN = 24, MAX_ALLOC = 16 };
typedef struct ResizeAlloc {
  void *ptr;
  size_t size;
} ResizeAlloc;

static _Thread_local struct {
  ResizeAlloc live[MAX_ALLOC];
  unsigned seen, fail_at;
  int armed, refused;
} trace;

void *__real_lj_arena_allocf(void *ud, void *p, size_t osize, size_t nsize);
void *__wrap_lj_arena_allocf(void *ud, void *p, size_t osize, size_t nsize)
{
  unsigned i;
  void *result;
  if (trace.armed && !p && nsize && !trace.refused) {
    i = trace.seen++;
    assert(i < MAX_ALLOC);
    if (trace.seen == trace.fail_at) {
      trace.refused = 1;
      return NULL;
    }
    result = __real_lj_arena_allocf(ud, p, osize, nsize);
    assert(result != NULL);
    trace.live[i].ptr = result;
    trace.live[i].size = nsize;
    return result;
  }
  if (trace.armed && p && !nsize) {
    for (i = 0; i < trace.seen; i++) {
      if (trace.live[i].ptr == p) {
        assert(trace.live[i].size == osize);
        trace.live[i].ptr = NULL;
      }
    }
  }
  return __real_lj_arena_allocf(ud, p, osize, nsize);
}

static int append(lua_State *L)
{
  int key = (int)lua_tointeger(L, 2);
  lua_pushinteger(L, 777);
  trace.armed = 1;
  lua_rawseti(L, 1, key);
  trace.armed = 0;
  return 0;
}

static void value(lua_State *L, int key, int expect)
{
  lua_rawgeti(L, 1, key);
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == expect);
  lua_pop(L, 1);
}

static unsigned attempt(lua_State *L, int shrink, unsigned fail_at,
                        unsigned optional, MSize *mask)
{
  GCtab *t;
  Node *node;
  TValue *array;
  unsigned i;
  int status, key = shrink ? -3 : ARRAY_LEN + 1;
  lua_settop(L, 0);
  lua_createtable(L, ARRAY_LEN, 2);
  t = tabV(L->top-1);
  for (i = 1; i <= ARRAY_LEN; i++) {
    if (shrink && i > 3 && i < ARRAY_LEN)
      continue;
    lua_pushinteger(L, (int)i * 3);
    lua_rawseti(L, 1, (int)i);
  }
  lua_pushinteger(L, 41);
  lua_rawseti(L, 1, -1);
  lua_pushinteger(L, 42);
  lua_rawseti(L, 1, -2);
  node = lj_tab_node_acq(t);
  array = lj_tab_array_acq(t);
  lua_pushcfunction(L, append);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, key);
  memset(&trace, 0, sizeof(trace));
  trace.fail_at = fail_at;
  status = lua_pcall(L, 2, 0, 0);
  trace.armed = 0;
  if (fail_at && fail_at != optional) {
    assert(trace.refused && status == LUA_ERRMEM);
    lua_pop(L, 1);
    for (i = 0; i < trace.seen; i++)
      assert(trace.live[i].ptr == NULL);
    assert(lj_tab_node_acq(t) == node && lj_tab_array_acq(t) == array);
    assert(lj_tab_struct_control_owner(lj_tab_struct_control_acq(t)) == 0);
    assert(lj_tab_resize_desc_control_acq(t) == NULL);
    lua_rawgeti(L, 1, key);
    assert(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_pushinteger(L, 777);
    lua_rawseti(L, 1, key);
  } else {
    assert(!status && trace.seen >= 5);
    if (fail_at)
      assert(trace.refused && lj_tab_hmask_acq(t) > *mask);
    else
      *mask = lj_tab_hmask_acq(t);
  }
  assert(lj_tab_array_acq(t) != array && lj_tab_node_acq(t) != node);
  for (i = 1; i <= ARRAY_LEN; i++) {
    if (!shrink || i <= 3 || i == ARRAY_LEN)
      value(L, (int)i, (int)i * 3);
  }
  value(L, -1, 41);
  value(L, -2, 42);
  value(L, key, 777);
  return trace.seen;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  unsigned count, i;
  MSize mask;
  assert(L);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "local t=require('threading'); "
    "local w=assert(t.spawn(function() return true end)); assert(w:join(10))") == 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(mt_active_acq(G(L)));
  count = attempt(L, 0, 0, 0, &mask);
  assert(count == 5);
  for (i = 1; i <= count; i++)
    (void)attempt(L, 0, i, 0, &mask);
  count = attempt(L, 1, 0, 0, &mask);
  assert(count == 6);  /* The last allocation offers a more compact hash. */
  for (i = 1; i <= count; i++)
    (void)attempt(L, 1, i, count, &mask);
  lua_close(L);
  puts("t-tab-resize-oom OK");
  return 0;
}
