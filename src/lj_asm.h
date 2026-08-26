/*
** IR assembler (SSA IR -> machine code).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ASM_H
#define _LJ_ASM_H

#include "lj_jit.h"

#if LJ_HASJIT
LJ_FUNC void lj_asm_trace(jit_State *J, GCtrace *T);
LJ_FUNC void lj_asm_patchexit(jit_State *J, GCtrace *T, ExitNo exitno,
			      MCode *target);
#if LJ_TARGET_ARM64 && defined(LJ_ARM64_EMIT_TEST_HELPERS)
typedef enum {
  LJ_ARM64_EMIT_TEST_GET_CUR_L,
  LJ_ARM64_EMIT_TEST_GET_JIT_BASE,
  LJ_ARM64_EMIT_TEST_SET_JIT_BASE,
  LJ_ARM64_EMIT_TEST_SETVMSTATE,
  LJ_ARM64_EMIT_TEST_SETVMSTATE_ROOT,
  LJ_ARM64_EMIT_TEST_GET_POLL,
  LJ_ARM64_EMIT_TEST_GET_PROFILE_REQUEST,
  LJ_ARM64_EMIT_TEST_GET_JIT_GATE
} LJArm64EmitTestOp;
LJ_FUNC MSize lj_asm_arm64_emit_test(jit_State *J, MCode *buf, MSize cap,
			     LJArm64EmitTestOp op, int32_t state);
#endif
#if LJ_TARGET_ARM64 && defined(LJ_ARM64_EXIT_TEST_HELPERS)
LJ_FUNC MSize lj_asm_arm64_exitstub_test(jit_State *J, MCode *buf, MSize cap,
					 TraceNo traceno, ExitNo nexits,
					 int indirect);
#endif
#endif

#endif
