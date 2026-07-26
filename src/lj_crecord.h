/*
** Trace recorder for C data operations.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CRECORD_H
#define _LJ_CRECORD_H

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_ffrecord.h"

#if LJ_HASJIT && LJ_HASFFI
LJ_FUNC void LJ_FASTCALL recff_cdata_index(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_cdata_call(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_cdata_arith(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_binop(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_unop(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_round(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_cmp(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_shift(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_movemask(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_maskcmp(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_simd_reduce(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_select(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_bitcast(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_convert(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_fma(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_shuffle(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_shuffle2(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_simd_insert(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_clib_index(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_new(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_errno(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_string(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_copy(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_fill(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_typeof(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_istype(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_abi(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_xof(jit_State *J, RecordFFData *rd);
LJ_FUNC void LJ_FASTCALL recff_ffi_gc(jit_State *J, RecordFFData *rd);

LJ_FUNC void LJ_FASTCALL recff_bit64_tobit(jit_State *J, RecordFFData *rd);
LJ_FUNC int LJ_FASTCALL recff_bit64_unary(jit_State *J, RecordFFData *rd);
LJ_FUNC int LJ_FASTCALL recff_bit64_nary(jit_State *J, RecordFFData *rd);
LJ_FUNC int recff_bit64_shift(jit_State *J, TRef *rb, TRef *rc,
			      TValue *rbv, TValue *rcv, IROp op);
LJ_FUNC TRef recff_bit64_tohex(jit_State *J, RecordFFData *rd, TRef hdr);
LJ_FUNC TRef recff_bit64_bitop(jit_State *J, TRef rb, TRef rc,
			       TValue *rbv, TValue *rcv, IROp op);

LJ_FUNC void LJ_FASTCALL lj_crecord_tonumber(jit_State *J, RecordFFData *rd);
LJ_FUNC TRef lj_crecord_loadiu64(jit_State *J, TRef tr, cTValue *o);
#if LJ_HASBUFFER
LJ_FUNC TRef lj_crecord_topcvoid(jit_State *J, TRef tr, cTValue *o);
LJ_FUNC TRef lj_crecord_topuint8(jit_State *J, TRef tr);
#endif
#endif

#endif
