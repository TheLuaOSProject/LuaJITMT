/*
** Focused regression test for lua_atpanic() exchange semantics.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"

static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, nsize);
}

static int panic_one(lua_State *L)
{
  (void)L;
  return 0;
}

static int panic_two(lua_State *L)
{
  (void)L;
  return 0;
}

int main(void)
{
  lua_State *L = lua_newstate(test_alloc, NULL);
  lua_CFunction old;

  assert(L != NULL);

  old = lua_atpanic(L, panic_one);
  assert(old == NULL);

  old = lua_atpanic(L, panic_two);
  assert(old == panic_one);

  old = lua_atpanic(L, NULL);
  assert(old == panic_two);

  old = lua_atpanic(L, panic_one);
  assert(old == NULL);

  old = lua_atpanic(L, panic_two);
  assert(old == panic_one);

  lua_close(L);
  printf("t-panic-callback-atomic OK\n");
  return 0;
}
