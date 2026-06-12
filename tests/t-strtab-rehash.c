/*
** Focused guard for M5 secondary string-table rehash gating.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"

#define TARGET_MASK		4095u
#define TARGET_BUCKET		0u
#define TARGET_STRINGS		40u

static StrHash test_hash_sparse(uint64_t seed, const char *str, MSize len)
{
  StrHash a, b, h = len ^ (StrHash)seed;
  if (len >= 4) {
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  }
  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);
  UNUSED(a);
  return h;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  char candidate[TARGET_STRINGS][64];
  GCstr *interned[TARGET_STRINGS];
  uint32_t found = 0, i;
  uint32_t n;

  assert(L != NULL);
  g = G(L);
  lj_str_resize(L, TARGET_MASK);
  assert(g->str.tabh != NULL);
  assert(g->str.mask == TARGET_MASK);

  for (n = 0; found < TARGET_STRINGS; n++) {
    char buf[64];
    MSize len;
    snprintf(buf, sizeof(buf), "m5-strtab-rehash-%08x", n);
    len = (MSize)strlen(buf);
    if ((test_hash_sparse(g->str.seed, buf, len) & TARGET_MASK) ==
	TARGET_BUCKET) {
      memcpy(candidate[found], buf, len + 1u);
      found++;
    }
  }

  for (i = 0; i < TARGET_STRINGS; i++) {
    interned[i] = lj_str_new(L, candidate[i], strlen(candidate[i]));
    assert(interned[i] != NULL);
  }

  assert(g->str.second == 1);
  assert(g->str.tabh->resize == 0);
  assert(lj_str_hashsecondary(g->str.tabh->bucket[TARGET_BUCKET]) != 0);
  for (i = 0; i < TARGET_STRINGS; i++)
    assert(lj_str_new(L, candidate[i], strlen(candidate[i])) == interned[i]);

  lua_close(L);
  printf("t-strtab-rehash OK: secondary chain rehash is gated and preserves identity\n");
  return 0;
}
