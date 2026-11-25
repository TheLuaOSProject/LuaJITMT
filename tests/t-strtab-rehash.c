/*
** Full-input string hashes avoid generations caused by sparse collisions.
*/
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"

#define TARGET_MASK 4095u
#define TARGET_STRINGS 64u

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  char candidate[TARGET_STRINGS][128];
  const char *interned[TARGET_STRINGS];
  StrHash first_hash = 0;
  StrTabHdr *hdr;
  unsigned i, different = 0;

  assert(L);
  g = G(L);
  lua_gc(L, LUA_GCSTOP, 0);
  lj_str_resize(L, TARGET_MASK);
  hdr = lj_str_tabh_acq(g);
  assert(hdr && hdr->mask == TARGET_MASK);
  lua_createtable(L, TARGET_STRINGS, 0);

  for (i = 0; i < TARGET_STRINGS; i++) {
    StrHash hash;
    memset(candidate[i], 'H', sizeof(candidate[i]));
    /* These bytes are outside the legacy sparse input samples. */
    candidate[i][12] = (char)i;
    candidate[i][13] = (char)(i ^ 0x3f);
    lua_pushlstring(L, candidate[i], sizeof(candidate[i]));
    interned[i] = lua_tolstring(L, -1, NULL);
    hash = strV(L->top - 1)->hash;
    if (i == 0)
      first_hash = hash;
    different += hash != first_hash;
    lua_rawseti(L, 1, (int)i + 1);
  }

  if (LUAJIT_SECURITY_STRHASH)
    assert(different != 0);
  assert(lj_str_tabh_acq(g) == hdr && lj_str_resize_state_acq(hdr) == 0);
  for (i = 0; i < TARGET_STRINGS; i++) {
    lua_pushlstring(L, candidate[i], sizeof(candidate[i]));
    assert(lua_tolstring(L, -1, NULL) == interned[i]);
    assert(memcmp(interned[i], candidate[i], sizeof(candidate[i])) == 0);
    lua_pop(L, 1);
  }

  lua_close(L);
  puts("t-strtab-rehash OK: full-input hashes preserve canonical identity");
  return 0;
}
