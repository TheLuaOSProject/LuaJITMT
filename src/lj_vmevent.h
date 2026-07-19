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
#define VMEVENT_HASH(ev)	((int)(ev) & ~7)
#define VMEVENT_HASHIDX(h)	((int)(h) << 3)
#define VMEVENT_NOCACHE		255

#define VMEVENT_DEF(name, hash) \
  LJ_VMEVENT_##name##_, \
  LJ_VMEVENT_##name = ((LJ_VMEVENT_##name##_) & 7)|((hash) << 3)

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

LJ_FUNC int lj_jit_event_attachment_snapshot(
  global_State *g, uint32_t slot, LJJitEventAttachmentSnapshot *snapshot);

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

LJ_FUNC ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev);
LJ_FUNC lua_State *lj_vmevent_state(global_State *g);
LJ_FUNC void lj_vmevent_call(lua_State *L, ptrdiff_t argbase,
			     ptrdiff_t oldtop);
#endif

#endif
