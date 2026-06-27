/*
** Focused guard for M5 string table CAS publication scaffolding.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_str.h"

#define TEST_STRTAB_RESIZE	((MSize)0x80000000u)

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  StrTabHdr *hdr;
  MSize oldmask, wantmask;
  uint64_t retire_epoch;
  uint64_t smr_runs0, smr_reclaimed0;
  GCstr *s1, *s2;
  int i;

  assert(L != NULL);
  g = G(L);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert(hdr->resize == 0);

  s1 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  s2 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  assert(s1 == s2);

  oldmask = g->str.mask;
  wantmask = (oldmask << 1) + 1u;

  /* Simulate the last active interner leaving a resize-claimed header. */
  la_store32_rel(&hdr->resize, TEST_STRTAB_RESIZE | 1u);
  {
    MSize old = la_sub32_acqrel(&hdr->resize, 1);
    assert((old & TEST_STRTAB_RESIZE) != 0);
    assert((old & ~TEST_STRTAB_RESIZE) == 1u);
    assert(hdr->resize == TEST_STRTAB_RESIZE);
  }
  la_store32_rel(&hdr->resize, 0);

  lj_str_resize(L, wantmask);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(g->str.mask == wantmask);
  assert(lj_str_tabh_acq(g)->resize == 0);
  assert(lj_str_retired_head_acq(g) == hdr);
  assert(lj_str_retired_next_acq(hdr) == NULL);
  retire_epoch = gc2_hs_epoch_acq(g);
  assert(lj_str_retire_epoch_acq(hdr) == retire_epoch);
  smr_runs0 = gc2_smr_reclaim_runs_acq(g);
  smr_reclaimed0 = gc2_smr_reclaimed_acq(g);
  assert(lj_gc2_reclaim_retired(g, retire_epoch) == 0);
  assert(gc2_smr_reclaim_runs_acq(g) == smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) == smr_reclaimed0);
  assert(lj_str_retired_head_acq(g) == hdr);
  assert(lj_str_new(L, "m5-strtab-cas-same",
		    strlen("m5-strtab-cas-same")) == s1);

  for (i = 0; i < 8192; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-cas-%d-%d", i, i * 31);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }
  assert(lj_str_tabh_acq(g)->resize == 0);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  assert(gc2_hs_epoch_acq(g) > retire_epoch);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(gc2_smr_reclaim_runs_acq(g) > smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) >= smr_reclaimed0 + 1u);

  lua_close(L);
  printf("t-strtab-cas OK: active-drain resize claim, GC2 epoch retire, and duplicate intern guard verified\n");
  return 0;
}
