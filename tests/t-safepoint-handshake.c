/*
** Focused test for the C-level soft-handshake scaffold.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  g->gc2.hs_actions = actions;
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  uint64_t epoch0;
  uint32_t actions;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert(g->gc2.hs_epoch == 0);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == 0);

  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);

  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  publish_manual(g, tg, LJ_GC2_HS_ENABLE_BARRIER);
  assert(lj_safepoint_poll(L) == LJ_GC2_HS_ENABLE_BARRIER);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 1);
  assert(lj_safepoint_poll(L) == 0);

  lj_native_enter(tg);
  assert(tg->in_native == 1);
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->in_native == 1);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(lj_native_leave(L) == 0);
  assert(tg->in_native == 0);

  lj_native_enter(tg);
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_BLACK);
  assert(lj_native_leave(L) == LJ_GC2_HS_ALLOC_BLACK);
  assert(tg->in_native == 0);
  assert(g->gc2.hs_pending == 0);
  assert(tg->alloc.alloc_black == 1);

  assert(lj_gc2_handshake(g, 0) == 0);
  lua_close(L);

  printf("t-safepoint-handshake OK: C soft handshakes verified\n");
  return 0;
}
