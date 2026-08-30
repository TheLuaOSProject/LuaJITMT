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
/* Encode one host-order ARM64 unconditional B instruction. The source and
** target are raw code addresses; no C pointer subtraction is performed. */
LJ_FUNC int lj_asm_arm64_b26_encode(uintptr_t source, uintptr_t target,
	MCode *insp);

/* Fail-closed admission result for bounded ARM64 native trace grammars. */
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

/* Immutable post-register-allocation data needed by both the assembler and
** the ARM64 root-entry gate. */
typedef struct LJArm64PostRAView {
  const IRIns *ir;
  const GCtrace *owner;
  const SnapShot *snap;
  const SnapEntry *snapmap;
  const BCIns *proto_bc;
  IRRef nins;
  IRRef nk;
  MSize nsnap;
  MSize nsnapmap;
  MSize spadjust;
  MSize proto_sizebc;
  MSize proto_numparams;
  MSize root_topslot;
  BCIns startins;
  uint8_t base_delta;
} LJArm64PostRAView;

/* Pure immutable view for the bounded ARM64 side-trace grammars. This is
** intentionally separate from GCtrace/jit_State so the policy can be tested
** before any recorder or publication path is opened. */
typedef struct LJArm64SideIRView {
  const IRIns *ir;
  const SnapShot *snap;
  const SnapEntry *snapmap;
  const BCIns *proto_bc;
  IRRef nins;
  IRRef nk;
  MSize nsnap;
  MSize nsnapmap;
  MSize proto_sizebc;
  MSize baseslot;
  MSize root_topslot;
  TraceNo traceno;
  TraceNo parent;
  TraceNo root;
  TraceNo link;
  ExitNo exitno;
  BCIns startins;
  TraceLink linktype;
  uint8_t sinktags;
  uint8_t base_delta;
} LJArm64SideIRView;

enum { LJ_ARM64_SIDE_CHILD_NSNAP = 5 };

/* Exact root/child bytecode geometry for an admitted first-side shape. */
typedef struct LJArm64SideShape {
  ExitNo exitno;
  MSize parent_nsnap;
  MSize continuation_pc;
  MSize child_pcpos[LJ_ARM64_SIDE_CHILD_NSNAP];
  uint32_t inherited_reg;
  uint32_t sload_reg;
  int32_t addends[2];  /* Repeat addends[0] for a singleton exact set. */
} LJArm64SideShape;

/* Exact immutable view of the repeatedly observed first-side ARM64 allocator
** layout. Production dispatch remains separate and fail-closed. */
typedef struct LJArm64SidePostRAView {
  LJArm64SideIRView semantic;
  const uint16_t *parentmap;
  const MCode *entry;
  IRRef nins;
  IRRef stopins;
  IRRef orignins;
  MSize spadjust;
  MSize parent_spadjust;
  MSize topslot;
  MSize parent_topslot;
  MSize parentmap_n;
  MSize entry_words;
  uint8_t branch_track;
} LJArm64SidePostRAView;

LJ_FUNC int lj_asm_arm64_ir_admit(const jit_State *J, const GCtrace *T,
				   LJArm64IRReject *reject);
LJ_FUNC int lj_asm_arm64_postra_admit(const LJArm64PostRAView *view,
				      IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_postra_funcf_entry_admit(
	const LJArm64PostRAView *view, BCIns liveins,
	IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_side_ir_admit(const LJArm64SideIRView *view,
	LJArm64IRReject *reject);
LJ_FUNC const LJArm64SideShape *lj_asm_arm64_side_shape(ExitNo exitno);
LJ_FUNC int lj_asm_arm64_side_prehead_admit(
	const LJArm64SidePostRAView *view, IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_side_postra_admit(
	const LJArm64SidePostRAView *view, IRRef *semantic_ninsp);
#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC void lj_asm_arm64_test_force_exitstub_mcode_retry(uint32_t count);
#ifdef LJ_ARM64_SIDE_ASM_TEST
enum {
  LJ_ARM64_SIDE_ASM_PROBE_CAPTURE = 0x01u,
  LJ_ARM64_SIDE_ASM_PROBE_PARENTMAP = 0x02u,
  LJ_ARM64_SIDE_ASM_PROBE_PREHEAD = 0x04u,
  LJ_ARM64_SIDE_ASM_PROBE_POSTRA = 0x08u,
  LJ_ARM64_SIDE_ASM_PROBE_TAIL = 0x10u,
  LJ_ARM64_SIDE_ASM_PROBE_FINAL = 0x20u,
  LJ_ARM64_SIDE_ASM_PROBE_MARKER = 0x40u,
  LJ_ARM64_SIDE_ASM_PROBE_SEAL = 0x80u,
  LJ_ARM64_SIDE_ASM_PROBE_COMPACT = 0x100u,
  LJ_ARM64_SIDE_ASM_PROBE_ALL = 0x1ffu
};
typedef struct LJArm64SideAsmProbe {
  uint32_t stages;
  uint32_t capture_count;
  uint32_t compact_geometry_reject;
  uint32_t compact_init;
  uint32_t compact_reset;
  uint32_t compact_pauth;
  uint32_t seal_failure;
  uint32_t raw_negative;
  TraceNo parent;
  TraceNo child;
  ExitNo exitno;
  MSize parentmap_n;
  MSize entry_words;
  uint16_t parentmap0;
  uint8_t branch_track;
  uint8_t marker;
  MCode entry[2];
  TraceVec *cert_tracev;
  GCtrace *cert_body;
  MCode *cert_mcode;
  const BCIns *cert_continuation;
  BCIns cert_continuationins;
  TraceNo cert_child;
  MCode *tail_target;
  MCode *tail_pc;
  MCode tail_ins;
} LJArm64SideAsmProbe;
LJ_FUNC void lj_asm_arm64_test_side_probe_arm(TraceNo parent, ExitNo exitno);
LJ_FUNC int lj_asm_arm64_test_side_probe_ingress(TraceNo parent,
	ExitNo exitno);
LJ_FUNC int lj_asm_arm64_test_side_probe_active(TraceNo parent,
	ExitNo exitno);
LJ_FUNC int lj_asm_arm64_test_side_probe_read(LJArm64SideAsmProbe *out);
#endif
#endif
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
  LJ_ARM64_EMIT_TEST_GET_JIT_GATE,
  LJ_ARM64_EMIT_TEST_GET_GC_CADENCE,
  LJ_ARM64_EMIT_TEST_SET_XSAVE_BASESLOT,
  LJ_ARM64_EMIT_TEST_SET_XSAVE_NSLOTS
} LJArm64EmitTestOp;
LJ_FUNC MSize lj_asm_arm64_emit_test(jit_State *J, MCode *buf, MSize cap,
			     LJArm64EmitTestOp op, int32_t state);
#endif
#if LJ_TARGET_ARM64 && defined(LJ_ARM64_EXIT_TEST_HELPERS)
LJ_FUNC int lj_asm_arm64_exitstub_layout_test(uintptr_t mctop,
	ExitNo nexits, MSize *needp);
LJ_FUNC MSize lj_asm_arm64_exitstub_test(jit_State *J, MCode *buf, MSize cap,
					 TraceNo traceno, ExitNo nexits,
					 MCode **slots);
#endif
#endif

#endif
