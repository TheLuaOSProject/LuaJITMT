/*
** Table handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TAB_H
#define _LJ_TAB_H

#include "lj_obj.h"

/* Hash constants. Tuned using a brute force search. */
#define HASH_BIAS	(-0x04c11db7)
#define HASH_ROT1	14
#define HASH_ROT2	5
#define HASH_ROT3	13

/* Scramble the bits of numbers and pointers. */
static LJ_AINLINE uint32_t hashrot(uint32_t lo, uint32_t hi)
{
#if LJ_TARGET_X86ORX64
  /* Prefer variant that compiles well for a 2-operand CPU. */
  lo ^= hi; hi = lj_rol(hi, HASH_ROT1);
  lo -= hi; hi = lj_rol(hi, HASH_ROT2);
  hi ^= lo; hi -= lj_rol(lo, HASH_ROT3);
#else
  lo ^= hi;
  lo = lo - lj_rol(hi, HASH_ROT1);
  hi = lo ^ lj_rol(hi, HASH_ROT1 + HASH_ROT2);
  hi = hi - lj_rol(lo, HASH_ROT3);
#endif
  return hi;
}

/* Hash values are masked with the table hash mask and used as an index. */
static LJ_AINLINE Node *hashmask_node(Node *n, MSize hmask, uint32_t hash)
{
  return &n[hash & hmask];
}

static LJ_AINLINE Node *hashmask(const GCtab *t, uint32_t hash)
{
  MSize hmask;
  Node *n = lj_tab_node_snapshot_acq(t, &hmask);
  return hashmask_node(n, hmask, hash);
}

/* String IDs are generated when a string is interned. */
#define hashstr_node(n, hmask, s)	hashmask_node((n), (hmask), (s)->sid)
#define hashstr(t, s)		hashmask(t, (s)->sid)

#define hashlohi_node(n, hmask, lo, hi) \
  hashmask_node((n), (hmask), hashrot((lo), (hi)))
#define hashlohi(t, lo, hi)	hashmask((t), hashrot((lo), (hi)))
#define hashnum_node(n, hmask, o) \
  hashlohi_node((n), (hmask), (o)->u32.lo, ((o)->u32.hi << 1))
#define hashnum(t, o)		hashlohi((t), (o)->u32.lo, ((o)->u32.hi << 1))
#if LJ_GC64
#define hashgcref_node(n, hmask, r) \
  hashlohi_node((n), (hmask), (uint32_t)gcrefu(r), \
		(uint32_t)(gcrefu(r) >> 32))
#define hashgcref(t, r) \
  hashlohi((t), (uint32_t)gcrefu(r), (uint32_t)(gcrefu(r) >> 32))
#else
#define hashgcref_node(n, hmask, r) \
  hashlohi_node((n), (hmask), gcrefu(r), gcrefu(r) + HASH_BIAS)
#define hashgcref(t, r)		hashlohi((t), gcrefu(r), gcrefu(r) + HASH_BIAS)
#endif

#define hsize2hbits(s)	((s) ? ((s)==1 ? 1 : 1+lj_fls((uint32_t)((s)-1))) : 0)

LJ_FUNCA GCtab *lj_tab_new(lua_State *L, uint32_t asize, uint32_t hbits);
LJ_FUNC GCtab *lj_tab_new_ah(lua_State *L, uint32_t a, uint32_t h);
#if LJ_HASJIT
LJ_FUNC GCtab * LJ_FASTCALL lj_tab_new1(lua_State *L, uint32_t ahsize);
#endif
LJ_FUNCA GCtab * LJ_FASTCALL lj_tab_dup(lua_State *L, const GCtab *kt);
LJ_FUNC void LJ_FASTCALL lj_tab_clear(GCtab *t);
LJ_FUNC void LJ_FASTCALL lj_tab_free(global_State *g, GCtab *t);
LJ_FUNC void lj_tab_resize(lua_State *L, GCtab *t, uint32_t asize, uint32_t hbits);
LJ_FUNC uint32_t lj_tab_reclaim_retired(global_State *g,
					uint64_t completed_epoch);
LJ_FUNC void lj_tab_freeretired(global_State *g);
LJ_FUNCA void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize);

/* Caveat: all getters except lj_tab_get() can return NULL! */

LJ_FUNCA cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key);
LJ_FUNC cTValue *lj_tab_getstr(GCtab *t, const GCstr *key);
LJ_FUNCA cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key);

/* Caveat: all setters require a write barrier for the stored value. */

LJ_FUNCA TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key);
LJ_FUNC int lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,
				     cTValue *claim, TValue **slot);
LJ_FUNC int lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,
				    cTValue *claim, TValue **slot);
LJ_FUNCA TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key);
LJ_FUNC TValue *lj_tab_setstr(lua_State *L, GCtab *t, const GCstr *key);
LJ_FUNC TValue *lj_tab_set(lua_State *L, GCtab *t, cTValue *key);
LJ_FUNCA TValue *lj_tab_storetv(lua_State *L, TValue *dst, cTValue *src);
LJ_FUNCA TValue *lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent,
					     TValue *dst, cTValue *src);
LJ_FUNCA TValue *lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src);
LJ_FUNCA TValue *lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src);
LJ_FUNCA TValue *lj_tab_storetvn(lua_State *L, TValue *dst, cTValue *src,
				 uint32_t n);
LJ_FUNC TValue *lj_tab_storenilraw(TValue *dst);
LJ_FUNC TValue *lj_tab_storenil(lua_State *L, TValue *dst);
LJ_FUNC TValue *lj_tab_storebool(lua_State *L, TValue *dst, int b);
LJ_FUNC TValue *lj_tab_storeint(lua_State *L, TValue *dst, int32_t i);
LJ_FUNC TValue *lj_tab_storeintptr(lua_State *L, TValue *dst, intptr_t i);
LJ_FUNC TValue *lj_tab_storestr(lua_State *L, TValue *dst, GCstr *s);
LJ_FUNC TValue *lj_tab_storetab(lua_State *L, TValue *dst, GCtab *t);
LJ_FUNC TValue *lj_tab_storethread(lua_State *L, TValue *dst, lua_State *th);
LJ_FUNC TValue *lj_tab_storeproto(lua_State *L, TValue *dst, GCproto *pt);
LJ_FUNC TValue *lj_tab_storefunc(lua_State *L, TValue *dst, GCfunc *fn);
LJ_FUNC TValue *lj_tab_storeudata(lua_State *L, TValue *dst, GCudata *ud);

static LJ_AINLINE cTValue *lj_tab_getint(GCtab *t, int32_t key)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  return (MSize)key < asize ? &array[key] : lj_tab_getinth(t, key);
}

static LJ_AINLINE TValue *lj_tab_setint(lua_State *L, GCtab *t, int32_t key)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  return (MSize)key < asize ? &array[key] : lj_tab_setinth(L, t, key);
}

LJ_FUNC uint32_t LJ_FASTCALL lj_tab_keyindex(GCtab *t, cTValue *key);
LJ_FUNCA int lj_tab_next(GCtab *t, cTValue *key, TValue *o);
LJ_FUNCA MSize LJ_FASTCALL lj_tab_len(GCtab *t);
#if LJ_HASJIT
LJ_FUNC MSize LJ_FASTCALL lj_tab_len_hint(GCtab *t, size_t hint);
#endif

#endif
