/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PROFILE_H
#define _LJ_PROFILE_H

#include "lj_obj.h"

#if LJ_HASPROFILE

/* ARM64 stays on the legacy signal path until LJ_THR_TG_SIGNAL_CACHE has a
** verified pthread_t/generated-code contract there. VM request loads alone
** are not sufficient to make signal publication safe. */
#define LJ_PROFILE_TGLOCAL	(LJ_PROFILE_SIGPROF && LJ_TARGET_X86ORX64)

LJ_FUNC void LJ_FASTCALL lj_profile_interpreter(lua_State *L);
LJ_FUNC void LJ_FASTCALL lj_profile_owner_poll(lua_State *L);
LJ_FUNC int lj_profile_pending(lua_State *L);
LJ_FUNC int lj_profile_active(lua_State *L);
LJ_FUNC int lj_profile_callback_active_tg(TGState *tg);
LJ_FUNC int lj_profile_poll_required(global_State *g);
LJ_FUNC uint32_t lj_profile_stop_hs(lua_State *L);
#if LJ_PROFILE_SIGPROF && defined(LJ_PROFILE_TIMER_TEST_HELPERS)
LJ_FUNC void lj_profile_timer_test_reset(void);
LJ_FUNC void lj_profile_timer_test_fail_sigaction(uint32_t nth);
LJ_FUNC void lj_profile_timer_test_fail_setitimer(uint32_t nth);
LJ_FUNC void lj_profile_timer_test_fail_image_pin(uint32_t nth);
LJ_FUNC void lj_profile_timer_test_fail_image_match(uint32_t nth);
LJ_FUNC void lj_profile_timer_test_fail_trace_flush(uint32_t nth);
LJ_FUNC uint32_t lj_profile_timer_test_sigaction_calls(void);
LJ_FUNC uint32_t lj_profile_timer_test_setitimer_calls(void);
LJ_FUNC uint32_t lj_profile_timer_test_handler_installed(void);
LJ_FUNC uint32_t lj_profile_timer_test_image_pinned(void);
LJ_FUNC uint32_t lj_profile_timer_test_signal_handlers(void);
LJ_FUNC void lj_profile_timer_test_force_signal_handlers(uint32_t handlers);
LJ_FUNC void lj_profile_timer_test_force_atfork_building(void);
LJ_FUNC void lj_profile_timer_test_force_process(uintptr_t process);
LJ_FUNC void lj_profile_timer_test_pause_signal(uint32_t pause);
LJ_FUNC uint32_t lj_profile_timer_test_signal_entered(void);
LJ_FUNC uint32_t lj_profile_timer_test_drain_waits(void);
LJ_FUNC void lj_profile_timer_test_pause_before_arm(uint32_t pause);
LJ_FUNC uint32_t lj_profile_timer_test_before_arm_entered(void);
#endif
#if !LJ_PROFILE_SIGPROF
LJ_FUNC void LJ_FASTCALL lj_profile_hook_enter(global_State *g);
LJ_FUNC void LJ_FASTCALL lj_profile_hook_leave(global_State *g);
#endif

#else

#define LJ_PROFILE_TGLOCAL	0

#endif

#endif
