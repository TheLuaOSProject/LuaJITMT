/*
** Bounded Darwin ARM64 scalar FFI ABI classifier shared by recorder and
** independent trace admission. Keep the supported CType surface explicit.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CCALL_ARM64_H
#define _LJ_CCALL_ARM64_H

#include "lj_ccall.h"
#include <string.h>

#if LJ_TARGET_ARM64 && LJ_TARGET_OSX && LJ_HASFFI

typedef enum LJArm64ABIClass {
  LJ_ARM64_ABI_NONE,
  LJ_ARM64_ABI_VOID,
  LJ_ARM64_ABI_GPR,
  LJ_ARM64_ABI_FPR
} LJArm64ABIClass;

typedef enum LJArm64ABIKind {
  LJ_ARM64_ABI_KIND_NONE,
  LJ_ARM64_ABI_KIND_VOID,
  LJ_ARM64_ABI_KIND_INTEGER,
  LJ_ARM64_ABI_KIND_FLOAT,
  LJ_ARM64_ABI_KIND_POINTER,
  LJ_ARM64_ABI_KIND_ENUM
} LJArm64ABIKind;

#define LJ_ARM64_ABI_MAX_ARGS 32
#define LJ_ARM64_ABI_STACK 255

typedef struct LJArm64ABIArg {
  CTypeID id;
  uint8_t cls;
  uint8_t kind;
  uint8_t reg;
  uint8_t size;
  uint8_t align;
  uint8_t is_unsigned;
  uint16_t stack_offset;
} LJArm64ABIArg;

typedef struct LJArm64ABISignature {
  LJArm64ABIArg arg[LJ_ARM64_ABI_MAX_ARGS];
  CTypeID result_id;
  uint8_t result_class;
  uint8_t result_kind;
  uint8_t result_size;
  uint8_t result_unsigned;
  uint8_t is_vararg;
  uint8_t nargs;
  uint8_t ngpr;
  uint8_t nfpr;
  uint16_t stack_size;
} LJArm64ABISignature;

/* Resolve typedef and attribute wrappers using only a stable CType snapshot. */
static int lj_ccall_arm64_raw_type(CTState *cts, CTypeID id,
				   CType *out, CTypeID *rawid)
{
  unsigned depth;
  for (depth = 0; depth < 32; depth++) {
    CTInfo info;
    if (lj_ctype_snapshot(cts, id, out) <= 0)
      return 0;
    info = ctype_info_acq(out);
    if (!(ctype_isattrib(info) || ctype_istypedef(info))) {
      *rawid = id;
      return 1;
    }
    id = ctype_cid(info);
    if (id == 0)
      return 0;
  }
  return 0;
}

/* Darwin AAPCS64 scalar carriers. Aggregates, vectors, complex values,
** references, and variadic promotions are intentionally not classified here. */
static int lj_ccall_arm64_scalar_class(CTState *cts, CTypeID id,
				       int result, LJArm64ABIArg *arg,
				       CTypeID *rawid)
{
  CType ct;
  CTInfo info;
  CTSize size;
  if (!lj_ccall_arm64_raw_type(cts, id, &ct, rawid))
    return 0;
  info = ctype_info_acq(&ct);
  size = ctype_size_acq(&ct);
  if (ctype_isvoid(info)) {
    if (!result || size != 0)
      return 0;
    if (arg) {
      arg->cls = LJ_ARM64_ABI_VOID;
      arg->kind = LJ_ARM64_ABI_KIND_VOID;
      arg->size = 0;
      arg->align = 0;
    }
    return 1;
  }
  if (ctype_isnum(info)) {
    if (ctype_isfp(info)) {
      if (size != sizeof(float) && size != sizeof(double))
	return 0;
      if (arg) {
	arg->cls = LJ_ARM64_ABI_FPR;
	arg->kind = LJ_ARM64_ABI_KIND_FLOAT;
	arg->size = (uint8_t)size;
	arg->align = ctype_align(info);
      }
    } else {
      if (!ctype_isinteger_or_bool(info) ||
	  (size != 1 && size != 2 && size != 4 && size != 8))
	return 0;
      if (arg) {
	arg->cls = LJ_ARM64_ABI_GPR;
	arg->kind = LJ_ARM64_ABI_KIND_INTEGER;
	arg->size = (uint8_t)size;
	arg->align = ctype_align(info);
	arg->is_unsigned = (uint8_t)((info & CTF_UNSIGNED) != 0);
      }
    }
    return 1;
  }
  if (ctype_isptr(info) && size == sizeof(void *)) {
    if (arg) {
      arg->cls = LJ_ARM64_ABI_GPR;
      arg->kind = LJ_ARM64_ABI_KIND_POINTER;
      arg->size = (uint8_t)size;
      arg->align = ctype_align(info);
    }
    return 1;
  }
  if (ctype_isenum(info) && size != 0 && size <= sizeof(uint64_t)) {
    CType child;
    CTInfo childinfo;
    CTSize childsize;
    if (lj_ctype_snapshot(cts, ctype_cid(info), &child) <= 0)
      return 0;
    childinfo = ctype_info_acq(&child);
    childsize = ctype_size_acq(&child);
    if (!ctype_isinteger_or_bool(childinfo) ||
	childsize != size ||
	(childsize != 1 && childsize != 2 && childsize != 4 &&
	 childsize != 8))
      return 0;
    if (arg) {
      arg->cls = LJ_ARM64_ABI_GPR;
      arg->kind = LJ_ARM64_ABI_KIND_ENUM;
      arg->size = (uint8_t)size;
      arg->align = ctype_align(info);
      arg->is_unsigned = (uint8_t)((childinfo & CTF_UNSIGNED) != 0);
    }
    return 1;
  }
  return 0;
}

/* Classify the fixed prefix in Darwin's independent GPR/FPR banks. For a
** variadic function, nargs and arg[] describe only the named fixed prefix;
** dynamic promoted tail arguments are classified from the CALLXS IR suffix
** by the independent semantic and post-RA admission passes. Fixed overflow
** arguments use naturally aligned, byte-packed stack locations. The assembler
** rounds the complete outgoing area together with its spill frame. */
static int lj_ccall_arm64_signature(CTState *cts, CTypeID id,
				    LJArm64ABISignature *abi)
{
  CType fn, field;
  CTInfo info;
  CTypeID rawid, fid;
  unsigned nargs = 0, ngpr = 0, nfpr = 0, stack_size = 0;
  LJArm64ABIArg result;
  if (cts == NULL || abi == NULL ||
      !lj_ccall_arm64_raw_type(cts, id, &fn, &rawid))
    return 0;
  info = ctype_info_acq(&fn);
  if (!ctype_isfunc(info) ||
      (ctype_cconv(info) != CTCC_CDECL &&
       ctype_cconv(info) != CTCC_STDCALL) ||
      ctype_size_acq(&fn) > LJ_ARM64_ABI_MAX_ARGS)
    return 0;
  memset(abi, 0, sizeof(*abi));
  memset(&result, 0, sizeof(result));
  abi->is_vararg = (uint8_t)((info & CTF_VARARG) != 0);
  abi->result_id = ctype_cid(info);
  if (!lj_ccall_arm64_scalar_class(cts, abi->result_id, 1, &result,
				   &rawid))
      return 0;
  abi->result_id = rawid;
  abi->result_class = result.cls;
  abi->result_kind = result.kind;
  abi->result_size = result.size;
  abi->result_unsigned = result.is_unsigned;
  fid = ctype_sib_acq(&fn);
  while (fid != 0) {
    CTypeID argid;
    CTInfo finfo;
    LJArm64ABIArg *arg;
    if (nargs >= LJ_ARM64_ABI_MAX_ARGS ||
	    lj_ctype_snapshot(cts, fid, &field) <= 0)
      return 0;
    finfo = ctype_info_acq(&field);
    if (!ctype_isfield(finfo))
      return 0;
    argid = ctype_cid(finfo);
    arg = &abi->arg[nargs];
    memset(arg, 0, sizeof(*arg));
    if (!lj_ccall_arm64_scalar_class(cts, argid, 0, arg, &rawid))
      return 0;
    arg->id = rawid;
    if (arg->cls == LJ_ARM64_ABI_FPR && nfpr < CCALL_NARG_FPR) {
      arg->reg = (uint8_t)nfpr++;
    } else if (arg->cls == LJ_ARM64_ABI_GPR && ngpr < CCALL_NARG_GPR) {
      arg->reg = (uint8_t)ngpr++;
    } else {
      unsigned mask = arg->size-1u;
      /* Over-aligned carriers need their own lowering. Do not silently
      ** substitute the ordinary scalar IR alignment for a different CType. */
      if (arg->size == 0 || arg->size > 8 ||
          (arg->size & mask) != 0 || arg->align > 3 ||
          (1u << arg->align) != arg->size)
        return 0;
      stack_size = (stack_size + mask) & ~mask;
      arg->reg = LJ_ARM64_ABI_STACK;
      arg->stack_offset = (uint16_t)stack_size;
      stack_size += arg->size;
    }
    nargs++;
    fid = ctype_sib_acq(&field);
  }
  if (nargs != ctype_size_acq(&fn))
    return 0;
  abi->nargs = (uint8_t)nargs;
  abi->ngpr = (uint8_t)ngpr;
  abi->nfpr = (uint8_t)nfpr;
  abi->stack_size = (uint16_t)stack_size;
  return 1;
}

#endif

#endif
