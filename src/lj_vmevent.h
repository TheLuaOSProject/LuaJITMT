/*
** VM event handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_VMEVENT_H
#define _LJ_VMEVENT_H

#include "lj_obj.h"

/* Registry key for VM event handler table. */
#define LJ_VMEVENTS_REGKEY	"_VMEVENTS"
#define LJ_VMEVENTS_HSIZE	4

#define VMEVENT_MASK(ev)	((uint8_t)1 << ((int)(ev) & 7))
#define VMEVENT_HASH(ev) \
  ((int32_t)((uint32_t)(int32_t)(ev) & ~(uint32_t)7))
/* Preserve the stock signed 32-bit registry key bits without shifting a
** negative signed value or overflowing signed int. */
#define VMEVENT_HASHIDX(h)	((int32_t)((uint32_t)(h) << 3))
#define VMEVENT_NOCACHE		255

#define VMEVENT_DEF(name, hash) \
  LJ_VMEVENT_##name##_, \
  LJ_VMEVENT_##name = \
    (int32_t)(((uint32_t)LJ_VMEVENT_##name##_ & 7u) | \
	      ((uint32_t)(hash) << 3))

/* VM event IDs. */
typedef enum {
  VMEVENT_DEF(BC,	0x00003883),
  VMEVENT_DEF(TRACE,	0x12d91467),
  VMEVENT_DEF(RECORD,	0x1284bf4f),
  VMEVENT_DEF(TEXIT,	0x129df2b0),
  VMEVENT_DEF(ERRFIN,	0x12d93888),
  LJ_VMEVENT__MAX
} VMEvent;

/* Dormant universe-global jit.attach() publication-clock substrate.  The
** snapshot API is deliberately available in no-JIT builds, where it returns
** RETRY, so internal users can remain fail-closed across configurations. */
typedef struct LJJitEventAttachmentSnapshot {
  uint64_t sequence;
  uint64_t next_generation;
  uint64_t generation;
} LJJitEventAttachmentSnapshot;

#define LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_RETRY		(-1)
#define LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL	0
#define LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED	1
#define LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE		UINT32_MAX

/* A claimed writer has made one authoritative lane odd. There is
** intentionally no cancel operation: after claim, the caller performs only
** its bounded external semantic CAS and writer_publish(). publish first
** invalidates the VM-event cache, then exposes the reserved generation and
** finally restores the exact lane sequence to even. Invalid handle use is a
** fail-stop invariant violation, never a recoverable path which could strand
** an odd lane. publish rederives the authoritative main-TG clock and
** fail-stops if it no longer exactly names this handle. */
typedef struct LJJitEventAttachmentWriter {
  global_State *g;
  uint64_t sequence;
  uint64_t generation;
  uint32_t slot;
  uint32_t claimed;
} LJJitEventAttachmentWriter;

#define LJ_JIT_EVENT_ATTACHMENT_WRITER_CORRUPT		(-2)
#define LJ_JIT_EVENT_ATTACHMENT_WRITER_EXHAUSTED	(-1)
#define LJ_JIT_EVENT_ATTACHMENT_WRITER_BUSY		0
#define LJ_JIT_EVENT_ATTACHMENT_WRITER_CLAIMED		1

LJ_FUNC int lj_jit_event_attachment_snapshot(
  global_State *g, uint32_t slot, LJJitEventAttachmentSnapshot *snapshot);
LJ_FUNC int lj_jit_event_attachment_clock_slot(int32_t registry_key,
					       uint32_t *slot);
LJ_FUNC int lj_jit_event_attachment_writer_claim(
  global_State *g, uint32_t slot, LJJitEventAttachmentWriter *writer);
LJ_FUNC void lj_jit_event_attachment_writer_publish(
  LJJitEventAttachmentWriter *writer);

/* One bounded VM-event handler observation. READY leaves exactly the handler
** and optional FR2 slot on |L| and reports the saved argument base. ABSENT
** and RETRY restore the exact entry top. Clocked builds accept INITIAL as a
** real state because luaL_newstate() installs ERRFIN before any jit.attach()
** writer publication. Runtime jit.off remains clocked; only a compile-time
** no-JIT build reports UNCLOCKED. */
typedef struct LJVMEVENTPrepareResult {
  ptrdiff_t argbase;
  LJJitEventAttachmentSnapshot attachment;
  uint32_t slot;
  uint32_t attachment_state;
} LJVMEVENTPrepareResult;

enum {
  LJ_VMEVENT_PREPARE_RETRY = -1,
  LJ_VMEVENT_PREPARE_ABSENT = 0,
  LJ_VMEVENT_PREPARE_READY = 1
};

enum {
  LJ_VMEVENT_ATTACHMENT_INVALID = 0,
  LJ_VMEVENT_ATTACHMENT_INITIAL = 1,
  LJ_VMEVENT_ATTACHMENT_PUBLISHED = 2,
  LJ_VMEVENT_ATTACHMENT_UNCLOCKED = 3
};

/* Canonical immutable session identity available in every build profile.
** INITIAL and compile-time UNCLOCKED have no publication generation;
** PUBLISHED always names one nonzero clock generation. */
static LJ_AINLINE int lj_vmevent_attachment_identity_valid(
  uint32_t state, uint64_t generation)
{
  return state == LJ_VMEVENT_ATTACHMENT_PUBLISHED ? generation != 0 :
    (state == LJ_VMEVENT_ATTACHMENT_INITIAL ||
     state == LJ_VMEVENT_ATTACHMENT_UNCLOCKED) ? generation == 0 : 0;
}

LJ_FUNC void lj_vmevent_init(lua_State *L);
LJ_FUNC int lj_vmevent_prepare_try(lua_State *L, VMEvent ev,
				    LJVMEVENTPrepareResult *result);
LJ_FUNC ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev);

enum {
  LJ_VMEVENT_TEST_AFTER_CLOCK_A = 1,
  LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP = 2,
  LJ_VMEVENT_TEST_AFTER_EVENT_LOOKUP = 3,
  LJ_VMEVENT_TEST_BEFORE_MASK_CLEAR = 4,
  LJ_VMEVENT_TEST_AFTER_MASK_CLEAR = 5
};
#if defined(LJ_GC2_TEST_HELPERS)
typedef void (*LJVMEVENTPrepareTestHook)(lua_State *L, VMEvent ev,
					 int stage, void *ud);
LJ_FUNC void lj_vmevent_test_set_prepare_hook(
  LJVMEVENTPrepareTestHook hook, void *ud);
#endif

#ifdef LUAJIT_DISABLE_VMEVENT
#define lj_vmevent_send(g, ev, args)		UNUSED(g)
#define lj_vmevent_send_(g, ev, args, post)	UNUSED(g)
#define lj_vmevent_send_l(L, ev, args)		UNUSED(L)
#define lj_vmevent_send_l_(L, ev, args, post)	UNUSED(L)
#else
#define lj_vmevent_send_l(L, ev, args) \
  do { \
    lua_State *V = (L); \
    global_State *vmevg = V ? G(V) : NULL; \
    if (vmevg && (vmevmask_load_acq(vmevg) & \
		  VMEVENT_MASK(LJ_VMEVENT_##ev))) { \
      ptrdiff_t vmevtop = savestack(V, V->top); \
      ptrdiff_t argbase = lj_vmevent_prepare(V, LJ_VMEVENT_##ev); \
      if (argbase) { \
	args \
	lj_vmevent_call(V, argbase, vmevtop); \
      } \
    } \
  } while (0)
#define lj_vmevent_send_l_(L, ev, args, post) \
  do { \
    lua_State *V = (L); \
    global_State *vmevg = V ? G(V) : NULL; \
    if (vmevg && (vmevmask_load_acq(vmevg) & \
		  VMEVENT_MASK(LJ_VMEVENT_##ev))) { \
      ptrdiff_t vmevtop = savestack(V, V->top); \
      ptrdiff_t argbase = lj_vmevent_prepare(V, LJ_VMEVENT_##ev); \
      if (argbase) { \
	args \
	lj_vmevent_call(V, argbase, vmevtop); \
	post \
      } \
    } \
  } while (0)
#define lj_vmevent_send(g, ev, args) \
  do { \
    lua_State *vmevL = lj_vmevent_state((g)); \
    if (vmevL) lj_vmevent_send_l(vmevL, ev, args); \
  } while (0)
#define lj_vmevent_send_(g, ev, args, post) \
  do { \
    lua_State *vmevL = lj_vmevent_state((g)); \
    if (vmevL) lj_vmevent_send_l_(vmevL, ev, args, post); \
  } while (0)

LJ_FUNC lua_State *lj_vmevent_state(global_State *g);
LJ_FUNC void lj_vmevent_call(lua_State *L, ptrdiff_t argbase,
			     ptrdiff_t oldtop);
#endif

#endif
