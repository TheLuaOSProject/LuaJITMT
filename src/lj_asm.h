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
#if LJ_TARGET_ARM64
/* Fail-closed admission result for the first ARM64 native trace shape. */
typedef enum {
  LJ_ARM64_IR_REJECT_NONE,
  LJ_ARM64_IR_REJECT_TRACE,
  LJ_ARM64_IR_REJECT_SINK,
  LJ_ARM64_IR_REJECT_CONSTANT,
  LJ_ARM64_IR_REJECT_OPCODE,
  LJ_ARM64_IR_REJECT_TYPE,
  LJ_ARM64_IR_REJECT_OPERAND,
  LJ_ARM64_IR_REJECT_CALL,
  LJ_ARM64_IR_REJECT_XPOLL,
  LJ_ARM64_IR_REJECT_SNAPSHOT
} LJArm64IRRejectReason;

#define LJ_ARM64_IR_CALL_NONE	((uint16_t)0xffffu)

typedef struct LJArm64IRReject {
  LJArm64IRRejectReason reason;
  IRRef ref;
  IROp op;
  uint16_t detail;  /* CALL helper ID, mode bits or structural discriminator. */
} LJArm64IRReject;

LJ_FUNC int lj_asm_arm64_ir_admit(const jit_State *J, const GCtrace *T,
				   LJArm64IRReject *reject);
#endif
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
