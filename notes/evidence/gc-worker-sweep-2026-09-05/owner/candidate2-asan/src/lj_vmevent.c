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
#include "lj_gc.h"
#include "lj_state.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_profile.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

#if defined(LJ_GC2_TEST_HELPERS)
static LJVMEVENTPrepareTestHook vmevent_prepare_test_hook;
static void *vmevent_prepare_test_ud;
#if LJ_HASJIT
static LJJitVMEVENTCallTestHook jit_vmevent_call_test_hook;
static void *jit_vmevent_call_test_ud;
#endif

void lj_vmevent_test_set_prepare_hook(LJVMEVENTPrepareTestHook hook,
				       void *ud)
{
  vmevent_prepare_test_hook = hook;
  vmevent_prepare_test_ud = ud;
}

#if LJ_HASJIT
void lj_jit_vmevent_call_test_set_hook(
  LJJitVMEVENTCallTestHook hook, void *ud)
{
  jit_vmevent_call_test_hook = hook;
  jit_vmevent_call_test_ud = ud;
}
#endif
#endif

void lj_vmevent_init(lua_State *L)
{
#ifndef LUAJIT_DISABLE_VMEVENT
  global_State *g = G(L);
  TGState *tg = g->main_tg;
  GCstr *key;
#if LJ_HASJIT
  GCstr *trace_flush_reason;
#endif
  if (LJ_UNLIKELY(!tg))
    abort();
  key = lj_str_newlit(L, LJ_VMEVENTS_REGKEY);
  /* This is the sole allocating/string-table admission for the internal
  ** registry key. VM-event observations only acquire-load this immutable
  ** pointer and therefore never enter strtab_wait(). */
  fixstring(g, key);
  la_storeptr_rel((void **)&tg->vmevent_regkey, key);
#if LJ_HASJIT
  trace_flush_reason = lj_str_newlit(L, "flush");
  /* Standalone FLUSH runs after the runtime may be concurrent. Intern and fix
  ** its sole argument during bootstrap so delivery never enters the string
  ** table writer/wait protocol while it owns the low JIT token. */
  fixstring(g, trace_flush_reason);
  la_storeptr_rel((void **)&tg->jit_trace_flush_reason, trace_flush_reason);
#endif
#else
  UNUSED(L);
#endif
}

#if LJ_HASJIT
GCstr *lj_vmevent_trace_flush_reason_acq(global_State *g)
{
#ifndef LUAJIT_DISABLE_VMEVENT
  TGState *tg = g ? g->main_tg : NULL;
  return tg ? (GCstr *)la_loadptr_acq(
    (void *const *)&tg->jit_trace_flush_reason) : NULL;
#else
  UNUSED(g);
  return NULL;
#endif
}
#endif

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

typedef struct VMEVENTPrepareRoots {
  ptrdiff_t oldtop;
  ptrdiff_t handler;
  ptrdiff_t key;
  ptrdiff_t table;
  ptrdiff_t argbase;
  ptrdiff_t rootend;
} VMEVENTPrepareRoots;

static void vmevent_prepare_result_reset(LJVMEVENTPrepareResult *result)
{
  memset(result, 0, sizeof(*result));
  result->slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  result->attachment_state = LJ_VMEVENT_ATTACHMENT_INVALID;
}

static int vmevent_event_slot(VMEvent ev, uint32_t *slot)
{
  switch (ev) {
  case LJ_VMEVENT_BC:
  case LJ_VMEVENT_TRACE:
  case LJ_VMEVENT_RECORD:
  case LJ_VMEVENT_TEXIT:
  case LJ_VMEVENT_ERRFIN:
    return lj_jit_event_attachment_clock_slot(VMEVENT_HASH(ev), slot);
  default:
    *slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
    return 0;
  }
}

#ifndef LUAJIT_DISABLE_VMEVENT
static GCstr *vmevent_regkey_acq(global_State *g)
{
  TGState *tg = g ? g->main_tg : NULL;
  return tg ? (GCstr *)la_loadptr_acq(
    (void *const *)&tg->vmevent_regkey) : NULL;
}

static int vmevent_prepare_roots_open(lua_State *L,
				       VMEVENTPrepareRoots *roots)
{
  GCstr *key;
  roots->oldtop = savestack(L, L->top);
  key = vmevent_regkey_acq(G(L));
  if (LJ_UNLIKELY(!key || key->gct != (uint8_t)~LJ_TSTR))
    return 0;

  /* All allocation and possible stack relocation precedes clock A and both
  ** bounded table observations. The key was interned and fixed once during
  ** state bootstrap; this path never enters the string-table writer gate. */
  lj_state_checkstack(L, LUA_MINSTACK);
  roots->handler = savestack(L, L->top);
  setnilV(L->top++);
  if (LJ_FR2)
    setnilV(L->top++);
  roots->argbase = savestack(L, L->top);
  roots->key = savestack(L, L->top);
  setstrV(L, L->top, key);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  roots->table = savestack(L, L->top);
  setnilV(L->top++);
  roots->rootend = savestack(L, L->top);
  return 1;
}

static void vmevent_prepare_roots_close(lua_State *L,
					 const VMEVENTPrepareRoots *roots,
					 int ready)
{
  L->top = restorestack(L, ready ? roots->argbase : roots->oldtop);
}

#if defined(LJ_GC2_TEST_HELPERS)
static void vmevent_prepare_test_call(lua_State *L, VMEvent ev, int stage,
				      const VMEVENTPrepareRoots *roots)
{
  LJVMEVENTPrepareTestHook hook = vmevent_prepare_test_hook;
  void *ud = vmevent_prepare_test_ud;
  if (hook) {
    /* One-shot before the callout: a hook which invokes reader-adjacent test
    ** code cannot recursively re-enter itself. A staged fixture may arm the
    ** same hook again explicitly for a later point. */
    vmevent_prepare_test_hook = NULL;
    vmevent_prepare_test_ud = NULL;
    hook(L, ev, stage, ud);
  }
  /* Hooks may force a relocation or leave scratch values above the four
  ** semantic roots. Never retain a raw stack address across that callout. */
  L->top = restorestack(L, roots->rootend);
}
#else
#define vmevent_prepare_test_call(L, ev, stage, roots) \
  ((void)(L), (void)(ev), (void)(stage), (void)(roots))
#endif

/* A complete two-level raw registry observation. Every table/vector authority
** interval ends inside the bounded rooted getter before a test hook can run.
** FOUND with a non-function value is semantic absence, matching stock VM-event
** dispatch and deliberately bypassing metatables at both levels. */
static int vmevent_handler_lookup_try(lua_State *L, VMEvent ev,
				      const VMEVENTPrepareRoots *roots)
{
  TValue *key = restorestack(L, roots->key);
  TValue *table = restorestack(L, roots->table);
  TValue *handler = restorestack(L, roots->handler);
  int status = lj_tab_gettv_rooted_try(L, registry(L), key, table);

  vmevent_prepare_test_call(L, ev,
			    LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP, roots);
  if (status == LJ_TAB_ROOTED_GET_RETRY)
    return LJ_VMEVENT_PREPARE_RETRY;
  if (status == LJ_TAB_ROOTED_GET_ABSENT)
    return LJ_VMEVENT_PREPARE_ABSENT;

  table = restorestack(L, roots->table);
  handler = restorestack(L, roots->handler);
  status = lj_tab_getinttv_rooted_try(L, table, VMEVENT_HASH(ev), handler);
  vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_AFTER_EVENT_LOOKUP,
			    roots);
  if (status == LJ_TAB_ROOTED_GET_RETRY)
    return LJ_VMEVENT_PREPARE_RETRY;
  handler = restorestack(L, roots->handler);
  return status == LJ_TAB_ROOTED_GET_FOUND && tvisfunc(handler) ?
    LJ_VMEVENT_PREPARE_READY : LJ_VMEVENT_PREPARE_ABSENT;
}

#if LJ_HASJIT
static int vmevent_attachment_snapshots_equal(
  int state_a, const LJJitEventAttachmentSnapshot *a,
  int state_b, const LJJitEventAttachmentSnapshot *b)
{
  return state_a == state_b &&
    (state_a == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL ||
     state_a == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED) &&
    a->sequence == b->sequence &&
    a->next_generation == b->next_generation &&
    a->generation == b->generation;
}
#endif

static void vmevent_prepare_accept(LJVMEVENTPrepareResult *result,
				    uint32_t slot, uint32_t state,
				    const LJJitEventAttachmentSnapshot *snapshot,
				    ptrdiff_t argbase)
{
  result->argbase = argbase;
  if (snapshot)
    result->attachment = *snapshot;
  result->slot = slot;
  result->attachment_state = state;
}
#endif /* !LUAJIT_DISABLE_VMEVENT */

int lj_vmevent_prepare_try(lua_State *L, VMEvent ev,
			    LJVMEVENTPrepareResult *result)
{
  uint32_t slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  if (!result)
    return LJ_VMEVENT_PREPARE_RETRY;
  vmevent_prepare_result_reset(result);
  if (!L || !vmevent_event_slot(ev, &slot))
    return LJ_VMEVENT_PREPARE_RETRY;

#ifdef LUAJIT_DISABLE_VMEVENT
  result->slot = slot;
  result->attachment_state = LJ_VMEVENT_ATTACHMENT_UNCLOCKED;
  return LJ_VMEVENT_PREPARE_ABSENT;
#else
  {
    global_State *g = G(L);
    VMEVENTPrepareRoots roots;
    int lookup;
    uint8_t event_mask = VMEVENT_MASK(ev);

    if (!vmevent_prepare_roots_open(L, &roots))
      return LJ_VMEVENT_PREPARE_RETRY;

#if LJ_HASJIT
    {
      LJJitEventAttachmentSnapshot snapshot_a, snapshot_b;
      int clock_a = lj_jit_event_attachment_snapshot(g, slot, &snapshot_a);
      int clock_b;
      int cleared = 0;

      vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_AFTER_CLOCK_A,
				&roots);
      if (clock_a == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY) {
	vmevent_prepare_roots_close(L, &roots, 0);
	return LJ_VMEVENT_PREPARE_RETRY;
      }

      lookup = vmevent_handler_lookup_try(L, ev, &roots);
      if (lookup == LJ_VMEVENT_PREPARE_RETRY) {
	vmevent_prepare_roots_close(L, &roots, 0);
	return LJ_VMEVENT_PREPARE_RETRY;
      }
      if (lookup == LJ_VMEVENT_PREPARE_ABSENT) {
	vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_BEFORE_MASK_CLEAR,
				  &roots);
	(void)vmevmask_clear_bits_acqrel(g, event_mask);
	cleared = 1;
	vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_AFTER_MASK_CLEAR,
				  &roots);
      }

      /* Pairs the handler/rooted-table reads and any cache-bit clear with the
      ** writer's release publication before accepting clock B. */
      la_fence_acq();
      clock_b = lj_jit_event_attachment_snapshot(g, slot, &snapshot_b);
      if (!vmevent_attachment_snapshots_equal(
	    clock_a, &snapshot_a, clock_b, &snapshot_b)) {
	if (cleared)
	  (void)vmevmask_set_bits_acqrel(g, event_mask);
	vmevent_prepare_roots_close(L, &roots, 0);
	return LJ_VMEVENT_PREPARE_RETRY;
      }

      vmevent_prepare_accept(result, slot,
	clock_b == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL ?
	  LJ_VMEVENT_ATTACHMENT_INITIAL : LJ_VMEVENT_ATTACHMENT_PUBLISHED,
	&snapshot_b, lookup == LJ_VMEVENT_PREPARE_READY ? roots.argbase : 0);
      vmevent_prepare_roots_close(
	L, &roots, lookup == LJ_VMEVENT_PREPARE_READY);
      return lookup;
    }
#else
    vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_AFTER_CLOCK_A, &roots);
    lookup = vmevent_handler_lookup_try(L, ev, &roots);
    if (lookup == LJ_VMEVENT_PREPARE_READY) {
      vmevent_prepare_accept(result, slot, LJ_VMEVENT_ATTACHMENT_UNCLOCKED,
			     NULL, roots.argbase);
      vmevent_prepare_roots_close(L, &roots, 1);
      return LJ_VMEVENT_PREPARE_READY;
    }
    if (lookup == LJ_VMEVENT_PREPARE_RETRY) {
      vmevent_prepare_roots_close(L, &roots, 0);
      return LJ_VMEVENT_PREPARE_RETRY;
    }

    vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_BEFORE_MASK_CLEAR,
			      &roots);
    (void)vmevmask_clear_bits_acqrel(g, event_mask);
    vmevent_prepare_test_call(L, ev, LJ_VMEVENT_TEST_AFTER_MASK_CLEAR,
			      &roots);
    la_fence_acq();
    lookup = vmevent_handler_lookup_try(L, ev, &roots);
    if (lookup != LJ_VMEVENT_PREPARE_ABSENT) {
      (void)vmevmask_set_bits_acqrel(g, event_mask);
      vmevent_prepare_roots_close(L, &roots, 0);
      return LJ_VMEVENT_PREPARE_RETRY;
    }

    vmevent_prepare_accept(result, slot, LJ_VMEVENT_ATTACHMENT_UNCLOCKED,
			   NULL, 0);
    vmevent_prepare_roots_close(L, &roots, 0);
    return LJ_VMEVENT_PREPARE_ABSENT;
#endif
  }
#endif
}

ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev)
{
  LJVMEVENTPrepareResult result;
  return lj_vmevent_prepare_try(L, ev, &result) == LJ_VMEVENT_PREPARE_READY ?
    result.argbase : 0;
}

#if LJ_HASJIT
static int jit_event_callback_event_valid(uint32_t event)
{
  return event >= LJ_JIT_EVENT_TRACE_START && event <= LJ_JIT_EVENT_RECORD;
}

static int jit_event_callback_owner_idle_raw(
  const LJJitEventCallbackOwner *owner)
{
  return la_load64_acq(&owner->generation) == 0 &&
    la_load64_acq(&owner->stream_generation) == 0 &&
    la_load64_acq(&owner->session_generation) == 0 &&
    la_load32_acq(&owner->state) == LJ_JIT_EVENT_CALLBACK_IDLE &&
    la_load32_acq(&owner->owner_actor) == 0 &&
    la_load32_acq(&owner->event) == 0 &&
    la_load32_acq(&owner->session_slot) == 0 &&
    la_loadptr_acq((void *const *)&owner->owner_L) == NULL;
}

static int jit_event_callback_owner_active_raw(
  const LJJitEventCallbackOwner *owner, uint32_t state)
{
  uint64_t generation = la_load64_acq(&owner->generation);
  uint32_t actor = la_load32_acq(&owner->owner_actor);
  return (state == LJ_JIT_EVENT_CALLBACK_CALLING ||
	  state == LJ_JIT_EVENT_CALLBACK_UNWINDING) &&
    generation != 0 &&
    la_load64_acq(&owner->next_generation) == generation &&
    la_load64_acq(&owner->stream_generation) != 0 &&
    la_load64_acq(&owner->session_generation) != 0 &&
    actor != 0 && actor != LJ_THR_ACTOR_RETIRED &&
    jit_event_callback_event_valid(la_load32_acq(&owner->event)) &&
    la_load32_acq(&owner->session_slot) < LJ_JIT_EVENT_SESSION_SLOTS &&
    la_loadptr_acq((void *const *)&owner->owner_L) != NULL;
}

int lj_jit_event_callback_snapshot(
  TGState *tg, LJJitEventCallbackSnapshot *snapshot)
{
  LJJitEventCallbackOwner *owner;
  uint64_t sequence;
  if (!snapshot)
    return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!tg)
    return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY;
  owner = &tg->jit_event_callback_owner;
  sequence = la_load64_acq(&owner->sequence);
  if ((sequence & 1u) != 0)
    return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY;
  snapshot->tg = tg;
  snapshot->sequence = sequence;
  snapshot->next_generation = la_load64_acq(&owner->next_generation);
  snapshot->generation = la_load64_acq(&owner->generation);
  snapshot->stream_generation =
    la_load64_acq(&owner->stream_generation);
  snapshot->session_generation =
    la_load64_acq(&owner->session_generation);
  snapshot->state = la_load32_acq(&owner->state);
  snapshot->owner_actor = la_load32_acq(&owner->owner_actor);
  snapshot->event = la_load32_acq(&owner->event);
  snapshot->session_slot = la_load32_acq(&owner->session_slot);
  snapshot->owner_L = (lua_State *)
    la_loadptr_acq((void *const *)&owner->owner_L);
  if (la_load64_acq(&owner->sequence) != sequence)
    goto retry;
  if (snapshot->state == LJ_JIT_EVENT_CALLBACK_IDLE) {
    if (!jit_event_callback_owner_idle_raw(owner) ||
	(lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) != 0 ||
	la_load64_acq(&owner->sequence) != sequence)
      goto retry;
    return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE;
  }
  if (!jit_event_callback_owner_active_raw(owner, snapshot->state) ||
      snapshot->generation != snapshot->next_generation ||
      snapshot->stream_generation == 0 ||
      snapshot->session_generation == 0 ||
      snapshot->owner_actor == 0 ||
      snapshot->owner_actor == LJ_THR_ACTOR_RETIRED ||
      snapshot->owner_actor != lj_tg_actor_acq(tg) ||
      !jit_event_callback_event_valid(snapshot->event) ||
      snapshot->session_slot >= LJ_JIT_EVENT_SESSION_SLOTS ||
      snapshot->owner_L == NULL ||
      (lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) !=
	(HOOK_ACTIVE|HOOK_VMEVENT) ||
      la_load64_acq(&owner->sequence) != sequence)
    goto retry;
  return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE;
retry:
  memset(snapshot, 0, sizeof(*snapshot));
  return LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY;
}

int lj_jit_event_callback_idle(TGState *tg)
{
  LJJitEventCallbackSnapshot snapshot;
  return lj_jit_event_callback_snapshot(tg, &snapshot) ==
    LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE;
}

static int jit_event_callback_session_exact(
  lua_State *L, const LJJitEventSessionSnapshot *snapshot)
{
  TGState *tg;
  LJJitEventSessions *sessions;
  const LJJitEventSessionSlot *slot;
  GCRef *roots;
  uint32_t nroots, capacity, actor;
  uint64_t sequence;
  if (!L || !snapshot || !(tg = snapshot->tg) || snapshot->g != G(L) ||
      tg->gl != snapshot->g || L2TG(L) != tg || G2TG(G(L)) != tg ||
      snapshot->slot_index >= LJ_JIT_EVENT_SESSION_SLOTS ||
      snapshot->generation == 0 || snapshot->callback_root_count != 1u ||
      !snapshot->callback_handler ||
      !jit_event_callback_event_valid(snapshot->event))
    return 0;
  sessions = &tg->jit_event_sessions;
  slot = &sessions->slot[snapshot->slot_index];
  if (snapshot->slot != slot)
    return 0;
  sequence = la_load64_acq(&sessions->sequence);
  nroots = la_load32_acq(&slot->root_count);
  capacity = la_load32_acq(&slot->root_capacity);
  roots = (GCRef *)la_loadptr_acq((void *const *)&slot->root_data);
  actor = lj_tg_actor_acq(tg);
  if ((sequence & 1u) != 0 || sequence != snapshot->sequence ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) != snapshot->slot_index ||
      la_load64_acq(&sessions->active_generation) != snapshot->generation ||
      la_load64_acq(&sessions->next_generation) != snapshot->generation ||
      la_load32_acq(&slot->state) != LJ_JIT_EVENT_SLOT_ACTIVE ||
      la_load64_acq(&slot->generation) != snapshot->generation ||
      la_load32_acq(&slot->event) != snapshot->event ||
      la_load32_acq(&slot->owner_mode) != snapshot->owner_mode ||
      la_load32_acq(&slot->edge_proof) != snapshot->edge_proof ||
      la_load64_acq(&slot->attachment_generation) !=
	snapshot->attachment_generation ||
      la_load32_acq(&slot->attachment_state) != snapshot->attachment_state ||
      la_load32_acq(&slot->callback_root_count) != 1u ||
      (la_load32_acq(&slot->flags) &
	LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) == 0 ||
      !roots || nroots >= capacity ||
      gcref_acq(roots[nroots]) != obj2gco(snapshot->callback_handler) ||
      la_load32_acq(&slot->owner_actor) != actor ||
      actor == 0 || actor == LJ_THR_ACTOR_RETIRED ||
      actor != lj_thr_actor_current() ||
      la_loadptr_acq((void *const *)&slot->owner_L) != L ||
      gcref_acq(slot->owner_root) != obj2gco(L) ||
      lj_tg_load_cur_L(tg) != L || !lj_tg_owns_state_acq(tg, L))
    return 0;
  return la_load64_acq(&sessions->sequence) == sequence;
}

static int jit_event_callback_owner_matches(
  const LJJitEventCallbackOwner *owner,
  const LJJitEventCallbackHandle *handle, uint32_t state)
{
  return handle && owner && handle->tg && handle->owner_L &&
    handle->generation != 0 && handle->stream_generation != 0 &&
    handle->session_generation != 0 &&
    la_load64_acq(&owner->generation) == handle->generation &&
    la_load64_acq(&owner->next_generation) == handle->generation &&
    la_load64_acq(&owner->stream_generation) == handle->stream_generation &&
    la_load64_acq(&owner->session_generation) ==
      handle->session_generation &&
    la_load32_acq(&owner->state) == state &&
    la_load32_acq(&owner->owner_actor) == handle->owner_actor &&
    la_load32_acq(&owner->event) == handle->event &&
    la_load32_acq(&owner->session_slot) == handle->session_slot &&
    la_loadptr_acq((void *const *)&owner->owner_L) == handle->owner_L;
}

static int jit_event_callback_sequence_claim(
  LJJitEventCallbackOwner *owner, uint64_t reserve, uint64_t *sequence)
{
  uint64_t value;
  if (!owner || !sequence || reserve == 0 || (reserve & 1u) != 0)
    return 0;
  value = la_load64_acq(&owner->sequence);
  if ((value & 1u) != 0 || value > ~(uint64_t)0 - reserve)
    return 0;
  if (!la_cas64(&owner->sequence, &value, value + 1u,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  *sequence = value;
  return 1;
}

static void jit_event_callback_sequence_publish(
  LJJitEventCallbackOwner *owner, uint64_t sequence)
{
  la_store64_rel(&owner->sequence, sequence + 2u);
}

int lj_jit_event_callback_claim_l(
  lua_State *L, uint64_t stream_generation,
  const LJJitEventSessionSnapshot *session,
  LJJitEventCallbackHandle *handle)
{
  LJJitEventCallbackOwner *owner;
  LJJitEventCallbackSnapshot before;
  TGState *tg;
  uint64_t sequence, generation;
  uint32_t actor, tid;
  if (!handle)
    return 0;
  memset(handle, 0, sizeof(*handle));
  if (!L || stream_generation == 0 || !session || !(tg = session->tg) ||
      !jit_event_callback_session_exact(L, session) ||
      lj_jit_event_callback_snapshot(tg, &before) !=
	LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE ||
      before.next_generation == ~(uint64_t)0)
    return 0;
  owner = &tg->jit_event_callback_owner;
  actor = lj_tg_actor_acq(tg);
  tid = lj_tg_tid_acq(tg);
  if (tid == 0 || vmevent_owner_acq(tg->gl) == tid ||
#if LJ_HASPROFILE
      lj_profile_callback_active_tg(tg) ||
#endif
      actor == 0 || actor == LJ_THR_ACTOR_RETIRED ||
      actor != lj_thr_actor_current() ||
      !jit_event_callback_sequence_claim(owner, 6u, &sequence))
    return 0;
  if (!jit_event_callback_owner_idle_raw(owner) ||
      la_load64_acq(&owner->next_generation) != before.next_generation ||
      !jit_event_callback_session_exact(L, session) ||
      lj_tg_tid_acq(tg) != tid || vmevent_owner_acq(tg->gl) == tid ||
#if LJ_HASPROFILE
      lj_profile_callback_active_tg(tg) ||
#endif
      lj_tg_actor_acq(tg) != actor || actor != lj_thr_actor_current() ||
      !lj_tg_hookmask_callback_enter_try(tg)) {
    jit_event_callback_sequence_publish(owner, sequence);
    return 0;
  }
#if LJ_HASPROFILE
  /* Close a stale-read race with PROFILE -> callback_tg handoff before any
  ** callback owner is published. The local hook reservation is reversible. */
  if (lj_profile_callback_active_tg(tg)) {
    if (LJ_UNLIKELY(!lj_tg_hookmask_callback_leave_exact(tg)))
      abort();
    jit_event_callback_sequence_publish(owner, sequence);
    return 0;
  }
#endif
  generation = before.next_generation + 1u;
  la_store64_rel(&owner->next_generation, generation);
  la_store64_rel(&owner->generation, generation);
  la_store64_rel(&owner->stream_generation, stream_generation);
  la_store64_rel(&owner->session_generation, session->generation);
  la_store32_rel(&owner->state, LJ_JIT_EVENT_CALLBACK_CALLING);
  la_store32_rel(&owner->owner_actor, actor);
  la_store32_rel(&owner->event, session->event);
  la_store32_rel(&owner->session_slot, session->slot_index);
  la_storeptr_rel((void **)&owner->owner_L, L);
  jit_event_callback_sequence_publish(owner, sequence);

  handle->tg = tg;
  handle->owner_L = L;
  handle->generation = generation;
  handle->stream_generation = stream_generation;
  handle->session_generation = session->generation;
  handle->owner_actor = actor;
  handle->event = session->event;
  handle->session_slot = session->slot_index;
  return 1;
}

static int jit_event_callback_handle_owner(
  lua_State *L, const LJJitEventCallbackHandle *handle,
  LJJitEventCallbackOwner **ownerp)
{
  TGState *tg;
  if (!L || !handle || !(tg = handle->tg) || handle->owner_L != L ||
      G(L) != tg->gl || L2TG(L) != tg || G2TG(G(L)) != tg ||
      handle->owner_actor == 0 ||
      handle->owner_actor == LJ_THR_ACTOR_RETIRED ||
      handle->owner_actor != lj_tg_actor_acq(tg) ||
      handle->owner_actor != lj_thr_actor_current())
    return 0;
  *ownerp = &tg->jit_event_callback_owner;
  return 1;
}

int lj_jit_event_callback_unwind_l(
  lua_State *L, const LJJitEventCallbackHandle *handle)
{
  LJJitEventCallbackOwner *owner;
  uint64_t sequence;
  if (!jit_event_callback_handle_owner(L, handle, &owner) ||
      !jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_CALLING) ||
      !jit_event_callback_sequence_claim(owner, 4u, &sequence))
    return 0;
  if (!jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_CALLING)) {
    jit_event_callback_sequence_publish(owner, sequence);
    return 0;
  }
  la_store32_rel(&owner->state, LJ_JIT_EVENT_CALLBACK_UNWINDING);
  jit_event_callback_sequence_publish(owner, sequence);
  return 1;
}

int lj_jit_event_callback_release_l(
  lua_State *L, const LJJitEventCallbackHandle *handle)
{
  LJJitEventCallbackOwner *owner;
  uint64_t sequence;
  if (!jit_event_callback_handle_owner(L, handle, &owner) ||
      !jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_UNWINDING) ||
      !jit_event_callback_sequence_claim(owner, 2u, &sequence))
    return 0;
  if (!jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_UNWINDING)) {
    jit_event_callback_sequence_publish(owner, sequence);
    return 0;
  }
  if (LJ_UNLIKELY(!lj_tg_hookmask_callback_leave_exact(handle->tg)))
    abort();
  la_store64_rel(&owner->generation, 0);
  la_store64_rel(&owner->stream_generation, 0);
  la_store64_rel(&owner->session_generation, 0);
  la_store32_rel(&owner->state, LJ_JIT_EVENT_CALLBACK_IDLE);
  la_store32_rel(&owner->owner_actor, 0);
  la_store32_rel(&owner->event, 0);
  la_store32_rel(&owner->session_slot, 0);
  la_storeptr_rel((void **)&owner->owner_L, NULL);
  jit_event_callback_sequence_publish(owner, sequence);
  return 1;
}
#endif

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

#if LJ_HASJIT
static int jit_vmevent_callback_stream_exact(
  lua_State *L, const LJJitEventCallbackHandle *handle)
{
  LJJitTraceStreamSnapshot stream;
  TGState *tg = handle ? handle->tg : NULL;
  if (!L || !tg || handle->event != LJ_JIT_EVENT_TRACE_FLUSH ||
      lj_jit_trace_stream_snapshot(G(L), &stream) !=
	LJ_JIT_STREAM_SNAPSHOT_ACTIVE)
    return 0;
  return stream.generation == handle->stream_generation &&
    stream.event_ordinal == 1u &&
    lj_tgregistry_key_equal(&stream.owner_key, &tg->registry_key) &&
    stream.owner_tid == lj_tg_tid_acq(tg) &&
    stream.owner_actor == handle->owner_actor &&
    stream.phase == LJ_JIT_STREAM_DETACHED_CALLBACK &&
    stream.traceno == 0 &&
    stream.callback_event == handle->event &&
    stream.callback_slot == handle->session_slot &&
    stream.callback_session_generation == handle->session_generation &&
    stream.terminal_event == handle->event &&
    stream.terminal_slot == handle->session_slot &&
    stream.terminal_session_generation == handle->session_generation;
}

/* The callback owner prevents exact session close, so its immutable root
** vector may be checked directly without reacquiring a GC2 reader.  Keep the
** scalar publication checks on both sides of the root load: this helper is a
** bounded refusal for malformed internal geometry, never permission to call a
** merely stack-shaped function. */
static int jit_vmevent_callback_handler_exact(
  lua_State *L, const LJJitEventCallbackHandle *handle, GCfunc *handler)
{
  LJJitEventSessions *sessions;
  const LJJitEventSessionSlot *slot;
  GCRef *roots;
  uint64_t sequence;
  uint32_t capacity, nroots;
  TGState *tg = handle ? handle->tg : NULL;
  if (!L || !tg || !handler || handle->session_generation == 0 ||
      handle->session_slot >= LJ_JIT_EVENT_SESSION_SLOTS)
    return 0;
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) != handle->session_slot ||
      la_load64_acq(&sessions->active_generation) !=
	handle->session_generation ||
      la_load64_acq(&sessions->next_generation) !=
	handle->session_generation)
    return 0;
  slot = &sessions->slot[handle->session_slot];
  roots = (GCRef *)la_loadptr_acq((void *const *)&slot->root_data);
  capacity = la_load32_acq(&slot->root_capacity);
  nroots = la_load32_acq(&slot->root_count);
  if (!roots || ((uintptr_t)roots & (__alignof__(GCRef) - 1u)) != 0 ||
      capacity < LJ_JIT_EVENT_SESSION_ROOTS || nroots >= capacity ||
      nroots > LJ_ROOT_SCAN_LIMIT ||
      ((roots == slot->root_inline) !=
	(capacity == LJ_JIT_EVENT_SESSION_ROOTS)))
    return 0;
  return la_load64_acq(&sessions->sequence) == sequence &&
    la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_ACTIVE &&
    la_load32_acq(&sessions->active_slot) == handle->session_slot &&
    la_load64_acq(&sessions->active_generation) ==
      handle->session_generation &&
    la_load64_acq(&slot->generation) == handle->session_generation &&
    la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_ACTIVE &&
    la_load32_acq(&slot->event) == handle->event &&
    la_load32_acq(&slot->callback_root_count) == 1u &&
    (la_load32_acq(&slot->flags) &
      LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) != 0 &&
    la_load32_acq(&slot->owner_tid) == lj_tg_tid_acq(tg) &&
    la_load32_acq(&slot->owner_actor) == handle->owner_actor &&
    la_loadptr_acq((void *const *)&slot->owner_L) == L &&
    gcref_acq(slot->owner_root) == obj2gco(L) &&
    gcref_acq(roots[nroots]) == obj2gco(handler) &&
    la_load64_acq(&sessions->sequence) == sequence;
}

int lj_jit_vmevent_call_l(
  lua_State *L, ptrdiff_t argbase, ptrdiff_t oldtop,
  const LJJitEventCallbackHandle *handle, LJJitVMEVENTCallResult *result)
{
  LJJitVMEVENTCallResult out;
  LJJitEventCallbackOwner *owner;
  global_State *g;
  TGState *tg;
  lua_State *oldL;
  TGState *old_tg_hint;
  ptrdiff_t currenttop;
  ptrdiff_t oldbase;
  GCfunc *handler;

  if (!result)
    return 0;
  memset(result, 0, sizeof(*result));
  if (!L || !handle || !(tg = handle->tg) ||
      !jit_event_callback_handle_owner(L, handle, &owner) ||
      !jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_CALLING))
    return 0;
  currenttop = savestack(L, L->top);
  if (oldtop < 0 || oldtop > currenttop ||
      argbase < oldtop || argbase > currenttop ||
      ((uintptr_t)oldtop % sizeof(TValue)) != 0 ||
      ((uintptr_t)argbase % sizeof(TValue)) != 0 ||
      argbase - oldtop !=
	(ptrdiff_t)((1u+LJ_FR2) * sizeof(TValue)) ||
      !tvisfunc(restorestack(L, oldtop)))
    return 0;
  handler = funcV(restorestack(L, oldtop));
  if (!jit_vmevent_callback_stream_exact(L, handle) ||
      !jit_vmevent_callback_handler_exact(L, handle, handler) ||
      !jit_event_callback_owner_matches(
	owner, handle, LJ_JIT_EVENT_CALLBACK_CALLING))
    return 0;

  memset(&out, 0, sizeof(out));
  g = G(L);
  oldL = lj_tg_load_cur_L(tg);
  old_tg_hint = L->tg_hint;
  oldbase = savestack(L, L->base);
  out.had_stopreq = lj_safepoint_had_stopreq(L);
  L->tg_hint = tg;

  out.status = lj_vm_pcall_unwind(
    L, restorestack(L, argbase), 0+1, 0);
  /* No Lua or allocation is legal between protected return and publishing
  ** UNWINDING. From this point onward only exact owner-local cleanup remains. */
  L->tg_hint = tg;
  if (LJ_UNLIKELY(!lj_jit_event_callback_unwind_l(L, handle)))
    abort();
  if (LJ_UNLIKELY(out.status)) {
    /* Preserve legacy diagnostic behavior. STOPREQ observed while reporting
    ** is returned to the transaction owner and cannot throw until the rooted
    ** session and stream have both closed. */
    L->top--;
    out.actions = vmevent_report_failure(L);
  }
#if defined(LJ_GC2_TEST_HELPERS)
  if (jit_vmevent_call_test_hook) {
    LJJitVMEVENTCallTestHook hook = jit_vmevent_call_test_hook;
    void *ud = jit_vmevent_call_test_ud;
    jit_vmevent_call_test_hook = NULL;
    jit_vmevent_call_test_ud = NULL;
    hook(L, ud);
  }
#endif

  L->base = restorestack(L, oldbase);
  L->top = restorestack(L, oldtop);
  if (oldL)
    lj_tg_setcur_L(g, oldL);
  else
    lj_tg_clearcur_L(g);
  L->tg_hint = old_tg_hint;
  if (LJ_UNLIKELY(!lj_jit_event_callback_release_l(L, handle)))
    abort();

  *result = out;
  return 1;
}
#endif

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
#if LJ_HASJIT
  if (tg && (lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT))) {
    L->top = restorestack(L, oldtop);
    return;  /* Same-TG recursion into the legacy global callback path. */
  }
#endif
  if (tid == 0 || !vmevent_owner_cas(g, &expect, tid)) {
    L->top = restorestack(L, oldtop);
    return;
  }
  if (!hookmask_vmevent_enter(g)) {
    L->top = restorestack(L, oldtop);
    vmevent_owner_rel(g, tid);
    return;
  }
#if LJ_HASJIT
  /* Close the local-owner/global-owner observation race without waiting. */
  if (tg && (lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT))) {
    L->top = restorestack(L, oldtop);
    hookmask_vmevent_leave(g);
    vmevent_owner_rel(g, tid);
    return;
  }
#endif

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
