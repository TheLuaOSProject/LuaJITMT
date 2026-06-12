/*
** Focused guard for M5 string table CAS publication scaffolding.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_str.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  StrTabHdr *hdr;
  MSize oldmask, wantmask;
  GCstr *s1, *s2;
  int i;

  assert(L != NULL);
  g = G(L);
  hdr = g->str.tabh;
  assert(hdr != NULL);
  assert(hdr->resize == 0);

  s1 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  s2 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  assert(s1 == s2);

  oldmask = g->str.mask;
  wantmask = (oldmask << 1) + 1u;
  la_store32_rel(&hdr->resize, 1);  /* Simulate an active pinned interner. */
  lj_str_resize(L, wantmask);
  assert(g->str.tabh == hdr);
  assert(g->str.mask == oldmask);
  assert(hdr->resize == 1);
  la_store32_rel(&hdr->resize, 0);

  lj_str_resize(L, wantmask);
  assert(g->str.tabh != hdr);
  assert(g->str.mask == wantmask);
  assert(g->str.tabh->resize == 0);
  assert(lj_str_new(L, "m5-strtab-cas-same",
		    strlen("m5-strtab-cas-same")) == s1);

  for (i = 0; i < 8192; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-cas-%d-%d", i, i * 31);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }
  assert(g->str.tabh->resize == 0);

  lua_close(L);
  printf("t-strtab-cas OK: active resize gate and duplicate intern guard verified\n");
  return 0;
}
