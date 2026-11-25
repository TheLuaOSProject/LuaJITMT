/* A refused growth allocation must still allow interning in the old table. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_str.h"

static StrTabHdr *source;
static size_t fail_size;
static int refused;
static unsigned serial;
static char bytes[64];

static void before_growth(lua_State *L, GCstr *s, StrCanonRec *r, uint32_t stage)
{
  if (stage == LJ_STR_TEST_CANON_RESIZE_ALLOC && !refused) {
    MSize mask;
    source = lj_str_tabh_acq(G(L));
    mask = (source->mask << 1) + 1u;
    fail_size = lj_str_tabsize_dense(mask, mask);
  }
  (void)s; (void)r;
}

void *__real_lj_arena_allocf(void *ud, void *p, size_t osize, size_t nsize);
void *__wrap_lj_arena_allocf(void *ud, void *p, size_t osize, size_t nsize)
{
  if (fail_size && !p && nsize) {
    assert(nsize == fail_size);
    fail_size = 0;
    refused = 1;
    return NULL;
  }
  return __real_lj_arena_allocf(ud, p, osize, nsize);
}

static void push_fresh(lua_State *L)
{
  int len = snprintf(bytes, sizeof(bytes), "directory-reservation-%08x", serial++);
  assert(len > 0 && (size_t)len < sizeof(bytes));
  lua_pushlstring(L, bytes, (size_t)len);
}

static int insert_until_refused(lua_State *L)
{
  do {
    assert(serial < 100000);
    push_fresh(L);
    if (refused) return 1;
    lua_pop(L, 1);
  } while (1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  const char *identity;
  assert(L);
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, insert_until_refused);
  lj_str_test_set_canon_hook(before_growth);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  lj_str_test_set_canon_hook(NULL);
  assert(refused && source && lj_str_tabh_acq(G(L)) == source);
  identity = lua_tostring(L, 1);
  assert(identity && !strcmp(identity, bytes));
  lua_pushstring(L, bytes);
  assert(lua_tostring(L, 2) == identity);
  lua_pop(L, 1);
  while (lj_str_tabh_acq(G(L)) == source) {
    assert(serial < 100000);
    push_fresh(L);
    lua_pop(L, 1);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushstring(L, identity);
  assert(lua_tostring(L, 2) == identity);
  lua_close(L);
  puts("t-strtab-growth-oom OK");
  return 0;
}
