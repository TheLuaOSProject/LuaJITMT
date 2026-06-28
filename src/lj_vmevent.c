/*
** VM event handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdio.h>

#define lj_vmevent_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_dispatch.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev)
{
  global_State *g = G(L);
  GCstr *s = lj_str_newlit(L, LJ_VMEVENTS_REGKEY);
  cTValue *tv = lj_tab_getstr(tabV(registry(L)), s);
  TValue tabv;
  if (tv) {
    lj_tv_load_acq(&tabv, tv);
    if (tvistab(&tabv)) {
      int hash = VMEVENT_HASH(ev);
      TValue fnv;
      tv = lj_tab_getint(tabV(&tabv), hash);
      if (tv)
	lj_tv_load_acq(&fnv, tv);
      if (tv && tvisfunc(&fnv)) {
	lj_state_checkstack(L, LUA_MINSTACK);
	setfuncV(L, L->top++, funcV(&fnv));
	if (LJ_FR2) setnilV(L->top++);
	return savestack(L, L->top);
      }
    }
  }
  g->vmevmask &= ~VMEVENT_MASK(ev);  /* No handler: cache this fact. */
  return 0;
}

static uint32_t vmevent_report_failure(lua_State *L)
{
  uint32_t actions;
  lj_native_enter(L2TG(L));
  fputs("VM handler failed: ", stderr);
  fputs(tvisstr(L->top) ? strVdata(L->top) : "?", stderr);
  fputc('\n', stderr);
  actions = lj_native_leave(L);
  return actions;
}

static int vmevent_had_stopreq(lua_State *L)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int vmevent_fresh_stopreq(lua_State *L, uint32_t actions,
				 int had_stopreq)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

static void vmevent_checkstop_fresh(lua_State *L, uint32_t actions,
				    int had_stopreq)
{
  if (vmevent_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

void lj_vmevent_call(lua_State *L, ptrdiff_t argbase)
{
  global_State *g = G(L);
  lua_State *oldL = lj_tg_cur_L(g);
  uint8_t oldmask = g->vmevmask;
  uint8_t oldh = hook_save(g);
  uint32_t actions = 0;
  int had_stopreq = 0;
  int status;
  g->vmevmask = 0;  /* Disable all events. */
  hook_vmevent(g);
  status = lj_vm_pcall(L, restorestack(L, argbase), 0+1, 0);
  if (LJ_UNLIKELY(status)) {
    /* Really shouldn't use stderr here, but where else to complain? */
    L->top--;
    had_stopreq = vmevent_had_stopreq(L);
    actions = vmevent_report_failure(L);
  }
  lj_tg_setcur_L(g, oldL);
#if LJ_HASJIT
  G2J(g)->L = oldL;
#endif
  hook_restore(g, oldh);
  if (g->vmevmask != VMEVENT_NOCACHE)
    g->vmevmask = oldmask;  /* Restore event mask, but not if not modified. */
  if (LJ_UNLIKELY(status))
    vmevent_checkstop_fresh(L, actions, had_stopreq);
}
