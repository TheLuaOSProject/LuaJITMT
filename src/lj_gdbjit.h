/*
** Client for the GDB JIT API.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GDBJIT_H
#define _LJ_GDBJIT_H

#include "lj_obj.h"
#include "lj_jit.h"

typedef struct GDBJITPrepared GDBJITPrepared;

#if LJ_HASJIT && defined(LUAJIT_USE_GDBJIT)

/* Optional debugger metadata is prepared before semantic trace publication.
** A NULL result means omission, never a recorder error. If T is J->curfinal,
** preparation reads the still-private J->cur image; exact_parent, when non-NULL,
** must still be the current parent-slot generation. Commit is a one-shot,
** nonallocating descriptor try-lock: success transfers ownership to T, while
** every failure leaves prep for lj_gdbjit_aborttrace() after token handoff.
** The caller must retain T and serialize its retirement through commit. */
LJ_FUNC GDBJITPrepared *lj_gdbjit_preparetrace(jit_State *J, GCtrace *T,
						const GCtrace *exact_parent);
LJ_FUNC int lj_gdbjit_committrace(GCtrace *T, GDBJITPrepared *prep);
LJ_FUNC void lj_gdbjit_aborttrace(global_State *g, GDBJITPrepared *prep);
LJ_FUNC void lj_gdbjit_addtrace(jit_State *J, GCtrace *T);
/* Returns zero if another Lua universe owns the process-global GDB descriptor.
** Callers must retain T and retry; deletion never waits for that owner. */
LJ_FUNC int lj_gdbjit_deltrace(jit_State *J, GCtrace *T);
LJ_FUNC void lj_gdbjit_deltrace_close(global_State *g, GCtrace *T);

#ifdef LJ_GDBJIT_TEST_HELPERS
typedef struct LJGDBJITTestStats {
  uint32_t prepare_attempts;
  uint32_t prepare_successes;
  uint32_t prepare_bounds_omits;
  uint32_t prepare_alloc_omits;
  uint32_t commit_attempts;
  uint32_t commit_successes;
  uint32_t commit_lock_omits;
  uint32_t aborts;
  uint32_t register_callbacks;
  uint32_t register_callbacks_ready;
} LJGDBJITTestStats;
LJ_FUNC void lj_gdbjit_test_reset(void);
LJ_FUNC void lj_gdbjit_test_force_prepare_alloc_omit(void);
LJ_FUNC void lj_gdbjit_test_stats(LJGDBJITTestStats *stats);
LJ_FUNC int lj_gdbjit_test_descriptor_lock_acquire(void);
LJ_FUNC void lj_gdbjit_test_descriptor_lock_release(void);
LJ_FUNC size_t lj_gdbjit_test_prepared_symfile_size(
  const GDBJITPrepared *prep);
LJ_FUNC size_t lj_gdbjit_test_prepared_object_size(
  const GDBJITPrepared *prep);
LJ_FUNC size_t lj_gdbjit_test_object_capacity(void);
#endif

#else
#define lj_gdbjit_preparetrace(J, T, P) \
  (UNUSED(J), UNUSED(T), UNUSED(P), (GDBJITPrepared *)NULL)
#define lj_gdbjit_committrace(T, P) \
  (UNUSED(T), UNUSED(P), 0)
#define lj_gdbjit_aborttrace(g, P) \
  (UNUSED(g), UNUSED(P))
#define lj_gdbjit_addtrace(J, T)	UNUSED(T)
#define lj_gdbjit_deltrace(J, T)	(UNUSED(J), UNUSED(T), 1)
#define lj_gdbjit_deltrace_close(g, T)	(UNUSED(g), UNUSED(T))
#endif

#endif
