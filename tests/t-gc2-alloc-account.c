/*
** Focused test for GC2 allocation accounting bridge.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  void *p;
  uint64_t total;
  uint64_t epoch0;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(g != NULL);
  assert(tg != NULL);

  (void)lj_gc2_flush_alloc(g, tg);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);

  p = lj_mem_realloc(L, NULL, 0, 128);
  assert(p != NULL);
  assert(la_load64_acq(&tg->local_total) == 128);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  assert(lj_gc2_flush_alloc(g, tg) == 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);

  lj_mem_free(g, p, 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);

  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&tg->local_total) == LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);
  lj_gc2_account_alloc(g, tg, 1);
  assert(la_load64_acq(&tg->local_total) == 0);
  total = la_load64_acq(&g->gc2.alloc_since_trigger);
  assert(total == 128 + LJ_GC2_ACCT_FLUSH);

  lj_gc2_account_alloc(g, tg, 7);
  assert(la_load64_acq(&tg->local_total) == 7);
  epoch0 = la_load64_acq(&g->gc2.hs_epoch);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH) == 1);
  assert(la_load64_acq(&g->gc2.hs_epoch) == epoch0 + 1u);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == total + 7);

  lua_close(L);
  puts("t-gc2-alloc-account OK: allocation accounting flushes by threshold and safepoint");
  return 0;
}
