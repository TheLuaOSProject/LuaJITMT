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
  LJ_ARM64_ABI_KIND_ENUM,
  LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE,
  LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER
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
  uint8_t source_index;
  uint8_t member_index;
  uint8_t member_count;
  uint16_t stack_offset;
} LJArm64ABIArg;

typedef struct LJArm64ABISignature {
  LJArm64ABIArg arg[LJ_ARM64_ABI_MAX_ARGS];
  CTypeID function_id;
  CTypeID result_declared_id;
  CTypeID result_id;
  uint8_t result_class;
  uint8_t result_kind;
  uint8_t result_size;
  uint8_t result_unsigned;
  uint8_t result_hfa_count;
  uint8_t result_hfa_offset[4];
  uint8_t is_vararg;
  uint8_t nargs;
  uint8_t nparams;
  uint8_t ngpr;
  uint8_t nfpr;
  uint16_t stack_size;
} LJArm64ABISignature;

typedef struct LJArm64FloatHFA {
  uint8_t count;
  uint8_t member_size;
  uint8_t size;
  uint8_t align;
  uint8_t nested;
  uint8_t offset[4];
} LJArm64FloatHFA;

typedef struct LJArm64HFAWalk {
  CTState *cts;
  CTypeID active[32];
  uint8_t count;
  uint8_t member_size;
  uint8_t offset[4];
  uint8_t nested;
} LJArm64HFAWalk;

static int lj_ccall_arm64_raw_type(CTState *cts, CTypeID id,
				   CType *out, CTypeID *rawid);

/* Recursively flatten bounded named nested structs. Every aggregate must be
** tightly packed at its natural alignment; arrays, anonymous subtypes, unions,
** VLA/vector/ref members and over-aligned wrappers stay closed. */
static int lj_ccall_arm64_hfa_flatten(LJArm64HFAWalk *walk, CTypeID id,
					      CTSize base, unsigned depth)
{
  CType ct, field;
  CTypeID rawid, declared_id = id, fid;
  CTInfo info;
  CTSize size;
  unsigned wrap, declared_align = 0;
  unsigned start_count = walk->count;
  if (depth >= 32 || base > UINT8_MAX) return 0;
  for (wrap = 0; wrap < 32; wrap++) {
    if (lj_ctype_snapshot(walk->cts, declared_id, &ct) <= 0) return 0;
    info = ctype_info_acq(&ct);
    if (ctype_isattrib(info) && ctype_isxattrib(info, CTA_ALIGN) &&
	declared_align < ctype_align(info))
      declared_align = ctype_align(info);
    if (!(ctype_isattrib(info) || ctype_istypedef(info))) break;
    declared_id = ctype_cid(info);
    if (declared_id == 0) return 0;
  }
  if (wrap == 32 || !lj_ccall_arm64_raw_type(walk->cts, id, &ct, &rawid))
    return 0;
  for (wrap = 0; wrap < depth; wrap++)
    if (walk->active[wrap] == rawid) return 0;
  walk->active[depth] = rawid;
  info = ctype_info_acq(&ct);
  size = ctype_size_acq(&ct);
  if (info & (CTF_REF|CTF_VLA)) return 0;
  if (ctype_isnum(info) && ctype_isfp(info) && !ctype_iscomplex(info)) {
    unsigned align;
    if (size != sizeof(float) && size != sizeof(double)) return 0;
    align = size == sizeof(float) ? 2u : 3u;
    if (ctype_align(info) != align || declared_align > align ||
	(walk->member_size && walk->member_size != size) ||
	walk->count >= 4 || base != walk->count*size)
      return 0;
    if (!walk->member_size) walk->member_size = (uint8_t)size;
    walk->offset[walk->count++] = (uint8_t)base;
    return 1;
  }
  if (ctype_isstruct(info) && !(info & (CTF_UNION|CTF_REF|CTF_VLA)) &&
      size != 0 && size != CTSIZE_INVALID) {
    CTSize cursor = 0;
    if (depth != 0) walk->nested = 1;
    for (fid = ctype_sib_acq(&ct); fid; ) {
      CTypeID childid;
      CTInfo finfo;
      CTSize offset;
      if (lj_ctype_snapshot(walk->cts, fid, &field) <= 0) return 0;
      finfo = ctype_info_acq(&field);
      fid = ctype_sib_acq(&field);
      if (!ctype_isfield(finfo)) return 0;
      childid = ctype_cid(finfo);
      offset = ctype_size_acq(&field);
      if (!childid || offset != cursor ||
	  !lj_ccall_arm64_hfa_flatten(walk, childid, base+offset, depth+1))
	return 0;
      {
	CType child;
	CTypeID childraw;
	CTSize childsize;
	if (!lj_ccall_arm64_raw_type(walk->cts, childid, &child, &childraw))
	  return 0;
	childsize = ctype_size_acq(&child);
	if (!childsize || childsize == CTSIZE_INVALID ||
	    cursor+childsize > size) return 0;
	cursor += childsize;
      }
    }
    if (walk->count == start_count || cursor != size ||
	(walk->count-start_count)*walk->member_size != size ||
	ctype_align(info) != (walk->member_size == sizeof(float) ? 2u : 3u) ||
	declared_align > (walk->member_size == sizeof(float) ? 2u : 3u))
      return 0;
    return 1;
  }
  return 0;
}

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

/* Darwin AAPCS64 passes a non-HFA composite of at most eight bytes in one
** GPR when one remains. The signature planner separately handles a full-width
** composite on the stack. This bounded subset accepts only structs whose
** direct members are integer/boolean/enum scalars. It deliberately rejects
** unions, nested composites, bitfields, vectors, floating members, refs and
** over-aligned shapes until their independent classifications are proved. */
static int lj_ccall_arm64_integer_aggregate_class(CTState *cts, CTypeID id,
						 LJArm64ABIArg *arg,
						 CTypeID *rawid)
{
  CType ct, field, member, enumbase;
  CTInfo info;
  CTypeID fid;
  CTSize size;
  CTSize field_end = 0;
  unsigned fields = 0;
  CTypeID declared_id = id;
  unsigned depth;
  /* Preserve attributes from the public CType: an aligned wrapper can alter
  ** both the native result channel and the allocation geometry. */
  for (depth = 0; depth < 32; depth++) {
    CType declared;
    if (lj_ctype_snapshot(cts, declared_id, &declared) <= 0) return 0;
    info = ctype_info_acq(&declared);
    if (ctype_isattrib(info) && ctype_isxattrib(info, CTA_ALIGN) &&
	ctype_align(info) > 3)
      return 0;
    if (!(ctype_isattrib(info) || ctype_istypedef(info))) {
      if (ctype_align(info) > 3) return 0;
      break;
    }
    declared_id = ctype_cid(info);
    if (declared_id == 0) return 0;
  }
  if (depth == 32) return 0;
  if (!lj_ccall_arm64_raw_type(cts, id, &ct, rawid)) return 0;
  info = ctype_info_acq(&ct);
  size = ctype_size_acq(&ct);
  if (!ctype_isstruct(info) || (info & CTF_UNION) || ctype_isvltype(info) ||
      size == 0 || size == CTSIZE_INVALID ||
      (size != 1 && size != 2 && size != 4 && size != 8) ||
      ctype_align(info) > 3)
    return 0;
  for (fid = ctype_sib_acq(&ct); fid; ) {
    CTypeID mid;
    CTInfo minfo;
    CTSize msize, offset;
    if (lj_ctype_snapshot(cts, fid, &field) <= 0) return 0;
    info = ctype_info_acq(&field);
    fid = ctype_sib_acq(&field);
    if (!ctype_isfield(info)) return 0;
    offset = ctype_size_acq(&field);
    mid = ctype_cid(info);
    if (!lj_ccall_arm64_raw_type(cts, mid, &member, &mid)) return 0;
    minfo = ctype_info_acq(&member);
    msize = ctype_size_acq(&member);
    if (ctype_isbitfield(minfo)) return 0;
    if (ctype_isenum(minfo)) {
      if (lj_ctype_snapshot(cts, ctype_cid(minfo), &enumbase) <= 0)
	return 0;
      minfo = ctype_info_acq(&enumbase);
      if (!ctype_isinteger_or_bool(minfo) || ctype_isfp(minfo) ||
	  ctype_size_acq(&enumbase) != msize)
	return 0;
    } else if (!ctype_isinteger_or_bool(minfo) || ctype_isfp(minfo)) {
      return 0;
    }
    if (msize == 0 || msize == CTSIZE_INVALID || msize > 8 ||
	offset < field_end || offset > size || msize > size-offset ||
	fields >= 8)
      return 0;
    field_end = offset+msize;
    fields++;
  }
  if (fields == 0) return 0;
  if (arg) {
    arg->cls = LJ_ARM64_ABI_GPR;
    arg->kind = LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE;
    arg->size = (uint8_t)size;
    arg->align = ctype_align(ctype_info_acq(&ct));
    arg->is_unsigned = 1;
  }
  return 1;
}

/* Admit tightly laid out Darwin float/double HFAs. Callers select the count
** and size limits independently for argument and result registers. */
static int lj_ccall_arm64_float_hfa_class_limit(CTState *cts, CTypeID id,
				       LJArm64FloatHFA *hfa,
				       CTypeID *rawid,
				       unsigned max_count,
				       unsigned max_size)
{
  CType ct;
  CTypeID rawout;
  LJArm64HFAWalk walk;
  CTInfo info;
  CTSize size;
  unsigned align;
  if (hfa != NULL) memset(hfa, 0, sizeof(*hfa));
  memset(&walk, 0, sizeof(walk));
  walk.cts = cts;
  if (!lj_ccall_arm64_raw_type(cts, id, &ct, &rawout)) return 0;
  info = ctype_info_acq(&ct);
  size = ctype_size_acq(&ct);
  if (!ctype_isstruct(info) || (info & (CTF_UNION|CTF_REF|CTF_VLA)) ||
      ctype_isvltype(info) || size == 0 || size == CTSIZE_INVALID ||
      size > max_size || !lj_ccall_arm64_hfa_flatten(&walk, id, 0, 0))
    return 0;
  align = walk.member_size == sizeof(float) ? 2u : 3u;
  if (walk.count == 0 || walk.count > max_count ||
      size != walk.count*walk.member_size || ctype_align(info) != align)
    return 0;
  if (rawid != NULL) *rawid = rawout;
  if (hfa != NULL) {
    hfa->count = walk.count;
    hfa->member_size = walk.member_size;
    hfa->size = (uint8_t)size;
    hfa->align = (uint8_t)ctype_align(info);
    hfa->nested = walk.nested;
    memcpy(hfa->offset, walk.offset, sizeof(hfa->offset));
  }
  return 1;
}

/* Darwin ARM64 assigns up to four HFA argument members to consecutive FPRs.
** The result path uses the same D0-D3 capacity. */
static int lj_ccall_arm64_float_hfa_class(CTState *cts, CTypeID id,
					  LJArm64FloatHFA *hfa,
					  CTypeID *rawid)
{
  return lj_ccall_arm64_float_hfa_class_limit(cts, id, hfa, rawid, 4, 32);
}

static int lj_ccall_arm64_float_hfa_result_class(CTState *cts, CTypeID id,
						 LJArm64FloatHFA *hfa,
						 CTypeID *rawid)
{
  LJArm64FloatHFA local;
  if (hfa == NULL) hfa = &local;
  if (!lj_ccall_arm64_float_hfa_class_limit(cts, id, hfa, rawid, 4, 32))
    return 0;
  /* Nested-result return channels are still closed independently of the
  ** flattened argument member lowering. */
  return !hfa->nested;
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
  unsigned nargs = 0, nparams = 0, ngpr = 0, nfpr = 0, stack_size = 0;
  LJArm64ABIArg result;
  if (cts == NULL || abi == NULL ||
      !lj_ccall_arm64_raw_type(cts, id, &fn, &rawid))
    return 0;
  info = ctype_info_acq(&fn);
  /* A callable scalar CData may hold a native-width function pointer. Keep
  ** the guarded outer CType distinct from the resolved function signature;
  ** the recorder's variadic wrapper carries the latter. */
  if (ctype_isptr(info)) {
    if ((info & CTF_REF) || ctype_size_acq(&fn) != sizeof(void *) ||
        !lj_ccall_arm64_raw_type(cts, ctype_cid(info), &fn, &rawid))
      return 0;
    info = ctype_info_acq(&fn);
  }
  if (!ctype_isfunc(info) ||
      (ctype_cconv(info) != CTCC_CDECL &&
       ctype_cconv(info) != CTCC_STDCALL) ||
      ctype_size_acq(&fn) > LJ_ARM64_ABI_MAX_ARGS)
    return 0;
  memset(abi, 0, sizeof(*abi));
  memset(&result, 0, sizeof(result));
  abi->function_id = rawid;
  abi->is_vararg = (uint8_t)((info & CTF_VARARG) != 0);
  abi->result_declared_id = ctype_cid(info);
  abi->result_id = abi->result_declared_id;
  if (!lj_ccall_arm64_scalar_class(cts, abi->result_declared_id, 1,
                                   &result, &rawid)) {
    LJArm64FloatHFA hfa;
    if (lj_ccall_arm64_float_hfa_result_class(cts, abi->result_declared_id,
		&hfa, &rawid)) {
      result.cls = LJ_ARM64_ABI_FPR;
      result.kind = LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER;
      result.size = hfa.member_size;
      abi->result_hfa_count = hfa.count;
      memcpy(abi->result_hfa_offset, hfa.offset,
             sizeof(abi->result_hfa_offset));
    } else {
      if (!lj_ccall_arm64_integer_aggregate_class(cts,
	    abi->result_declared_id, &result, &rawid))
	return 0;
      result.kind = LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE;
      result.is_unsigned = 1;
    }
  }
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
    {
      LJArm64FloatHFA hfa;
      if (nparams >= LJ_ARM64_ABI_MAX_ARGS) return 0;
      if (lj_ccall_arm64_float_hfa_class(cts, argid, &hfa, &rawid)) {
	unsigned part;
	int stack_hfa = nfpr+hfa.count > CCALL_NARG_FPR;
	if (abi->is_vararg || nargs+hfa.count > LJ_ARM64_ABI_MAX_ARGS)
	  return 0;
	/* AAPCS64 places an HFA wholly in consecutive FPRs or wholly on the
	** stack. If the remaining FPR bank cannot hold the complete group, it is
	** closed for subsequent FP arguments as well. Stack placement uses the
	** natural alignment of an HFA member (4 for float, 8 for double). */
	if (stack_hfa) {
	  unsigned align = 1u << hfa.align;
	  stack_size = (stack_size+align-1u) & ~(align-1u);
	  nfpr = CCALL_NARG_FPR;
	}
	for (part = 0; part < hfa.count; part++) {
	  arg = &abi->arg[nargs];
	  memset(arg, 0, sizeof(*arg));
	  arg->id = rawid;
	  arg->cls = LJ_ARM64_ABI_FPR;
	  arg->kind = LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER;
	  arg->reg = stack_hfa ? LJ_ARM64_ABI_STACK : (uint8_t)nfpr++;
	  arg->size = hfa.member_size;
	  arg->align = hfa.align;
	  arg->source_index = (uint8_t)nparams;
	  arg->member_index = (uint8_t)part;
	  arg->member_count = hfa.count;
	  if (stack_hfa) {
	    arg->stack_offset = (uint16_t)stack_size;
	    stack_size += hfa.member_size;
	  }
	  nargs++;
	}
      } else {
	arg = &abi->arg[nargs];
	memset(arg, 0, sizeof(*arg));
	if (!lj_ccall_arm64_scalar_class(cts, argid, 0, arg, &rawid) &&
	    !lj_ccall_arm64_integer_aggregate_class(cts, argid, arg, &rawid))
	  return 0;
	arg->id = rawid;
	arg->source_index = (uint8_t)nparams;
	arg->member_count = 1;
	if (arg->kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE) {
	  if (ngpr < CCALL_NARG_GPR) {
	    arg->reg = (uint8_t)ngpr++;
	  } else {
	    /* A small non-HFA composite occupies one whole eight-byte slot.
	    ** Only its CType-width payload is read and stored. */
	    if (abi->is_vararg) return 0;
	    stack_size = (stack_size+7u) & ~7u;
	    arg->reg = LJ_ARM64_ABI_STACK;
	    arg->stack_offset = (uint16_t)stack_size;
	    stack_size += 8u;
	  }
	} else if (arg->cls == LJ_ARM64_ABI_FPR && nfpr < CCALL_NARG_FPR) {
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
      }
      nparams++;
    }
    fid = ctype_sib_acq(&field);
  }
  if (nparams != ctype_size_acq(&fn))
    return 0;
  abi->nargs = (uint8_t)nargs;
  abi->nparams = (uint8_t)nparams;
  abi->ngpr = (uint8_t)ngpr;
  abi->nfpr = (uint8_t)nfpr;
  abi->stack_size = (uint16_t)stack_size;
  return 1;
}

#endif

#endif
