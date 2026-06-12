/*
** Focused guard for M5 string table representation prep.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"

LJ_STATIC_ASSERT((offsetof(StrTabHdr, bucket) & (sizeof(GCRef)-1u)) == 0);

static void check_strtab(global_State *g)
{
  GCRef *bucket;
  MSize i;

  assert(g->str.tabh != NULL);
  assert(g->str.mask != ~(MSize)0);
  assert(g->str.tabh->mask == g->str.mask);
  assert(g->str.tabh->resize == 0);
  assert(g->str.tabh->copy_cursor == 0);
  assert(((uintptr_t)lj_str_buckets(g) & (sizeof(GCRef)-1u)) == 0);
  assert(offsetof(StrTabHdr, bucket) < lj_str_tabsize(g->str.mask));

  bucket = lj_str_buckets(g);
  for (i = 0; i <= g->str.mask; i++) {
    uintptr_t u = gcrefu(bucket[i]);
    GCobj *head = lj_str_hashhead(bucket[i]);
    assert((u & LJ_STRHASH_DEAD) == 0);
    assert((u & LJ_STRHASH_LINKMASK) == lj_str_hashflags(bucket[i]));
    assert((u & LJ_STRHASH_SECONDARY) == lj_str_hashsecondary(bucket[i]));
    if (head)
      assert(((uintptr_t)head & LJ_STRHASH_LINKMASK) == 0);
  }
}

int main(void)
{
  lua_State *L;
  global_State *g;
  MSize initial_mask;
  int i;

  assert(LJ_STRHASH_DEAD == (uintptr_t)1);
  assert(LJ_STRHASH_SECONDARY == (uintptr_t)2);
  assert(LJ_STRHASH_LINKMASK == (uintptr_t)3);

  L = luaL_newstate();
  assert(L != NULL);
  g = G(L);
  check_strtab(g);
  initial_mask = g->str.mask;

  for (i = 0; i < 4096; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-prep-%d-%d", i, i * 17);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }

  assert(g->str.mask > initial_mask);
  check_strtab(g);
  lua_gc(L, LUA_GCCOLLECT, 0);
  check_strtab(g);

  lua_close(L);
  printf("t-strtab-prep OK: StrTabHdr and marker bits are consistent\n");
  return 0;
}
