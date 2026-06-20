/*
** Focused guard for JIT trace body/exittab SMR retirement.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_jit.h"
#include "lj_trace.h"

static GCtrace *retired_find(jit_State *J, GCtrace *needle)
{
  GCtrace *T;
  for (T = J->retiredtraces; T != NULL; T = trace_retired_next_acq(T))
    if (T == needle)
      return T;
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  GCproto *pt;
  GCtrace tmpl, *T;
  static IRIns dummyir[REF_TRUE+1];
  MCode **exittab;
  GCtrace *ret;
  uint64_t epoch, scoped_epoch;

  assert(L != NULL);
  g = G(L);
  J = G2J(g);
  assert(J->retiredtraces == NULL);
  assert(luaL_loadstring(L, "return 1") == 0);
  pt = funcproto(funcV(L->top - 1));

  memset(&tmpl, 0, sizeof(tmpl));
  tmpl.nk = REF_TRUE;
  tmpl.nins = REF_TRUE;
  tmpl.nsnap = 1;
  tmpl.nsnapmap = 0;
  tmpl.ir = dummyir;

  T = lj_trace_alloc(L, &tmpl);
  assert(gcref(T->startpt) == NULL);
  lj_trace_free_unpublished(g, T);
  assert(J->retiredtraces == NULL);

  T = lj_trace_alloc(L, &tmpl);
  setgcref(T->startpt, obj2gco(pt));
  exittab = lj_mem_newvec(L, 1, MCode *);
  exittab[0] = NULL;
  T->exittab = exittab;
  T->nsnap = 1;
  assert(retired_find(J, T) == NULL);

  lj_trace_free(g, T);
  ret = retired_find(J, T);
  assert(ret != NULL);
  assert(ret == T);
  assert(ret->exittab == exittab);

  epoch = ret->retire_epoch;
  assert(epoch == g->gc2.hs_epoch);
  assert(lj_trace_reclaim_retired(g, epoch) == 0);
  assert(retired_find(J, T) != NULL);
  assert(lj_trace_reclaim_retired(g, epoch + 1u) == 0);
  assert(retired_find(J, T) != NULL);
  assert(lj_trace_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  T = lj_trace_alloc(L, &tmpl);
  setgcref(T->startpt, obj2gco(pt));
  exittab = lj_mem_newvec(L, 1, MCode *);
  exittab[0] = NULL;
  T->exittab = exittab;
  T->nsnap = 1;
  scoped_epoch = la_load64_acq(&g->gc2.hs_epoch) + 1u;
  la_store64_rel(&T->retire_epoch, scoped_epoch);
  la_store64_rel(&g->gc2.hs_epoch, scoped_epoch + 1u);

  lj_trace_free(g, T);
  ret = retired_find(J, T);
  assert(ret != NULL);
  assert(ret == T);
  assert(ret->retire_epoch == scoped_epoch);
  assert(lj_trace_reclaim_retired(g, scoped_epoch + LJ_FLUSH_EPOCHS - 1u) == 0);
  assert(retired_find(J, T) != NULL);
  assert(lj_trace_reclaim_retired(g, scoped_epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(retired_find(J, T) == NULL);

  lua_close(L);
  printf("t-jit-trace-retire OK: trace bodies, scratch bodies, and exittabs retire correctly\n");
  return 0;
}
