/* Required arena directories must be available before a Lua thread can run. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_thr.h"

static _Thread_local unsigned refuse_after;

int __real_lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits);
int __wrap_lj_arena_hugetab_init(HugeTab *ht, uint32_t hbits)
{
  if (refuse_after && --refuse_after == 0)
    return 0;
  return __real_lj_arena_hugetab_init(ht, hbits);
}

static void *count_blocks(void *ud, void *p, size_t osize, size_t nsize)
{
  size_t *live = (size_t *)ud;
  if (nsize == 0) {
    assert(p && *live >= osize);
    *live -= osize;
    free(p);
    return NULL;
  } else {
    void *q = realloc(p, nsize);
    if (!q)
      return nsize <= osize ? p : NULL;
    *live = *live - osize + nsize;
    return q;
  }
}

static void fail_newstate(void)
{
  size_t live = 0;
  unsigned index;
  TGState *prior = lj_thr_get_tg();
  for (index = 1; index <= 2; index++) {
    refuse_after = index;  /* Per-thread huge or global small-arena directory. */
    assert(lua_newstate(count_blocks, &live) == NULL);
    assert(refuse_after == 0 && live == 0);
    assert(lj_thr_get_tg() == prior);
  }
}

static void *foreign_worker(void *arg)
{
  lua_State *co = (lua_State *)arg;
  assert(!lj_thr_get_tg());
  refuse_after = 1;
  assert(!lj_threading_attach(co));
  assert(refuse_after == 0 && !lj_thr_get_tg());
  assert(lj_state_owner_acq(co) == 0 && !mt_entering_acq(G(co)));
  assert(lj_threading_attach(co));
  lua_pushinteger(co, 42);
  lj_threading_detach(co, 1);
  assert(!lj_thr_get_tg());
  return NULL;
}

int main(void)
{
  lua_State *L, *co;
  pthread_t thread;
  fail_newstate();
  L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  fail_newstate();  /* An existing state's TLS binding must survive failure. */
  assert(luaL_dostring(L,
    "threading = require('threading')\n"
    "function answer() return 42 end") == LUA_OK);
  lua_getglobal(L, "threading");
  lua_getfield(L, -1, "spawn");
  lua_getglobal(L, "answer");
  refuse_after = 1;
  assert(lua_pcall(L, 1, 1, 0) == LUA_ERRMEM);
  assert(refuse_after == 0 && la_load32_acq(&G(L)->threading_live_count) == 0);
  lua_pop(L, 2);
  assert(luaL_dostring(L,
    "local ok, result = threading.spawn(answer):join()\n"
    "assert(ok and result == 42)") == LUA_OK);

  co = lua_newthread(L);
  assert(pthread_create(&thread, NULL, foreign_worker, co) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(lua_tointeger(co, -1) == 42);
  lua_pop(L, 1);

  /* Start one real collector before the second directory fails. The first
  ** collector must be joined and release its slot before gcworkers returns. */
  refuse_after = 2;
  assert(luaL_dostring(L, "threading.gcworkers(2)") != LUA_OK);
  assert(refuse_after == 0);
  assert(strstr(lua_tostring(L, -1), "cannot start GC worker"));
  lua_pop(L, 1);
  assert(luaL_dostring(L,
    "assert(threading.gcworkers() == 0)\n"
    "assert(threading.gcworkers(2) == 0)\n"
    "collectgarbage()\n"
    "assert(threading.gcworkers(0) == 2)") == LUA_OK);
  lua_close(L);
  assert(!lj_thr_get_tg());
  puts("arena directory allocation failures OK");
  return 0;
}
