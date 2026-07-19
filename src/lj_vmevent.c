/*
** VM event handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lj_vmevent_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

int lj_jit_event_attachment_clock_slot(int32_t registry_key, uint32_t *slot)
{
  if (!slot)
    return 0;
  *slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  switch ((uint32_t)registry_key) {
  case (uint32_t)VMEVENT_HASH(LJ_VMEVENT_BC):
    *slot = (uint32_t)LJ_VMEVENT_BC & 7u;
    return 1;
  case (uint32_t)VMEVENT_HASH(LJ_VMEVENT_TRACE):
    *slot = (uint32_t)LJ_VMEVENT_TRACE & 7u;
    return 1;
  case (uint32_t)VMEVENT_HASH(LJ_VMEVENT_RECORD):
    *slot = (uint32_t)LJ_VMEVENT_RECORD & 7u;
    return 1;
  case (uint32_t)VMEVENT_HASH(LJ_VMEVENT_TEXIT):
    *slot = (uint32_t)LJ_VMEVENT_TEXIT & 7u;
    return 1;
  case (uint32_t)VMEVENT_HASH(LJ_VMEVENT_ERRFIN):
    *slot = (uint32_t)LJ_VMEVENT_ERRFIN & 7u;
    return 1;
  default:
    return 0;
  }
}

#if LJ_HASJIT
enum {
  VMEVENT_ATTACHMENT_CLOCK_BUSY,
  VMEVENT_ATTACHMENT_CLOCK_INITIAL,
  VMEVENT_ATTACHMENT_CLOCK_PUBLISHED,
  VMEVENT_ATTACHMENT_CLOCK_CORRUPT
};

static int vmevent_attachment_snapshot_canonical(
  const LJJitEventAttachmentSnapshot *snapshot)
{
  if (!snapshot || (snapshot->sequence & 1u) != 0 ||
      snapshot->next_generation != snapshot->generation)
    return 0;
  /* Zero is the unique untouched state. Every successful writer claim must
  ** publish a nonzero generation, including conservative invalidation after
  ** a post-claim store collision. Thus a nonzero stable sequence paired with
  ** generation zero is corruption, not idle. */
  return (snapshot->sequence == 0) == (snapshot->generation == 0);
}

static int vmevent_attachment_snapshot_initial_idle(
  const LJJitEventAttachmentSnapshot *snapshot)
{
  return snapshot && snapshot->sequence == 0 &&
    snapshot->next_generation == 0 && snapshot->generation == 0;
}

static int vmevent_attachment_clock_read(
  LJJitEventAttachmentClock *clock,
  LJJitEventAttachmentSnapshot *snapshot)
{
  uint64_t sequence;
  memset(snapshot, 0, sizeof(*snapshot));
  sequence = la_load64_acq(&clock->sequence);
  if ((sequence & 1u) != 0)
    return VMEVENT_ATTACHMENT_CLOCK_BUSY;
  snapshot->sequence = sequence;
  snapshot->next_generation =
    la_load64_acq(&clock->next_generation);
  snapshot->generation = la_load64_acq(&clock->generation);
  if (la_load64_acq(&clock->sequence) != sequence) {
    memset(snapshot, 0, sizeof(*snapshot));
    return VMEVENT_ATTACHMENT_CLOCK_BUSY;
  }
  if (!vmevent_attachment_snapshot_canonical(snapshot)) {
    memset(snapshot, 0, sizeof(*snapshot));
    return VMEVENT_ATTACHMENT_CLOCK_CORRUPT;
  }
  return vmevent_attachment_snapshot_initial_idle(snapshot) ?
    VMEVENT_ATTACHMENT_CLOCK_INITIAL :
    VMEVENT_ATTACHMENT_CLOCK_PUBLISHED;
}
#endif

int lj_jit_event_attachment_snapshot(
  global_State *g, uint32_t slot, LJJitEventAttachmentSnapshot *snapshot)
{
  if (!snapshot)
    return LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY;
  memset(snapshot, 0, sizeof(*snapshot));
#if LJ_HASJIT
  if (g && g->main_tg && slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS) {
    LJJitEventAttachmentClock *clock =
      &g->main_tg->jit_event_attachment[slot];
    int state = vmevent_attachment_clock_read(clock, snapshot);
    if (state == VMEVENT_ATTACHMENT_CLOCK_INITIAL)
      return LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL;
    if (state == VMEVENT_ATTACHMENT_CLOCK_PUBLISHED)
      return LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED;
  }
#else
  UNUSED(g);
  UNUSED(slot);
#endif
  memset(snapshot, 0, sizeof(*snapshot));
  return LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY;
}

int lj_jit_event_attachment_writer_claim(
  global_State *g, uint32_t slot, LJJitEventAttachmentWriter *writer)
{
  if (!writer)
    return LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT;
  memset(writer, 0, sizeof(*writer));
#if LJ_HASJIT
  if (g && g->main_tg && slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS) {
    LJJitEventAttachmentClock *clock =
      &g->main_tg->jit_event_attachment[slot];
    LJJitEventAttachmentSnapshot snapshot;
    uint64_t sequence;
    int state = vmevent_attachment_clock_read(clock, &snapshot);
    if (state == VMEVENT_ATTACHMENT_CLOCK_BUSY)
      return LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY;
    if (state == VMEVENT_ATTACHMENT_CLOCK_CORRUPT)
      return LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT;
    if (snapshot.sequence > ~(uint64_t)3 ||
	snapshot.generation == ~(uint64_t)0)
      return LJ_JIT_EVENT_ATTACHMENT_WRITER_EXHAUSTED;
    sequence = snapshot.sequence;
    if (!la_cas64(&clock->sequence, &sequence, snapshot.sequence + 1u,
		  LA_ACQ_REL, LA_ACQ))
      return LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY;
    writer->g = g;
    writer->sequence = snapshot.sequence;
    writer->generation = snapshot.generation + 1u;
    writer->slot = slot;
    writer->claimed = 1;
    /* Once this reservation is visible behind the odd sequence, the only
    ** legal terminal operation is writer_publish(). */
    la_store64_rel(&clock->next_generation, writer->generation);
    return LJ_JIT_EVENT_ATTACHMENT_WRITER_CLAIMED;
  }
#else
  UNUSED(g);
  UNUSED(slot);
#endif
  return LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT;
}

void lj_jit_event_attachment_writer_publish(
  LJJitEventAttachmentWriter *writer)
{
#if LJ_HASJIT
  LJJitEventAttachmentClock *clock;
  global_State *g;
  if (!writer || writer->claimed != 1 || !(g = writer->g) || !g->main_tg ||
      writer->slot >= LJ_JIT_EVENT_ATTACHMENT_SLOTS ||
      (writer->sequence & 1u) != 0 ||
      writer->sequence > ~(uint64_t)3 || writer->generation == 0)
    abort();
  clock = &g->main_tg->jit_event_attachment[writer->slot];
  if (la_load64_acq(&clock->sequence) != writer->sequence + 1u ||
      la_load64_acq(&clock->next_generation) != writer->generation ||
      la_load64_acq(&clock->generation) != writer->generation - 1u)
    abort();
  /* The caller's exact semantic CAS precedes this function. Make the cache
  ** retryable before any reader can acquire the matching even sequence. */
  vmevmask_store_rel(g, VMEVENT_NOCACHE);
  la_store64_rel(&clock->generation, writer->generation);
  la_store64_rel(&clock->sequence, writer->sequence + 2u);
  memset(writer, 0, sizeof(*writer));
#else
  UNUSED(writer);
  abort();
#endif
}

static int vmevent_handler_acq(global_State *g, GCstr *s, VMEvent ev,
			       TValue *fnv)
{
  cTValue *tv = lj_tab_getstr(lj_registry_tab_acq(g), s);
  TValue tabv;
  if (tv) {
    lj_tv_load_acq(&tabv, tv);
    if (tvistab(&tabv)) {
      int hash = VMEVENT_HASH(ev);
      tv = lj_tab_getint(tabV(&tabv), hash);
      if (tv)
	lj_tv_load_acq(fnv, tv);
      return tv && tvisfunc(fnv);
    }
  }
  return 0;
}

ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev)
{
  global_State *g = G(L);
  GCstr *s;
  TValue fnv;
  /* Reserve before acquiring the handler. A concurrent jit.attach() may
  ** remove the registry's last reference immediately after the lookup; stack
  ** growth can allocate, poll and run GC, so carrying fnv only in a C local
  ** across that call would expose a reclaimed function. Once loaded below,
  ** the copy to L->top is allocation- and safepoint-free. */
  lj_state_checkstack(L, LUA_MINSTACK);
  s = lj_str_newlit(L, LJ_VMEVENTS_REGKEY);
  /* Detach may remove the registry's last source reference after the load.
  ** Keep the source generation alive through publication of the replacement
  ** stack root, then hand it to the active mark cycle before ending the SMR
  ** read section. This is a single-slot publication, not an O(stack) rescan. */
  if (!lj_gc2_smr_read_try(g))
    return 0;  /* Observational event: never wait behind reclamation. */
  if (vmevent_handler_acq(g, s, ev, &fnv)) {
    TValue *dst = L->top;
    setfuncV(L, dst, funcV(&fnv));
    lj_state_stack_pubtv(L, L, dst);
    L->top = dst + 1;
    if (LJ_FR2) setnilV(L->top++);
    lj_gc2_smr_read_leave(g);
    return savestack(L, L->top);
  }
  lj_gc2_smr_read_leave(g);
  vmevmask_update(g, VMEVENT_MASK(ev), 0);  /* No handler: cache this fact. */
  /* jit.attach() publishes the registry entry before invalidating the mask
  ** cache. If it raced our first lookup, a blind clear after that invalidation
  ** could permanently suppress the newly attached event. Validate the miss
  ** once after the clear; a later attachment will publish NOCACHE itself.
  */
  if (!lj_gc2_smr_read_try(g)) {
    /* The first miss already cleared this cache bit. Keep it retryable: an
    ** attach may have raced that clear while reclamation owns the read gate. */
    (void)vmevmask_update(g, 0, VMEVENT_MASK(ev));
    return 0;
  }
  if (vmevent_handler_acq(g, s, ev, &fnv))
    (void)vmevmask_update(g, 0, VMEVENT_MASK(ev));
  lj_gc2_smr_read_leave(g);
  return 0;
}

lua_State *lj_vmevent_state(global_State *g)
{
  lua_State *L = g ? lj_tg_cur_L(g) : NULL;
  /* The shared legacy vmthread stack is not a concurrency primitive. Internal
  ** callers pass their initiating/claimed L explicitly; compatibility callers
  ** without a TG-current state skip this observational event instead of racing
  ** another TG's callback stack. */
  return L && G(L) == g ? L : NULL;
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

void lj_vmevent_call(lua_State *L, ptrdiff_t argbase, ptrdiff_t oldtop)
{
  global_State *g = G(L);
  uint32_t tid = lj_thr_current_id(g);
  uint32_t expect = 0;
  lua_State *oldL = lj_tg_cur_L(g);
  TGState *tg = G2TG(g);
  TGState *old_tg_hint;
  ptrdiff_t oldbase;
  uint32_t actions = 0;
  int had_stopreq = 0;
  int status;
#if LJ_HASJIT
  jit_State *J = G2J(g);
  lua_State *oldJL = NULL;
  int owns_jit = 0;
#endif

  /* Handler lookup and argument construction are owner-local on L. Serialize
  ** only the protected callback and universe-global hook/mask state. A loser
  ** never waits: discard this racy instrumentation event and restore its exact
  ** pre-prepare stack top, which may have moved while building arguments. */
  if (tid == 0 || !vmevent_owner_cas(g, &expect, tid)) {
    L->top = restorestack(L, oldtop);
    return;
  }
  if (!hookmask_vmevent_enter(g)) {
    L->top = restorestack(L, oldtop);
    vmevent_owner_rel(g, tid);
    return;
  }

  old_tg_hint = L->tg_hint;
  oldbase = savestack(L, L->base);
#if LJ_HASJIT
  owns_jit = lj_jit_token_held(J);
  if (owns_jit)
    oldJL = jit_owner_l_acq(J);  /* Snapshot the actual pointer under token. */
#endif
  L->tg_hint = tg;
  /* Do not overwrite the global VM-event cache here. The exact callback owner
  ** already turns nested events into bounded drops, while a temporary zero mask
  ** can erase a concurrent jit.attach() VMEVENT_NOCACHE invalidation.
  */
  status = lj_vm_pcall_unwind(L, restorestack(L, argbase), 0+1, 0);
  if (LJ_UNLIKELY(status)) {
    /* Really shouldn't use stderr here, but where else to complain? */
    L->top--;
    had_stopreq = lj_safepoint_had_stopreq(L);
    actions = vmevent_report_failure(L);
  }
  L->base = restorestack(L, oldbase);
  L->top = restorestack(L, oldtop);
  if (oldL)
    lj_tg_setcur_L(g, oldL);
  else
    lj_tg_clearcur_L(g);
#if LJ_HASJIT
  /* TEXIT/BC/finalizer events can run while a peer owns the recorder. Such an
  ** event must not manufacture a "restore" write to the universe-global J->L.
  ** Only the unchanged token owner may restore the pointer it actually read. */
  if (owns_jit && lj_jit_token_held(J))
    jit_owner_l_rel(J, oldJL);
#endif
  L->tg_hint = old_tg_hint;
  hookmask_vmevent_leave(g);
  vmevent_owner_rel(g, tid);  /* Release before STOPREQ can throw. */
  if (LJ_UNLIKELY(status))
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
}
