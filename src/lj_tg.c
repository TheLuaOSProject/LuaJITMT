/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_tg_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_tg.h"

void lj_tg_init(GG_State *GG, int alloc_ready)
{
  TGState *tg = &GG->main_tg;
  global_State *g = &GG->g;
  lua_State *L = &GG->L;
  g->main_tg = tg;
  L->tg_hint = tg;
  tg->gl = g;
  tg->cur_L = L;
  tg->thread_L = L;
  tg->prng = g->prng;
  if (!alloc_ready)
    lj_arena_alloc_init(&tg->alloc);
  else
    tg->tg_flags |= TGF_ARENA_INTERNAL;
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  lj_buf_init(NULL, &tg->tmpbuf);
#if LJ_HASJIT
  memcpy(tg->hotcount, GG->hotcount, sizeof(tg->hotcount));
#endif
  memcpy(tg->dispatch, GG->dispatch, sizeof(tg->dispatch));
}

void lj_tg_fini(global_State *g)
{
  if (g->main_tg) {
    lj_buf_free(g, &g->main_tg->tmpbuf);
    lj_arena_alloc_fini(&g->main_tg->alloc);
  }
}

void lj_tg_sync_dispatch(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    memcpy(tg->dispatch, G2GG(g)->dispatch, sizeof(tg->dispatch));
}
