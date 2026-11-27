/* Exercise the public allocator API with arena-backed Lua storage. */
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

typedef union Block {
  size_t size;
  max_align_t alignment;
} Block;

typedef struct Counting {
  atomic_size_t attempts, live, frees;
  atomic_int pause;
  size_t fail_at;
} Counting;

enum {
  PAUSE_DISABLED,
  PAUSE_REQUESTED,
  PAUSE_ACTIVE
};

typedef struct Wrapper {
  lua_Alloc next;
  void *next_ud;
  size_t allocations, frees;
} Wrapper;

static void *counting_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
  Counting *count = ud;
  Block *block = p ? (Block *)p - 1 : NULL;
  const size_t attempt = nsize ? ++count->attempts : 0;
  assert((p == NULL) == (osize == 0));
  assert(!block || block->size == osize);
  if (!nsize) {
    assert(block && count->live >= osize);
    count->live -= osize;
    count->frees++;
    free(block);
    return NULL;
  }
  if (nsize > osize && count->fail_at && attempt >= count->fail_at)
    return NULL;
  if (nsize >= 1000000) {
    int expected = PAUSE_REQUESTED;
    if (atomic_compare_exchange_strong(&count->pause, &expected, PAUSE_ACTIVE))
      while (count->pause == PAUSE_ACTIVE)
	sched_yield();
  }
  block = realloc(block, sizeof(*block) + nsize);
  if (!block) {
    assert(nsize > osize);
    return NULL;
  }
  block->size = nsize;
  count->live += nsize;
  count->live -= osize;
  if (nsize > osize)
    memset((char *)(block + 1) + osize, 0xa7, nsize - osize);
  return block + 1;
}

static void *wrapping_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
  Wrapper *wrapper = ud;
  if (nsize)
    wrapper->allocations++;
  else
    wrapper->frees++;
  return wrapper->next(wrapper->next_ud, p, osize, nsize);
}

static int push_userdata(lua_State *L)
{
  size_t size = (size_t)lua_tointeger(L, 1);
  void *p = lua_newuserdata(L, size);
  memset(p, 'a', size);
  return 1;
}

static int protected_userdata(lua_State *L, size_t size)
{
  lua_pushcfunction(L, push_userdata);
  lua_pushinteger(L, (lua_Integer)size);
  return lua_pcall(L, 1, 1, 0);
}

static void collect(lua_State *L)
{
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

static void check_allocf(lua_State *L, lua_Alloc expected, void *expected_ud)
{
  void *ud = NULL;
  assert(lua_getallocf(L, &ud) == expected && ud == expected_ud);
  assert(lua_getallocf(L, NULL) == expected);
}

static void test_bootstrap_failure(void)
{
  Counting initial = {0};
  size_t attempt, initial_allocations;
  lua_State *L = lua_newstate(counting_alloc, &initial);
  assert(L && initial.attempts > 1 && initial.live);
  check_allocf(L, counting_alloc, &initial);
  initial_allocations = initial.attempts;
  lua_close(L);
  assert(initial.live == 0);
  for (attempt = 1; attempt <= initial_allocations; attempt++) {
    Counting failure = {0};
    failure.fail_at = attempt;
    assert(lua_newstate(counting_alloc, &failure) == NULL);
    assert(failure.attempts >= attempt && failure.live == 0);
  }
}

static void test_custom_allocator(void)
{
  Counting count = {0};
  lua_State *L = lua_newstate(counting_alloc, &count);
  lua_State *co;
  size_t attempts, frees;
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(protected_userdata(L, 200000) == LUA_OK);
  co = lua_newthread(L);
  check_allocf(co, counting_alloc, &count);
  {
    Wrapper wrapper = { counting_alloc, &count, 0, 0 };
    lua_setallocf(co, wrapping_alloc, &wrapper);
    check_allocf(L, wrapping_alloc, &wrapper);
    lua_pop(L, 2);  /* Includes a huge userdata allocated before the wrapper. */
    collect(L);
    assert(wrapper.frees > 0);
    assert(protected_userdata(L, 250000) == LUA_OK);
    assert(wrapper.allocations > 0);
    lua_setallocf(L, counting_alloc, &count);
    check_allocf(L, counting_alloc, &count);
  }  /* Wrapper context expires while its huge allocation is still live. */
  frees = count.frees;
  lua_pop(L, 1);
  collect(L);
  assert(count.frees > frees);

  attempts = count.attempts;
  count.fail_at = attempts + 1;
  assert(protected_userdata(L, 400000) == LUA_ERRMEM);
  assert(count.attempts > attempts);
  lua_pop(L, 1);
  count.fail_at = 0;
  assert(protected_userdata(L, 400000) == LUA_OK);
  lua_close(L);
  assert(count.live == 0);
}

static void test_wrapping_builtin(void)
{
  lua_State *L = luaL_newstate();
  void *ud;
  lua_Alloc builtin;
  assert(L);
  builtin = lua_getallocf(L, &ud);
  assert(builtin);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(protected_userdata(L, 180000) == LUA_OK);
  {
    Wrapper wrapper = { builtin, ud, 0, 0 };
    lua_setallocf(L, wrapping_alloc, &wrapper);
    check_allocf(L, wrapping_alloc, &wrapper);
    lua_pop(L, 1);
    collect(L);
    assert(wrapper.frees == 0);  /* The old mapping is native. */
    assert(protected_userdata(L, 180000) == LUA_OK);
    assert(wrapper.allocations > 0);
    lua_pop(L, 1);
    collect(L);
    assert(wrapper.frees > 0);
    assert(protected_userdata(L, 180000) == LUA_OK);
    lua_setallocf(L, builtin, ud);
    check_allocf(L, builtin, ud);
  }
  lua_pop(L, 1);
  lua_close(L);

  {
    Wrapper wrapper = { builtin, ud, 0, 0 };
    L = lua_newstate(wrapping_alloc, &wrapper);
    assert(L && wrapper.allocations > 1);
    lua_setallocf(L, builtin, ud);
    check_allocf(L, builtin, ud);
  }  /* Even the main state's backing was acquired through the wrapper. */
  assert(protected_userdata(L, 180000) == LUA_OK);
  lua_close(L);
}

static int wait_for_callback(lua_State *L)
{
  Counting *count = lua_touserdata(L, lua_upvalueindex(1));
  while (count->pause != PAUSE_ACTIVE)
    sched_yield();
  return 0;
}

static int resume_callback(lua_State *L)
{
  Counting *count = lua_touserdata(L, lua_upvalueindex(1));
  count->pause = PAUSE_DISABLED;
  return 0;
}

static void test_parallel_callbacks(void)
{
  Counting count = {0};
  lua_State *L = lua_newstate(counting_alloc, &count);
  int status;
  assert(L);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushlightuserdata(L, &count);
  lua_pushcclosure(L, wait_for_callback, 1);
  lua_setglobal(L, "wait_for_callback");
  lua_pushlightuserdata(L, &count);
  lua_pushcclosure(L, resume_callback, 1);
  lua_setglobal(L, "resume_callback");
  count.pause = PAUSE_REQUESTED;
  status = luaL_dostring(L,
    "local threading = require('threading')\n"
    "local first = threading.spawn(function() return string.rep('a', 1100000) end)\n"
    "wait_for_callback()\n"
    "local second = threading.spawn(function() return string.rep('b', 200000) end)\n"
    "local ok, b = second:join()\n"
    "assert(ok and #b == 200000 and b:sub(100000, 100001) == 'bb')\n"
    "resume_callback()\n"
    "local ok, a = first:join()\n"
    "assert(ok and #a == 1100000 and a:sub(1000000, 1000001) == 'aa')\n");
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK && count.pause == PAUSE_DISABLED);
  lua_close(L);
  assert(count.live == 0);
}

int main(void)
{
  test_bootstrap_failure();
  test_custom_allocator();
  test_wrapping_builtin();
  test_parallel_callbacks();
  puts("t-allocator-embedding OK");
  return 0;
}
