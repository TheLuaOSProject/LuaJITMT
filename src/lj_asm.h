/*
** IR assembler (SSA IR -> machine code).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ASM_H
#define _LJ_ASM_H

#include "lj_jit.h"

typedef struct CTState CTState;

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
  CTState *cts; /* Read-only CType snapshots for independent FFI validation. */
  const SnapShot *snap;
  const SnapEntry *snapmap;
  const BCIns *proto_bc;
  const GCproto *proto;
  const BCIns *startpc;
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
  uint8_t owner_private;
} LJArm64PostRAView;

/* Pure immutable view for the bounded ARM64 side-trace grammars. This is
** intentionally separate from GCtrace/jit_State so the policy can be tested
** before any recorder or publication path is opened. */
typedef struct LJArm64SideParentBinding {
  IRIns value;
  IRRef ref;
  uint16_t regsp;
  uint16_t slot;
} LJArm64SideParentBinding;

/* Bounded source evidence for one value in the selected parent restart map.
** It is copied under SMR so both independent child validators can verify
** constant-pool starts and dynamic-ref ordering without retaining parent IR. */
typedef struct LJArm64SideParentMapValue {
  IRIns value;
  IRRef ref;
  uint16_t slot;
  uint8_t ref_start;
} LJArm64SideParentMapValue;

typedef struct LJArm64SideIRView {
  const IRIns *ir;
  const SnapShot *snap;
  const SnapEntry *snapmap;
  const BCIns *proto_bc;
  IRRef nins;
  IRRef nk;
  MSize nsnap;
  MSize nsnapmap;
  /* The selected parent exit is copied while its body is under SMR. The
  ** child validator never follows a parent snapshot pointer after capture. */
  const SnapShot *parent_snap;
  const SnapEntry *parent_snapmap;
  const LJArm64SideParentMapValue *parent_map_values;
  const LJArm64SideParentBinding *parent_bindings;
  const uint16_t *parent_regspmap;
  IRRef parent_nins;
  IRRef parent_nk;
  MSize parent_nsnap;
  MSize parent_nsnapmap;
  MSize parent_nmap_values;
  MSize parent_nregspmap;
  MSize parent_nslots;
  MSize parent_topslot;
  const BCIns *parent_continuation;
  /* The root loop can be headed by BC_LOOP or bounded by a paired FORI/FORL. */
  BCIns parent_startins;
  MSize loop_header_pc;
  MSize loop_start_pc;
  MSize loop_end_pc;
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

typedef struct LJArm64SideParentSnapshot {
  SnapShot snap;
  SnapEntry map[LJ_MAX_JSLOTS+1+LJ_FR2];
  LJArm64SideParentMapValue map_values[LJ_MAX_JSLOTS];
  uint16_t regspmap[LJ_MAX_JSLOTS];
  LJArm64SideParentBinding bindings[LJ_MAX_JSLOTS];
  MSize nmap_values;
  IRRef parent_nins;
  IRRef parent_nk;
  MSize parent_nsnap;
  MSize nsnapmap;
  MSize nregspmap;
  MSize nslots;
  MSize topslot;
  const BCIns *continuation;
  /* Root-loop source and bounded executable body interval. */
  BCIns startins;
  MSize loop_header_pc;
  MSize loop_start_pc;
  MSize loop_end_pc;
} LJArm64SideParentSnapshot;

/* Immutable view used by the independent first-side post-RA validator. */
typedef struct LJArm64SidePostRAView {
  LJArm64SideIRView semantic;
  const GCtrace *owner;
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
  MSize loop_mcofs;
  uint8_t branch_track;
} LJArm64SidePostRAView;

LJ_FUNC int lj_asm_arm64_ir_admit(const jit_State *J, const GCtrace *T,
				   LJArm64IRReject *reject);
LJ_FUNC int lj_asm_arm64_postra_admit(const LJArm64PostRAView *view,
				      IRRef *semantic_ninsp);
#if defined(LJ_TRACE_TEST_HELPERS)
LJ_FUNC int lj_asm_arm64_test_string_byte_dynamic_index_ir(
						const GCtrace *T, IRRef conv);
LJ_FUNC int lj_asm_arm64_test_string_byte_dynamic_index_postra(
						const LJArm64PostRAView *view, IRRef conv);
LJ_FUNC int lj_asm_arm64_test_string_byte_dynamic_guard_ir(
						const GCtrace *T, IRRef guard);
LJ_FUNC int lj_asm_arm64_test_string_byte_dynamic_guard_postra(
						const LJArm64PostRAView *view, IRRef guard);
#endif
LJ_FUNC int lj_asm_arm64_postra_funcf_entry_admit(
	const LJArm64PostRAView *view, BCIns liveins,
	IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_funcf_bytecode(const GCproto *pt, BCIns startins,
	BCIns liveins, int prepublication);
LJ_FUNC int lj_asm_arm64_side_scalar_ir_admit(const jit_State *J,
	const GCtrace *child, const LJArm64SideIRView *view,
	LJArm64IRReject *reject);
LJ_FUNC int lj_asm_arm64_side_scalar_postra_admit(
	const LJArm64PostRAView *child, const LJArm64SideIRView *view,
	IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_side_prehead_admit(
	const LJArm64SidePostRAView *view, IRRef *semantic_ninsp);
LJ_FUNC int lj_asm_arm64_side_postra_admit(
	const LJArm64SidePostRAView *view, IRRef *semantic_ninsp);
#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC int lj_asm_arm64_test_scalar_math_live_across(
	const LJArm64PostRAView *view, IRRef semantic_nins, IRRef loop, IRRef call);
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
LJ_FUNC MCode *lj_asm_arm64_tbar_test(jit_State *J, MCode *buf, MSize cap,
	GCtab *tab, cTValue *key, int dynamic);
LJ_FUNC MCode *lj_asm_arm64_phi_spill_test(jit_State *J, MCode *buf,
	MSize cap);
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
