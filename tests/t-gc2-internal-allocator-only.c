/* Temporary GC2 internal-arena-only lua_Alloc policy regression. */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static unsigned callback_calls;

static void *poison_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  (void)ptr;
  (void)osize;
  (void)nsize;
  callback_calls++;
  return NULL;
}

int main(void)
{
  int marker;
  void *actual_ud = NULL, *after_ud = NULL;
  lua_Alloc actual, after;
  lua_State *L = lua_newstate(poison_alloc, &marker);

  assert(L != NULL);
  assert(callback_calls == 0);
  actual = lua_getallocf(L, &actual_ud);
  assert(actual != NULL && actual != poison_alloc);
  assert(actual_ud != &marker);

  lua_setallocf(L, poison_alloc, &marker);
  after = lua_getallocf(L, &after_ud);
  assert(after == actual);
  assert(after_ud == actual_ud);
  assert(callback_calls == 0);

  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "local keep = {}; for r = 1, 8 do "
    "for i = 1, 2000 do keep[(i % 97) + 1] = {r, i, tostring(i)} end "
    "collectgarbage('collect') end "
    "for i = 1, #keep do assert(keep[i][2] > 0) end") == 0);
  lua_close(L);
  assert(callback_calls == 0);

  puts("t-gc2-internal-allocator-only OK: lua_Alloc callbacks stayed disabled");
  return 0;
}
