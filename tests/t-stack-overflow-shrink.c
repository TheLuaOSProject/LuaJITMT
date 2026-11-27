/* Shrinking the emergency Lua stack must succeed during protected unwind,
** including when another reader still holds the complete allocation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_tg.h"

static int with_reader;
static void *emergency_stack;
static size_t emergency_bytes;
static LJHugeReader reader;

static int overflow_handler(lua_State *L)
{
  const char *message = lua_tostring(L, 1);
  LJHugeInfo hi;
  assert(message && strstr(message, "stack overflow"));
  assert(emergency_stack == NULL);
  emergency_stack = tvref(L->stack);
  emergency_bytes = (size_t)L->stacksize * sizeof(TValue);
  assert(L->stacksize > LUAI_MAXSTACK + 1 + LJ_STACK_EXTRA);
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, emergency_stack, &hi));
  assert(hi.size == emergency_bytes);
  if (with_reader)
    assert(lj_arena_hugetab_reader_acquire(&L2TG(L)->huge, emergency_stack,
                                           &reader, NULL) == 1);
  lj_arena_test_alloc_fail(1);
  return 1;
}

static void check_unwind(void)
{
  lua_State *L = luaL_newstate();
  size_t normal_bytes;
  assert(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, overflow_handler);
  assert(luaL_loadstring(L,
    "local function f() local a,b,c,d,e,g,h,i,j,k,l,m,n,o,p,q "
    "f() return a end f()") == 0);
  emergency_stack = NULL;
  assert(lua_pcall(L, 0, 0, 1) == LUA_ERRRUN);
  assert(lj_arena_test_alloc_failures() == 0);
  lj_arena_test_alloc_fail(0);
  assert(lua_gettop(L) == 2 && strstr(lua_tostring(L, -1), "stack overflow"));
  assert(tvref(L->stack) == emergency_stack);
  assert(L->stacksize == LUAI_MAXSTACK + 1 + LJ_STACK_EXTRA);
  normal_bytes = (size_t)L->stacksize * sizeof(TValue);
  assert(lj_arena_huge_mapsize(normal_bytes) <
         lj_arena_huge_mapsize(emergency_bytes));
  if (with_reader) {
    assert(lj_arena_hugetab_reader_covers_range(&reader, emergency_stack,
                                                emergency_bytes));
    assert(lj_arena_hugetab_reader_release(&reader, NULL) ==
           LJ_ARENA_HUGE_READER_RELEASED);
  }
  lua_settop(L, 0);
  lua_gc(L, LUA_GCRESTART, 0);
  assert(luaL_dostring(L, "return 6 * 7") == 0);
  assert(lua_tointeger(L, -1) == 42);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
}

int main(void)
{
  for (with_reader = 0; with_reader <= 1; with_reader++)
    check_unwind();
  puts("protected stack overflow recovered with allocation refusal");
  return 0;
}
