/*
** FFI C call handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CCALL_H
#define _LJ_CCALL_H

#include "lj_obj.h"
#include "lj_ctype.h"

#if LJ_HASFFI

/* -- C calling conventions ----------------------------------------------- */

#if LJ_TARGET_X86ORX64

#if LJ_TARGET_X86
#define CCALL_NARG_GPR		2	/* For fastcall arguments. */
#define CCALL_NARG_FPR		0
#define CCALL_NRET_GPR		2
#define CCALL_NRET_FPR		1	/* For FP results on x87 stack. */
#define CCALL_ALIGN_STACKARG	0	/* Don't align argument on stack. */
#elif LJ_ABI_WIN
#define CCALL_NARG_GPR		4
#define CCALL_NARG_FPR		4
#define CCALL_NRET_GPR		1
#define CCALL_NRET_FPR		1
#define CCALL_SPS_EXTRA		4
#else
#define CCALL_NARG_GPR		6
#define CCALL_NARG_FPR		8
#define CCALL_NRET_GPR		2
#define CCALL_NRET_FPR		2
#define CCALL_VECTOR_REG	1	/* Pass vectors in registers. */
#endif

#define CCALL_SPS_FREE		1
#define CCALL_ALIGN_CALLSTATE	16

typedef LJ_ALIGN(16) union FPRArg {
  double d[2];
  float f[4];
  uint8_t b[16];
  uint16_t s[8];
  int i[4];
  int64_t l[2];
} FPRArg;

typedef intptr_t GPRArg;

#elif LJ_TARGET_ARM

#define CCALL_NARG_GPR		4
#define CCALL_NRET_GPR		2	/* For softfp double. */
#if LJ_ABI_SOFTFP
#define CCALL_NARG_FPR		0
#define CCALL_NRET_FPR		0
#else
#define CCALL_NARG_FPR		8
#define CCALL_NRET_FPR		4
#endif
#define CCALL_SPS_FREE		0

typedef intptr_t GPRArg;
typedef union FPRArg {
  double d;
  float f[2];
} FPRArg;

#elif LJ_TARGET_ARM64

#define CCALL_NARG_GPR		8
#define CCALL_NRET_GPR		2
#define CCALL_NARG_FPR		8
#define CCALL_NRET_FPR		4
#define CCALL_SPS_FREE		0
#if LJ_TARGET_OSX
#define CCALL_PACK_STACKARG	1
#endif

typedef intptr_t GPRArg;
typedef union FPRArg {
  double d;
  struct { LJ_ENDIAN_LOHI(float f; , float g;) };
  struct { LJ_ENDIAN_LOHI(uint32_t lo; , uint32_t hi;) };
} FPRArg;

#elif LJ_TARGET_PPC

#define CCALL_NARG_GPR		8
#define CCALL_NARG_FPR		(LJ_ABI_SOFTFP ? 0 : 8)
#define CCALL_NRET_GPR		4	/* For complex double. */
#define CCALL_NRET_FPR		(LJ_ABI_SOFTFP ? 0 : 1)
#define CCALL_SPS_EXTRA		4
#define CCALL_SPS_FREE		0

typedef intptr_t GPRArg;
typedef double FPRArg;

#elif LJ_TARGET_MIPS32

#define CCALL_NARG_GPR		4
#define CCALL_NARG_FPR		(LJ_ABI_SOFTFP ? 0 : 2)
#define CCALL_NRET_GPR		(LJ_ABI_SOFTFP ? 4 : 2)
#define CCALL_NRET_FPR		(LJ_ABI_SOFTFP ? 0 : 2)
#define CCALL_SPS_EXTRA		7
#define CCALL_SPS_FREE		1

typedef intptr_t GPRArg;
typedef union FPRArg {
  double d;
  struct { LJ_ENDIAN_LOHI(float f; , float g;) };
} FPRArg;

#elif LJ_TARGET_MIPS64

/* FP args are positional and overlay the GPR array. */
#define CCALL_NARG_GPR		8
#define CCALL_NARG_FPR		0
#define CCALL_NRET_GPR		2
#define CCALL_NRET_FPR		(LJ_ABI_SOFTFP ? 0 : 2)
#define CCALL_SPS_EXTRA		3
#define CCALL_SPS_FREE		1

typedef intptr_t GPRArg;
typedef union FPRArg {
  double d;
  struct { LJ_ENDIAN_LOHI(float f; , float g;) };
} FPRArg;

#else
#error "Missing calling convention definitions for this architecture"
#endif

#ifndef CCALL_SPS_EXTRA
#define CCALL_SPS_EXTRA		0
#endif
#ifndef CCALL_VECTOR_REG
#define CCALL_VECTOR_REG	0
#endif
#ifndef CCALL_ALIGN_STACKARG
#define CCALL_ALIGN_STACKARG	1
#endif
#ifndef CCALL_PACK_STACKARG
#define CCALL_PACK_STACKARG	0
#endif
#ifndef CCALL_ALIGN_CALLSTATE
#define CCALL_ALIGN_CALLSTATE	8
#endif

#define CCALL_NUM_GPR \
  (CCALL_NARG_GPR > CCALL_NRET_GPR ? CCALL_NARG_GPR : CCALL_NRET_GPR)
#define CCALL_NUM_FPR \
  (CCALL_NARG_FPR > CCALL_NRET_FPR ? CCALL_NARG_FPR : CCALL_NRET_FPR)

/* Check against constants in lj_ctype.h. */
LJ_STATIC_ASSERT(CCALL_NUM_GPR <= CCALL_MAX_GPR);
LJ_STATIC_ASSERT(CCALL_NUM_FPR <= CCALL_MAX_FPR);

#define CCALL_NUM_STACK		31
#define CCALL_SIZE_STACK	(CCALL_NUM_STACK * CTSIZE_PTR)

/* -- C call state -------------------------------------------------------- */

typedef LJ_ALIGN(CCALL_ALIGN_CALLSTATE) struct CCallState {
  void (*func)(void);		/* Pointer to called function. */
  uint32_t spadj;		/* Stack pointer adjustment. */
  uint8_t nsp;			/* Number of bytes on stack. */
  uint8_t retref;		/* Return value by reference. */
#if LJ_TARGET_X64
  uint8_t ngpr;			/* Number of arguments in GPRs. */
  uint8_t nfpr;			/* Number of arguments in FPRs. */
#elif LJ_TARGET_X86
  uint8_t resx87;		/* Result on x87 stack: 1:float, 2:double. */
#elif LJ_TARGET_ARM64
  void *retp;			/* Aggregate return pointer in x8. */
#elif LJ_TARGET_PPC
  uint8_t nfpr;			/* Number of arguments in FPRs. */
#endif
#if LJ_32
  int32_t align1;
#endif
#if CCALL_NUM_FPR
  FPRArg fpr[CCALL_NUM_FPR];	/* Arguments/results in FPRs. */
#endif
  GPRArg gpr[CCALL_NUM_GPR];	/* Arguments/results in GPRs. */
  GPRArg stack[CCALL_NUM_STACK];	/* Stack slots. */
} CCallState;

/* Native-state bookkeeping around an FFI C call. */
typedef struct CCallNativeState {
  TGState *tg;
  CCallbackRuntime *cb;
  void *old_ffi_call_func;
  MSize old_callback_slot;
  uint8_t old_native_had_stopreq;
  int had_stopreq;
} CCallNativeState;

#define LJ_CCALL_JIT_SIG0		0u
#define LJ_CCALL_JIT_SIG_I32		1u
#define LJ_CCALL_JIT_SIG_PTR		2u
#define LJ_CCALL_JIT_SIG_I32_I32	3u
#define LJ_CCALL_JIT_SIG_I32_PTR	4u
#define LJ_CCALL_JIT_SIG_PTR_I32	5u
#define LJ_CCALL_JIT_SIG_PTR_PTR	6u

#define LJ_CCALL_JIT_NUM_SIG0		0u
#define LJ_CCALL_JIT_NUM_SIG_NUM	1u
#define LJ_CCALL_JIT_NUM_SIG_NUM_NUM	2u

#define LJ_CCALL_JIT_NARROW_I8		0u
#define LJ_CCALL_JIT_NARROW_U8		1u
#define LJ_CCALL_JIT_NARROW_I16		2u
#define LJ_CCALL_JIT_NARROW_U16		3u

/* -- C call handling ----------------------------------------------------- */

/* Really belongs to lj_vm.h. */
LJ_ASMF void LJ_FASTCALL lj_vm_ffi_call(CCallState *cc);

LJ_FUNC CTypeID lj_ccall_ctid_vararg(lua_State *L, CTState *cts, cTValue *o);
LJ_FUNC void lj_ccall_native_save(lua_State *L, CCallNativeState *st);
LJ_FUNC void lj_ccall_native_enter(lua_State *L, CCallNativeState *st,
				   void *func);
LJ_FUNC uint32_t lj_ccall_native_leave(lua_State *L, CTState *cts,
				       CCallNativeState *st, void *func);
LJ_FUNC void lj_ccall_native_checkstop(lua_State *L, uint32_t actions,
				       const CCallNativeState *st);
LJ_FUNC void lj_ccall_jit_void_gpr(lua_State *L, void *func,
				   uintptr_t a, uintptr_t b, uint32_t sig);
LJ_FUNC int32_t lj_ccall_jit_i32_gpr(lua_State *L, void *func,
				     uintptr_t a, uintptr_t b, uint32_t sig);
LJ_FUNC int32_t lj_ccall_jit_narrow_0(lua_State *L, void *func,
				      uint32_t sig);
LJ_FUNC double lj_ccall_jit_u32_gpr(lua_State *L, void *func,
				    uintptr_t a, uintptr_t b, uint32_t sig);
LJ_FUNC double lj_ccall_jit_u32_0(lua_State *L, void *func);
LJ_FUNC uint64_t lj_ccall_jit_u64_0(lua_State *L, void *func);
LJ_FUNC int64_t lj_ccall_jit_i64_gpr(lua_State *L, void *func,
				     int64_t a, int64_t b, uint32_t sig);
LJ_FUNC int64_t lj_ccall_jit_i64_ret_gpr(lua_State *L, void *func,
					 uintptr_t a, uintptr_t b,
					 uint32_t sig);
LJ_FUNC uint64_t lj_ccall_jit_u64_gpr(lua_State *L, void *func,
				      uintptr_t a, uintptr_t b, uint32_t sig);
LJ_FUNC void *lj_ccall_jit_ptr_gpr(lua_State *L, void *func,
				   uintptr_t a, uintptr_t b, uint32_t sig);
LJ_FUNC double lj_ccall_jit_num_fpr(lua_State *L, void *func,
				    double a, double b, uint32_t sig);
LJ_FUNC float lj_ccall_jit_flt_fpr(lua_State *L, void *func,
				   float a, float b, uint32_t sig);
LJ_FUNC int lj_ccall_func(lua_State *L, GCcdata *cd);

#endif

#endif
