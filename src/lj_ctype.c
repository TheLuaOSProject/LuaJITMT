/*
** C type management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_strfmt.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"
#include "lj_buf.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

static void ctype_fin_claim_wait(lua_State *L)
{
  /*
  ** FINREG generation publication is a short CAS window. Wait as native time
  ** for the current TG when possible, so safepoint handshakes can complete
  ** while another mutator resolves a visible claim.
  */
  (void)lj_thr_sleep_ns(L, 1000000);
}

static int ctype_had_stopreq(lua_State *L)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int ctype_fresh_stopreq(lua_State *L, uint32_t actions,
			       int had_stopreq)
{
  TGState *tg;
  if (!L)
    return 0;
  tg = L2TG(L);
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

static void ctype_checkstop_fresh(lua_State *L, uint32_t actions,
				  int had_stopreq)
{
  if (ctype_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

/* -- C type definitions -------------------------------------------------- */

/* Predefined typedefs. */
#define CTTDDEF(_) \
  /* Vararg handling. */ \
  _("va_list",			P_VOID) \
  _("__builtin_va_list",	P_VOID) \
  _("__gnuc_va_list",		P_VOID) \
  /* From stddef.h. */ \
  _("ptrdiff_t",		INT_PSZ) \
  _("size_t",			UINT_PSZ) \
  _("wchar_t",			WCHAR) \
  /* Subset of stdint.h. */ \
  _("int8_t",			INT8) \
  _("int16_t",			INT16) \
  _("int32_t",			INT32) \
  _("int64_t",			INT64) \
  _("int128_t",			INT128) \
  _("uint8_t",			UINT8) \
  _("uint16_t",			UINT16) \
  _("uint32_t",			UINT32) \
  _("uint64_t",			UINT64) \
  _("uint128_t",		UINT128) \
  _("intptr_t",			INT_PSZ) \
  _("uintptr_t",		UINT_PSZ) \
  /* From POSIX. */ \
  _("ssize_t",			INT_PSZ) \
  /* End of typedef list. */

/* Keywords (only the ones we actually care for). */
#define CTKWDEF(_) \
  /* Type specifiers. */ \
  _("void",		-1,	CTOK_VOID) \
  _("_Bool",		0,	CTOK_BOOL) \
  _("bool",		1,	CTOK_BOOL) \
  _("char",		1,	CTOK_CHAR) \
  _("int",		4,	CTOK_INT) \
  _("__int8",		1,	CTOK_INT) \
  _("__int16",		2,	CTOK_INT) \
  _("__int32",		4,	CTOK_INT) \
  _("__int64",		8,	CTOK_INT) \
  _("__int128",		16,	CTOK_INT) \
  _("float",		4,	CTOK_FP) \
  _("double",		8,	CTOK_FP) \
  _("long",		0,	CTOK_LONG) \
  _("short",		0,	CTOK_SHORT) \
  _("_Complex",		0,	CTOK_COMPLEX) \
  _("complex",		0,	CTOK_COMPLEX) \
  _("__complex",	0,	CTOK_COMPLEX) \
  _("__complex__",	0,	CTOK_COMPLEX) \
  _("signed",		0,	CTOK_SIGNED) \
  _("__signed",		0,	CTOK_SIGNED) \
  _("__signed__",	0,	CTOK_SIGNED) \
  _("unsigned",		0,	CTOK_UNSIGNED) \
  /* Type qualifiers. */ \
  _("const",		0,	CTOK_CONST) \
  _("__const",		0,	CTOK_CONST) \
  _("__const__",	0,	CTOK_CONST) \
  _("volatile",		0,	CTOK_VOLATILE) \
  _("__volatile",	0,	CTOK_VOLATILE) \
  _("__volatile__",	0,	CTOK_VOLATILE) \
  _("restrict",		0,	CTOK_RESTRICT) \
  _("__restrict",	0,	CTOK_RESTRICT) \
  _("__restrict__",	0,	CTOK_RESTRICT) \
  _("inline",		0,	CTOK_INLINE) \
  _("__inline",		0,	CTOK_INLINE) \
  _("__inline__",	0,	CTOK_INLINE) \
  /* Storage class specifiers. */ \
  _("typedef",		0,	CTOK_TYPEDEF) \
  _("extern",		0,	CTOK_EXTERN) \
  _("static",		0,	CTOK_STATIC) \
  _("auto",		0,	CTOK_AUTO) \
  _("register",		0,	CTOK_REGISTER) \
  /* GCC Attributes. */ \
  _("__extension__",	0,	CTOK_EXTENSION) \
  _("__attribute",	0,	CTOK_ATTRIBUTE) \
  _("__attribute__",	0,	CTOK_ATTRIBUTE) \
  _("asm",		0,	CTOK_ASM) \
  _("__asm",		0,	CTOK_ASM) \
  _("__asm__",		0,	CTOK_ASM) \
  /* MSVC Attributes. */ \
  _("__declspec",	0,	CTOK_DECLSPEC) \
  _("__cdecl",		CTCC_CDECL,	CTOK_CCDECL) \
  _("__thiscall",	CTCC_THISCALL,	CTOK_CCDECL) \
  _("__fastcall",	CTCC_FASTCALL,	CTOK_CCDECL) \
  _("__stdcall",	CTCC_STDCALL,	CTOK_CCDECL) \
  _("__ptr32",		4,	CTOK_PTRSZ) \
  _("__ptr64",		8,	CTOK_PTRSZ) \
  /* Other type specifiers. */ \
  _("struct",		0,	CTOK_STRUCT) \
  _("union",		0,	CTOK_UNION) \
  _("enum",		0,	CTOK_ENUM) \
  /* Operators. */ \
  _("sizeof",		0,	CTOK_SIZEOF) \
  _("__alignof",	0,	CTOK_ALIGNOF) \
  _("__alignof__",	0,	CTOK_ALIGNOF) \
  /* End of keyword list. */

/* Type info for predefined types. Size merged in. */
static CTInfo lj_ctype_typeinfo[] = {
#define CTTYINFODEF(id, sz, ct, info)	CTINFO((ct),(((sz)&0x3fu)<<10)+(info)),
#define CTTDINFODEF(name, id)		CTINFO(CT_TYPEDEF, CTID_##id),
#define CTKWINFODEF(name, sz, kw)	CTINFO(CT_KW,(((sz)&0x3fu)<<10)+(kw)),
CTTYDEF(CTTYINFODEF)
CTTDDEF(CTTDINFODEF)
CTKWDEF(CTKWINFODEF)
#undef CTTYINFODEF
#undef CTTDINFODEF
#undef CTKWINFODEF
  0
};

/* Predefined type names collected in a single string. */
static const char * const lj_ctype_typenames =
#define CTTDNAMEDEF(name, id)		name "\0"
#define CTKWNAMEDEF(name, sz, cds)	name "\0"
CTTDDEF(CTTDNAMEDEF)
CTKWDEF(CTKWNAMEDEF)
#undef CTTDNAMEDEF
#undef CTKWNAMEDEF
;

static int ctype_cspace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	 c == '\f' || c == '\v';
}

static int ctype_strlit(const char *p, MSize plen, const char *lit, MSize len)
{
  if (plen != len)
    return 0;
  while (len-- != 0)
    if (*p++ != *lit++)
      return 0;
  return 1;
}

#define ctype_string_match(lit) \
  ctype_strlit(p, len, "" lit, (MSize)(sizeof(lit)-1))

int lj_ctype_predefined_string(const char *p, MSize len, CTypeID *idp)
{
  while (len != 0 && ctype_cspace(*p)) { p++; len--; }
  while (len != 0 && ctype_cspace(p[len-1])) len--;
  if (ctype_string_match("void")) { *idp = CTID_VOID; return 1; }
  if (ctype_string_match("const void")) { *idp = CTID_CVOID; return 1; }
  if (ctype_string_match("void const")) { *idp = CTID_CVOID; return 1; }
  if (ctype_string_match("void *")) { *idp = CTID_P_VOID; return 1; }
  if (ctype_string_match("void*")) { *idp = CTID_P_VOID; return 1; }
  if (ctype_string_match("const void *")) { *idp = CTID_P_CVOID; return 1; }
  if (ctype_string_match("const void*")) { *idp = CTID_P_CVOID; return 1; }
  if (ctype_string_match("void const *")) { *idp = CTID_P_CVOID; return 1; }
  if (ctype_string_match("void const*")) { *idp = CTID_P_CVOID; return 1; }
  if (ctype_string_match("bool")) { *idp = CTID_BOOL; return 1; }
  if (ctype_string_match("_Bool")) { *idp = CTID_BOOL; return 1; }
  if (ctype_string_match("char")) { *idp = CTID_INT8; return 1; }
  if (ctype_string_match("signed char")) { *idp = CTID_INT8; return 1; }
  if (ctype_string_match("unsigned char")) { *idp = CTID_UINT8; return 1; }
  if (ctype_string_match("const char")) { *idp = CTID_CCHAR; return 1; }
  if (ctype_string_match("char const")) { *idp = CTID_CCHAR; return 1; }
  if (ctype_string_match("const char *")) { *idp = CTID_P_CCHAR; return 1; }
  if (ctype_string_match("const char*")) { *idp = CTID_P_CCHAR; return 1; }
  if (ctype_string_match("char const *")) { *idp = CTID_P_CCHAR; return 1; }
  if (ctype_string_match("char const*")) { *idp = CTID_P_CCHAR; return 1; }
  if (ctype_string_match("unsigned char *")) {
    *idp = CTID_P_UINT8;
    return 1;
  }
  if (ctype_string_match("unsigned char*")) { *idp = CTID_P_UINT8; return 1; }
  if (ctype_string_match("short")) { *idp = CTID_INT16; return 1; }
  if (ctype_string_match("short int")) { *idp = CTID_INT16; return 1; }
  if (ctype_string_match("signed short")) { *idp = CTID_INT16; return 1; }
  if (ctype_string_match("signed short int")) { *idp = CTID_INT16; return 1; }
  if (ctype_string_match("unsigned short")) { *idp = CTID_UINT16; return 1; }
  if (ctype_string_match("unsigned short int")) {
    *idp = CTID_UINT16;
    return 1;
  }
  if (ctype_string_match("int")) { *idp = CTID_INT32; return 1; }
  if (ctype_string_match("signed")) { *idp = CTID_INT32; return 1; }
  if (ctype_string_match("signed int")) { *idp = CTID_INT32; return 1; }
  if (ctype_string_match("unsigned")) { *idp = CTID_UINT32; return 1; }
  if (ctype_string_match("unsigned int")) { *idp = CTID_UINT32; return 1; }
  if (sizeof(long) == 8 &&
      (ctype_string_match("long") || ctype_string_match("long int") ||
       ctype_string_match("signed long") ||
       ctype_string_match("signed long int"))) {
    *idp = CTID_INT64;
    return 1;
  }
  if (sizeof(long) == 8 &&
      (ctype_string_match("unsigned long") ||
       ctype_string_match("unsigned long int"))) {
    *idp = CTID_UINT64;
    return 1;
  }
  if (ctype_string_match("float")) { *idp = CTID_FLOAT; return 1; }
  if (ctype_string_match("double")) { *idp = CTID_DOUBLE; return 1; }
  if (ctype_string_match("complex") ||
      ctype_string_match("_Complex") ||
      ctype_string_match("__complex") ||
      ctype_string_match("__complex__") ||
      ctype_string_match("complex double") ||
      ctype_string_match("double complex") ||
      ctype_string_match("_Complex double") ||
      ctype_string_match("double _Complex") ||
      ctype_string_match("__complex double") ||
      ctype_string_match("double __complex") ||
      ctype_string_match("__complex__ double") ||
      ctype_string_match("double __complex__") ||
      ctype_string_match("complex long double") ||
      ctype_string_match("long double complex") ||
      ctype_string_match("_Complex long double") ||
      ctype_string_match("long double _Complex") ||
      ctype_string_match("__complex long double") ||
      ctype_string_match("long double __complex") ||
      ctype_string_match("__complex__ long double") ||
      ctype_string_match("long double __complex__")) {
    *idp = CTID_COMPLEX_DOUBLE;
    return 1;
  }
  if (ctype_string_match("complex float") ||
      ctype_string_match("float complex") ||
      ctype_string_match("_Complex float") ||
      ctype_string_match("float _Complex") ||
      ctype_string_match("__complex float") ||
      ctype_string_match("float __complex") ||
      ctype_string_match("__complex__ float") ||
      ctype_string_match("float __complex__")) {
    *idp = CTID_COMPLEX_FLOAT;
    return 1;
  }
  if (ctype_string_match("int8_t")) { *idp = CTID_INT8; return 1; }
  if (ctype_string_match("uint8_t")) { *idp = CTID_UINT8; return 1; }
  if (ctype_string_match("uint8_t *")) { *idp = CTID_P_UINT8; return 1; }
  if (ctype_string_match("uint8_t*")) { *idp = CTID_P_UINT8; return 1; }
  if (ctype_string_match("int16_t")) { *idp = CTID_INT16; return 1; }
  if (ctype_string_match("uint16_t")) { *idp = CTID_UINT16; return 1; }
  if (ctype_string_match("int32_t")) { *idp = CTID_INT32; return 1; }
  if (ctype_string_match("uint32_t")) { *idp = CTID_UINT32; return 1; }
  if (ctype_string_match("int64_t")) { *idp = CTID_INT64; return 1; }
  if (ctype_string_match("uint64_t")) { *idp = CTID_UINT64; return 1; }
  if (ctype_string_match("ptrdiff_t")) { *idp = CTID_INT_PSZ; return 1; }
  if (ctype_string_match("size_t")) { *idp = CTID_UINT_PSZ; return 1; }
  if (ctype_string_match("ssize_t")) { *idp = CTID_INT_PSZ; return 1; }
  if (ctype_string_match("intptr_t")) { *idp = CTID_INT_PSZ; return 1; }
  if (ctype_string_match("uintptr_t")) { *idp = CTID_UINT_PSZ; return 1; }
  if (ctype_string_match("wchar_t")) { *idp = CTID_WCHAR; return 1; }
  return 0;
}

#undef ctype_string_match

#define CTTYPEINFO_NUM		(sizeof(lj_ctype_typeinfo)/sizeof(CTInfo)-1)
#ifdef LUAJIT_CTYPE_CHECK_ANCHOR
#define CTTYPETAB_MIN		CTTYPEINFO_NUM
#else
#define CTTYPETAB_MIN		128
#endif
#define CTCBBLACK_SIZE		4096u

/* -- C type interning ---------------------------------------------------- */

#define ct_hashtype(info, size)	(hashrot(info, size) & CTHASH_MASK)
#define ct_hashname(name) \
  (hashrot(u32ptr(name), u32ptr(name) + HASH_BIAS) & CTHASH_MASK)

void lj_ctype_parse_wait(CTState *cts, lua_State *L, uint32_t seq)
{
  if ((seq & 1u) == 0)
    return;
#if defined(__linux__)
  {
    uint32_t actions;
    int had_stopreq = ctype_had_stopreq(L);
    if (L)
      lj_native_enter(L2TG(L));
    (void)ctype_parse_token_wait(cts, seq, 1000000);
    actions = L ? lj_native_leave(L) : 0;
    ctype_checkstop_fresh(L, actions, had_stopreq);  /* 11.2: ctype wait may park. */
  }
#else
  {
    int had_stopreq = ctype_had_stopreq(L);
    uint32_t actions = lj_thr_sleep_ns(L, 1000000);
    ctype_checkstop_fresh(L, actions, had_stopreq);
  }
#endif
}

void lj_ctype_parse_lock(CTState *cts, lua_State *L)
{
  for (;;) {
    uint32_t seq = ctype_parse_token_acq(cts);
    if ((seq & 1u) == 0) {
      uint32_t expect = seq;
      if (ctype_parse_token_cas(cts, &expect, seq + 1u))
	return;  /* 11.2: acquire the cparse CTState mutation sequence. */
      continue;
    }
    lj_ctype_parse_wait(cts, L, seq);
  }
}

void lj_ctype_parse_unlock(CTState *cts)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  lj_assertCTS((seq & 1u) != 0, "cparse mutation sequence not held");
  ctype_parse_token_rel(cts, seq + 1u);  /* 11.2: publish parser mutations. */
#if defined(__linux__)
  (void)ctype_parse_token_wake(cts, 1);  /* 11.2: wake next cparse waiter. */
#endif
}

static LJ_AINLINE int ctype_snapshot_done(CTState *cts, uint32_t seq0,
					  int ok)
{
  uint32_t seq1 = ctype_parse_token_acq(cts);
  return (seq0 == seq1 && !(seq1 & 1u)) ? ok : -1;
}

int lj_ctype_snapshot(CTState *cts, CTypeID id, CType *out)
{
  uint32_t seq0;
  CTypeTab *tabh;
  CType *ct;
  GCobj *name;

  if (id == 0)
    return 0;
  seq0 = ctype_parse_token_acq(cts);
  if (seq0 & 1u)
    return -1;  /* Active parser rollback window: use the locked reader. */
  if (id >= ctype_top_acq(cts))
    return ctype_snapshot_done(cts, seq0, 0);
  tabh = ctype_tabh_acq(cts);
  if ((MSize)id >= ctype_tab_sizetab_acq(tabh))
    return -1;  /* Table/top snapshot raced a grow; retry under the lock. */
  ct = ctype_tab_slot(tabh, id);
  out->info = ctype_info_acq(ct);
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  name = ctype_nameobj_acq(ct);
  setgcrefp(out->name, name);
  return ctype_snapshot_done(cts, seq0,
			     !ctype_isabandoned(ctype_info_acq(out)));
}

static void ctype_storestr_str(lua_State *L, GCtab *tab, GCstr *key,
			       GCstr *val)
{
  TValue keytv, tv, *dst;
  setstrV(L, &tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_l(L);  /* ctype string store saw stale/FORWARD slot. */
  }
}

static GCtab *ctype_fin_tab_new_l(lua_State *L, uint32_t hbits)
{
  TValue *anchor = L->top;
  GCtab *t = lj_tab_new(L, 0, hbits);
  settabV(L, L->top++, t);
  fin_gen_tab_enable_rel(t);
  ctype_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "k"));
  lj_tab_nomm_rel(t, (uint8_t)(~(1u<<MM_mode)));
  lj_gc_pubtab(L, t);
  L->top = anchor;
  return t;
}

static FinRegGen *ctype_fin_gen_new_l(lua_State *L, GCtab *t)
{
  FinRegGen *gen = lj_mem_newt(L, sizeof(FinRegGen), FinRegGen);
  fin_gen_tab_rel(gen, t);
  fin_gen_next_rel(gen, NULL);
  return gen;
}

FinRegOrderNode *lj_ctype_fin_order_new(lua_State *L)
{
  FinRegOrderNode *ord = lj_mem_newt(L, sizeof(FinRegOrderNode),
				     FinRegOrderNode);
  fin_order_obj_clear(ord);
  fin_order_tab_rel(ord, NULL);
  fin_order_slot_rel(ord, NULL);
  fin_order_next_rel(ord, NULL);
  fin_order_retired_next_rel(ord, NULL);
  fin_order_active_rel(ord, 0);
  return ord;
}

void lj_ctype_fin_order_free(global_State *g, FinRegOrderNode *ord)
{
  if (ord)
    lj_mem_freet(g, ord);
}

void lj_ctype_fin_order_publish(CTState *cts, FinRegOrderNode *ord,
				GCobj *o, GCtab *t, TValue *slot)
{
  FinRegOrderNode *head;
  if (!cts || !ord || !o || !t || !slot)
    return;
  fin_order_obj_rel(ord, o);
  fin_order_tab_rel(ord, t);
  fin_order_slot_rel(ord, slot);
  fin_order_active_rel(ord, 1);
  do {
    head = fin_order_head_acq(cts);
    fin_order_next_rel(ord, head);
  } while (!fin_order_head_cas(cts, &head, ord));
  /* 11.4 FINREG ordered registration publish. */
}

int lj_ctype_fin_order_retire(CTState *cts, FinRegOrderNode *prev,
			      FinRegOrderNode *ord, FinRegOrderNode *next)
{
  FinRegOrderNode *head, *expect;
  if (!cts || !ord)
    return 0;
  if (!fin_order_active_retiring(ord))
    return 1;
  do {
    head = fin_order_retired_acq(cts);
    fin_order_retired_next_rel(ord, head);
  } while (!fin_order_retired_cas(cts, &head, ord));
  lj_gc2_finreg_cdata_note_order_retired(cts->g);
  fin_order_active_rel(ord, 0);
  if (prev) {
    expect = ord;
    if (fin_order_active_acq(prev) == 1)
      (void)fin_order_next_cas(prev, &expect, next);
  } else {
    expect = ord;
    (void)fin_order_head_cas(cts, &expect, next);
  }
  return 1;  /* 11.4 ordered FINREG logical retire plus best-effort splice. */
}

GCtab *lj_ctype_fin_head(CTState *cts)
{
  FinRegGen *gen = fin_gen_head_acq(cts);
  return gen ? fin_gen_tab_acq(gen) : NULL;
}

cTValue *lj_ctype_fin_get(lua_State *L, CTState *cts, cTValue *key,
			  GCtab **tabp)
{
  FinRegGen *gen;
  for (gen = fin_gen_head_acq(cts);
       gen != NULL;
       gen = fin_gen_next_acq(gen)) {
    GCtab *t = fin_gen_tab_acq(gen);
    if (!t || !fin_gen_tab_enabled_acq(t))
      continue;
    cTValue *tv = lj_tab_get(L, t, key);
    if (tv != niltv(L)) {
      *tabp = t;
      return tv;
    }
  }
  *tabp = NULL;
  return niltv(L);
}

static int ctype_fin_any_key(CTState *cts, lua_State *L, cTValue *key)
{
  GCtab *t;
  return lj_ctype_fin_get(L, cts, key, &t) != niltv(L);
}

static int ctype_fin_has_claim(CTState *cts, cTValue *claim)
{
  FinRegGen *gen;
  for (gen = fin_gen_head_acq(cts);
       gen != NULL;
       gen = fin_gen_next_acq(gen)) {
    GCtab *t = fin_gen_tab_acq(gen);
    MSize i, hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    for (i = 0; i <= hmask; i++) {
      TValue val;
      lj_tv_load_acq(&val, &node[i].val);
      if (tv_rawload(&val) == tv_rawload(claim))
	return 1;
    }
  }
  return 0;
}

int lj_ctype_fin_newgen(lua_State *L, CTState *cts, cTValue *key,
			cTValue *claim, GCtab **tabp, TValue **slot)
{
  for (;;) {
    FinRegGen *head = fin_gen_head_acq(cts);
    GCtab *headtab = head ? fin_gen_tab_acq(head) : NULL;
    MSize hmask = 1;
    uint32_t hbits;
    GCtab *t;
    FinRegGen *gen;
    TValue *tv;
    if (headtab)
      (void)lj_tab_node_snapshot_acq(headtab, &hmask);
    hbits = hmask > 0 ? lj_fls((uint32_t)hmask) + 2u : 1u;
    if (headtab && !fin_gen_tab_enabled_acq(headtab))
      return 0;
    while (ctype_fin_has_claim(cts, claim))
      ctype_fin_claim_wait(L);
    if (ctype_fin_any_key(cts, L, key))
      return -1;
    t = ctype_fin_tab_new_l(L, hbits);
    gen = ctype_fin_gen_new_l(L, t);
    tv = lj_tab_set(L, t, key);  /* Private generation, unpublished. */
    copyTVrel(L, tv, claim);
    fin_gen_next_rel(gen, head);
    if (fin_gen_head_cas(cts, &head, gen)) {
      *tabp = t;
      *slot = tv;
      return 1;  /* 11.4 FINREG generation CAS publish. */
    }
    lj_mem_freet(G(L), gen);
    ctype_fin_claim_wait(L);
  }
}

int lj_ctype_fin_istab(global_State *g, GCtab *t)
{
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen;
  if (!cts)
    return 0;
  for (gen = fin_gen_head_acq(cts);
       gen != NULL;
       gen = fin_gen_next_acq(gen)) {
    GCtab *ft = fin_gen_tab_acq(gen);
    if (ft == t && ft && fin_gen_tab_enabled_acq(ft))
      return 1;
  }
  return 0;
}

void lj_ctype_fin_mark(global_State *g, void (*mark)(global_State *, GCobj *),
		       void (*markmem)(global_State *, void *))
{
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen;
  if (!cts)
    return;
  for (gen = fin_gen_head_acq(cts);
       gen != NULL;
       gen = fin_gen_next_acq(gen)) {
    GCtab *t = fin_gen_tab_acq(gen);
    markmem(g, gen);
    if (t)
      mark(g, obj2gco(t));
  }
  {
    FinRegOrderNode *ord;
    for (ord = fin_order_head_acq(cts);
	 ord != NULL;
	 ord = fin_order_next_acq(ord)) {
      if (fin_order_active_acq(ord) != 0)
	markmem(g, ord);
    }
    for (ord = fin_order_retired_acq(cts);
	 ord != NULL;
	 ord = fin_order_retired_next_acq(ord))
      markmem(g, ord);
  }
}

void lj_ctype_fin_freetabs(global_State *g, CTState *cts)
{
  FinRegGen *gen = fin_gen_head_xchg_acqrel(cts, NULL);
  FinRegOrderNode *ord = fin_order_head_xchg_acqrel(cts, NULL);
  while (ord) {
    FinRegOrderNode *next = fin_order_next_acq(ord);
    if (fin_order_active_acq(ord) != 0)
      lj_mem_freet(g, ord);
    ord = next;
  }
  ord = fin_order_retired_xchg_acqrel(cts, NULL);
  while (ord) {
    FinRegOrderNode *next = fin_order_retired_next_acq(ord);
    lj_mem_freet(g, ord);
    ord = next;
  }
  while (gen) {
    FinRegGen *next = fin_gen_next_acq(gen);
    lj_mem_freet(g, gen);
    gen = next;
  }
}

static GCRef *ctype_metamap_init_l(lua_State *L, CTState *cts)
{
  GCRef *meta = lj_mem_newvec(L, CTID_MAX, GCRef);
  memset(meta, 0, CTID_MAX*sizeof(GCRef));
  ctype_metamap_rel(cts, meta);
  ctype_metamap_size_rel(cts, CTID_MAX);
  return meta;
}

static LJ_AINLINE GCtab *ctype_meta_tab(CTState *cts, CTypeID id)
{
  GCRef *meta = ctype_metamap_acq(cts);
  MSize sizemeta = ctype_metamap_size_acq(cts);
  if (LJ_UNLIKELY(meta == NULL || (MSize)id >= sizemeta))
    return NULL;
  return (GCtab *)ctype_metamap_obj_acq(meta, (MSize)id);
}

int lj_ctype_setmeta(CTState *cts, CTypeID id, GCtab *mt)
{
  GCRef *meta = ctype_metamap_acq(cts);
  MSize sizemeta = ctype_metamap_size_acq(cts);
  if (LJ_UNLIKELY(meta == NULL || (MSize)id >= sizemeta))
    return 0;
  return ctype_metamap_obj_cas(meta, (MSize)id, mt);
}

static uint64_t *ctype_cbblack_init_l(lua_State *L, CTState *cts)
{
  uint64_t *tab = lj_mem_newvec(L, CTCBBLACK_SIZE, uint64_t);
  memset(tab, 0, CTCBBLACK_SIZE*sizeof(uint64_t));
  ctype_cbblack_rel(cts, tab);
  ctype_cbblack_size_rel(cts, CTCBBLACK_SIZE);
  return tab;
}

static LJ_AINLINE uint64_t ctype_cbblack_key(void *func)
{
  return ((uintptr_t)func >> 2) | U64x(800000000, 00000000);
}

static LJ_AINLINE MSize ctype_cbblack_hash(uint64_t key, MSize mask)
{
  return (MSize)(hashrot((uint32_t)key, (uint32_t)(key >> 32)) & mask);
}

static void ctype_cbblack_wait(lua_State *L)
{
  /*
  ** Callback-blacklist duplicate publication is a short CAS window. Runtime
  ** FFI call/callback paths have a current Lua state; use it so their TG is
  ** native and visible to safepoint handshakes while probing past a peer
  ** publisher. Test and teardown-only callers may still pass NULL.
  */
  (void)lj_thr_sleep_ns(L, 1000000);
}

void lj_ctype_cb_blacklist(lua_State *L, CTState *cts, void *func)
{
  uint64_t key = ctype_cbblack_key(func);
  uint64_t *tab = ctype_cbblack_acq(cts);
  MSize size = ctype_cbblack_size_acq(cts);
  MSize i, mask;
  if (LJ_UNLIKELY(tab == NULL || size == 0)) {
    ctype_cbblack_all_rel(cts, 1);
    return;
  }
  mask = size - 1u;
  for (i = 0; i < size; i++) {
    MSize slot = (ctype_cbblack_hash(key, mask) + i) & mask;
    uint64_t cur = ctype_cbblack_slot_acq(tab, slot);
    if (cur == key)
      return;
    if (cur == 0) {
      uint64_t expect = 0;
      if (ctype_cbblack_slot_cas(tab, slot, &expect, key))
	return;  /* 11.5 callback blacklist CAS publish. */
      if (expect == key)
	return;  /* Duplicate publisher won this slot. */
      ctype_cbblack_wait(L);  /* CAS loser: yield before probing on. */
    }
  }
  ctype_cbblack_all_rel(cts, 1);  /* Full set: blacklist all. */
}

int lj_ctype_cb_isblacklisted(CTState *cts, void *func)
{
  uint64_t key = ctype_cbblack_key(func);
  uint64_t *tab = ctype_cbblack_acq(cts);
  MSize size = ctype_cbblack_size_acq(cts);
  MSize i, mask;
  if (ctype_cbblack_all_acq(cts))
    return 1;
  if (LJ_UNLIKELY(tab == NULL || size == 0))
    return 0;
  mask = size - 1u;
  for (i = 0; i < size; i++) {
    MSize slot = (ctype_cbblack_hash(key, mask) + i) & mask;
    uint64_t cur = ctype_cbblack_slot_acq(tab, slot);
    if (cur == key)
      return 1;
    if (cur == 0)
      return 0;
  }
  return 1;
}

static LJ_AINLINE CTypeID ctype_hash_load(CTState *cts, uint32_t h)
{
  return ctype_hash_head_acq(cts, h);  /* 11.2: ctype hash publication. */
}

static LJ_AINLINE int ctype_hash_cas(CTState *cts, uint32_t h,
				     CTypeID *oldid, CTypeID newid)
{
  return ctype_hash_head_cas(cts, h, oldid,
			     newid);  /* 11.2: CAS-prepend ctype hash publication. */
}

static void ctype_hash_setnext(CTState *cts, CType *src, CTypeID id,
			       CTypeID next)
{
  CType *tab, *dst;
  do {
    dst = ctype_get(cts, id);
    if (dst != src)
      ctype_copy_rel(dst, src);
    ctype_next_rel(dst, next);
    tab = ctype_tab_acq(cts);
  } while (dst != &tab[id]);
}

CType *lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  CType *tab, *dst;
  do {
    dst = ctype_get(cts, id);
    if (dst != src)
      ctype_copy_rel(dst, src);
    tab = ctype_tab_acq(cts);
  } while (dst != &tab[id]);
  return dst;
}

static int ctype_hash_try_prepend(CTState *cts, uint32_t h, CType *src,
				  CTypeID id, CTypeID *head)
{
  ctype_hash_setnext(cts, src, id, *head);
  return ctype_hash_cas(cts, h, head, id);
}

static void ctype_hash_prepend(CTState *cts, uint32_t h, CType *src, CTypeID id)
{
  CTypeID head = ctype_hash_load(cts, h);
  if (id == 0)
    return;  /* CTID 0 is the hash-chain sentinel. */
  while (!ctype_hash_try_prepend(cts, h, src, id, &head))
    ;
}

static CTypeID ctype_hash_findtype(CTState *cts, CTypeID id, CTInfo info,
				   CTSize size)
{
  while (id) {
    CType *ct = ctype_get(cts, id);
    CTInfo cinfo = ctype_info_acq(ct);
    CTSize csize = ctype_size_acq(ct);
    if (!ctype_isabandoned(cinfo) && cinfo == info && csize == size)
      return id;
    id = ctype_next_acq(ct);
  }
  return 0;
}

static CTypeID ctype_hash_findname(CTState *cts, CTypeID id, GCstr *name,
				   uint32_t tmask)
{
  while (id) {
    CType *ct = ctype_get(cts, id);
    CTInfo info = ctype_info_acq(ct);
    if (!ctype_isabandoned(info) && ctype_name_acq(ct) == name &&
	((tmask >> ctype_type(info)) & 1))
      return id;
    id = ctype_next_acq(ct);
  }
  return 0;
}

static void ctype_abandon(CTState *cts, CTypeID id)
{
  CType tmp = *ctype_get(cts, id);
  tmp.info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD));
  tmp.size = 0;
  tmp.sib = 0;
  ctype_clearname(&tmp);
  /* Keep ct->next so hash walkers can skip through abandoned entries. */
  lj_ctype_publish(cts, id, &tmp);
  lj_assertCTS(ctype_isabandoned(ctype_info_acq(ctype_get(cts, id))),
	       "abandoned ctype not visible in current table");
}

static GCSize ctype_tab_size(MSize sizetab)
{
  return (GCSize)(sizeof(CTypeTab) + (sizetab - 1u)*sizeof(CType));
}

static CTypeTab *ctype_tab_new(lua_State *L, MSize sizetab)
{
  CTypeTab *tabh = (CTypeTab *)lj_mem_new(L, ctype_tab_size(sizetab));
  ctype_tab_sizetab_rel(tabh, sizetab);
  ctype_tab_retire_epoch_rel(tabh, 0);
  ctype_tab_retired_next_rel(tabh, NULL);
  return tabh;
}

static void ctype_tab_free(global_State *g, CTypeTab *tabh)
{
  lj_mem_free(g, tabh, ctype_tab_size(ctype_tab_sizetab_acq(tabh)));
}

static void ctype_tab_retired_push(CTState *cts, CTypeTab *ret)
{
  CTypeTab *head = ctype_retiredtab_acq(cts);
  do {
    ctype_tab_retired_next_rel(ret, head);
  } while (!ctype_retiredtab_cas(cts, &head,
				 ret));  /* 11.2 CTState table SMR. */
}

static void ctype_tab_retire(CTState *cts, CTypeTab *ret)
{
  ctype_tab_retire_epoch_rel(ret, lj_gc2_retire_epoch(cts->g));
  ctype_tab_retired_push(cts, ret);
}

static MSize ctype_tab_growsize(MSize osz, CTypeID id)
{
  MSize nsz = osz << 1;
  if (nsz < LJ_MIN_VECSZ)
    nsz = LJ_MIN_VECSZ;
  if (nsz <= (MSize)id)
    nsz = (MSize)id + 1u;
  if (nsz > CTID_MAX)
    nsz = CTID_MAX;
  return nsz;
}

static CType *ctype_tab_grow_l(lua_State *L, CTState *cts, CTypeID id)
{
  if (id >= CTID_MAX) lj_err_msg(L, LJ_ERR_TABOV);
  for (;;) {
    CTypeTab *oldh = ctype_tabh_acq(cts);
    MSize osz = ctype_tab_sizetab_acq(oldh);
    MSize nsz;
    CTypeTab *newh;
    CTypeTab *expect;
    if ((MSize)id < osz)
      return oldh->tab;
    nsz = ctype_tab_growsize(osz, id);
    newh = ctype_tab_new(L, nsz);
    memcpy(newh->tab, oldh->tab, osz*sizeof(CType));
    memset(newh->tab + osz, 0, (nsz - osz)*sizeof(CType));
    expect = oldh;
    if (ctype_tabh_cas(cts, &expect, newh)) {
      ctype_tab_retire(cts, oldh);
      return newh->tab;
    }
    ctype_tab_free(cts->g, newh);
  }
}

static CTypeID ctype_top_reserve_l(lua_State *L, CTState *cts, CType **ctp)
{
  for (;;) {
    CTypeTab *tabh = ctype_tabh_acq(cts);
    CTypeID id = ctype_top_acq(cts);
    uint32_t expect = id;
    if (LJ_UNLIKELY(id >= CTID_MAX)) lj_err_msg(L, LJ_ERR_TABOV);
    if (LJ_UNLIKELY((MSize)id >= ctype_tab_sizetab_acq(tabh))) {
      (void)ctype_tab_grow_l(L, cts, id);
      continue;
    }
    if (ctype_top_cas(cts, &expect, id+1u)) {
      *ctp = ctype_tab_slot(tabh, id);
      return id;
    }
  }
}

/* Create new type element. */
CTypeID lj_ctype_new_l(lua_State *L, CTState *cts, CType **ctp)
{
  CTypeID id = ctype_top_reserve_l(L, cts, ctp);
  CType *ct;
  ct = *ctp;
  ctype_info_rel(ct, 0);
  ctype_size_rel(ct, 0);
  ctype_sib_rel(ct, 0);
  ctype_next_rel(ct, 0);
  ctype_clearname(ct);
  return id;
}

static CTypeID ctype_intern_l(lua_State *L, CTState *cts, CTInfo info,
			      CTSize size, int *newp)
{
  uint32_t h = ct_hashtype(info, size);
  CTypeID head = ctype_hash_load(cts, h);
  CTypeID id = ctype_hash_findtype(cts, head, info, size);
  if (newp) *newp = 0;
  if (id)
    return id;
  {
    CType *ct;
    id = ctype_top_reserve_l(L, cts, &ct);
    ctype_info_rel(ct, info);
    ctype_size_rel(ct, size);
    ctype_sib_rel(ct, 0);
    ctype_next_rel(ct, 0);
    ctype_clearname(ct);
    head = ctype_hash_load(cts, h);
    for (;;) {
      CTypeID winner = ctype_hash_findtype(cts, head, info, size);
      if (winner) {
	ctype_abandon(cts, id);
	return winner;
      }
      if (ctype_hash_try_prepend(cts, h, ct, id, &head)) {
	if (newp) *newp = 1;
	return id;
      }
    }
  }
}

/* Intern a type element. */
CTypeID lj_ctype_intern_l(lua_State *L, CTState *cts, CTInfo info, CTSize size)
{
  return ctype_intern_l(L, cts, info, size, NULL);
}

CTypeID lj_ctype_intern_new_l(lua_State *L, CTState *cts, CTInfo info,
			      CTSize size, int *newp)
{
  return ctype_intern_l(L, cts, info, size, newp);
}

/* Add type element to hash table. */
static void ctype_addtype(CTState *cts, CType *ct, CTypeID id)
{
  uint32_t h = ct_hashtype(ctype_info_acq(ct), ctype_size_acq(ct));
  ctype_hash_prepend(cts, h, ct, id);
}

/* Add named element to hash table. */
void lj_ctype_addname(CTState *cts, CType *ct, CTypeID id)
{
  uint32_t h = ct_hashname(ctype_name_acq(ct));
  ctype_hash_prepend(cts, h, ct, id);
}

/* Add named element to hash table, abandoning duplicate-name losers. */
CTypeID lj_ctype_addname_unique(CTState *cts, CType *ct, CTypeID id,
				uint32_t tmask)
{
  GCstr *name = ctype_name_acq(ct);
  uint32_t h = ct_hashname(name);
  CTypeID head = ctype_hash_load(cts, h);
  if (id == 0)
    return 0;  /* CTID 0 is the hash-chain sentinel. */
  for (;;) {
    CTypeID winner = ctype_hash_findname(cts, head, name, tmask);
    if (winner) {
      if (winner != id)
	ctype_abandon(cts, id);
      return winner;  /* 11.2 named ctype duplicate winner. */
    }
    if (ctype_hash_try_prepend(cts, h, ct, id, &head))
      return id;  /* 11.2 CAS-prepend named ctype publication. */
  }
}

/* Get a C type by name, matching the type mask. */
CTypeID lj_ctype_getname(CTState *cts, CType **ctp, GCstr *name, uint32_t tmask)
{
  CTypeID id = ctype_hash_findname(cts,
		   ctype_hash_load(cts, ct_hashname(name)), name, tmask);
  if (id) {
    *ctp = ctype_get(cts, id);
    return id;
  }
  *ctp = ctype_tab_slot(ctype_tabh_acq(cts), 0);  /* Simplify caller logic. */
  return 0;
}

/* Sequence-checked named ctype lookup for stable non-parser readers. */
int lj_ctype_getname_snapshot(CTState *cts, GCstr *name, uint32_t tmask,
			      CTypeID *idp, CType *out, GCstr **redirp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top, id;
  MSize budget;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  id = ctype_hash_load(cts, ct_hashname(name));
  budget = top ? (MSize)top * 2u : 1u;
  while (id) {
    CType *ct;
    CTInfo info;
    CTSize size;
    CTypeID sib, next;
    GCobj *gco;
    if (id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (budget-- == 0)
      return -1;
    ct = ctype_tab_slot(tabh, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    sib = ctype_sib_acq(ct);
    next = ctype_next_acq(ct);
    gco = ctype_nameobj_acq(ct);
    if (!ctype_isabandoned(info) && gco == obj2gco(name) &&
	((tmask >> ctype_type(info)) & 1)) {
      GCstr *redir = name;
      uint32_t seq1;
      if (redirp && sib) {
	CType *rt;
	CTInfo rinfo;
	GCobj *rgco;
	if (sib >= top || (MSize)sib >= ctype_tab_sizetab_acq(tabh))
	  return -1;
	rt = ctype_tab_slot(tabh, sib);
	rinfo = ctype_info_acq(rt);
	rgco = ctype_nameobj_acq(rt);
	if (ctype_isabandoned(rinfo))
	  return 0;
	if (ctype_isxattrib(rinfo, CTA_REDIR)) {
	  if (rgco == NULL)
	    return -1;
	  redir = gco2str(rgco);
	}
      }
      out->info = info;
      out->size = size;
      out->sib = (CTypeID1)sib;
      out->next = (CTypeID1)next;
      setgcrefp(out->name, gco);
      *idp = id;
      if (redirp)
	*redirp = redir;
      seq1 = ctype_parse_token_acq(cts);
      return (seq0 == seq1 && !(seq1 & 1u)) ? 1 : -1;
    }
    id = next;
  }
  {
    uint32_t seq1 = ctype_parse_token_acq(cts);
    return (seq0 == seq1 && !(seq1 & 1u)) ? 0 : -1;
  }
}

int lj_ctype_getname_wait(lua_State *L, CTState *cts, GCstr *name,
			  uint32_t tmask, CTypeID *idp, CType *out,
			  GCstr **redirp)
{
  for (;;) {
    int ok = lj_ctype_getname_snapshot(cts, name, tmask, idp, out, redirp);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* Get a struct/union/enum/function field by name. */
CType *lj_ctype_getfieldq(CTState *cts, CType *ct, GCstr *name, CTSize *ofs,
			  CTInfo *qual)
{
  for (;;) {
    CTypeID sib = ctype_sib_acq(ct);
    if (!sib)
      break;
    ct = ctype_get(cts, sib);
    if (ctype_name_acq(ct) == name) {
      *ofs = ctype_size_acq(ct);
      return ct;
    }
    if (ctype_isxattrib(ctype_info_acq(ct), CTA_SUBTYPE)) {
      CType *fct, *cct = ctype_child(cts, ct);
      CTInfo q = 0;
      for (;;) {
	CTInfo cinfo = ctype_info_acq(cct);
	if (!ctype_isattrib(cinfo))
	  break;
	if (ctype_attrib(cinfo) == CTA_QUAL) q |= ctype_size_acq(cct);
	cct = ctype_child(cts, cct);
      }
      fct = lj_ctype_getfieldq(cts, cct, name, ofs, qual);
      if (fct) {
	if (qual) *qual |= q;
	*ofs += ctype_size_acq(ct);
	return fct;
      }
    }
  }
  return NULL;  /* Not found. */
}

static int ctype_snapshot_copy(CTypeTab *tabh, CTypeID top, CTypeID id,
			       CType *out)
{
  CType *ct;
  GCobj *gco;
  if (id == 0 || id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
    return 0;
  ct = ctype_tab_slot(tabh, id);
  out->info = ctype_info_acq(ct);
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  gco = ctype_nameobj_acq(ct);
  setgcrefp(out->name, gco);
  return !ctype_isabandoned(ctype_info_acq(out));
}

static int ctype_getfieldq_snapshot_rec(CTypeTab *tabh, CTypeID top,
					CTypeID sib, GCstr *name,
					CTSize baseofs, CTInfo basequal,
					CTSize *ofsp, CTInfo *qualp,
					CType *out, MSize *budget)
{
  while (sib) {
    CType *ct;
    CTInfo info;
    CTSize size;
    CTypeID child, next;
    GCobj *gco;
    if (sib >= top || (MSize)sib >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if ((*budget)-- == 0)
      return -1;
    ct = ctype_tab_slot(tabh, sib);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    child = ctype_cid(info);
    next = ctype_sib_acq(ct);
    gco = ctype_nameobj_acq(ct);
    if (ctype_isabandoned(info))
      return 0;
    if (gco == obj2gco(name)) {
      if (!ctype_snapshot_copy(tabh, top, sib, out))
	return 0;
      *ofsp = baseofs + size;
      if (qualp)
	*qualp |= basequal;
      return 1;
    }
    if (ctype_isxattrib(info, CTA_SUBTYPE)) {
      CTInfo q = basequal;
      CTypeID cid = child;
      CType cct;
      int ok;
      for (;;) {
	CTInfo cinfo;
	if (cid >= top || (MSize)cid >= ctype_tab_sizetab_acq(tabh))
	  return -1;
	if ((*budget)-- == 0)
	  return -1;
	if (!ctype_snapshot_copy(tabh, top, cid, &cct))
	  return 0;
	cinfo = ctype_info_acq(&cct);
	if (!ctype_isattrib(cinfo))
	  break;
	if (ctype_attrib(cinfo) == CTA_QUAL)
	  q |= ctype_size_acq(&cct);
	cid = ctype_cid(cinfo);
      }
      ok = ctype_getfieldq_snapshot_rec(tabh, top, ctype_sib_acq(&cct),
					name, baseofs + size, q, ofsp,
					qualp, out, budget);
      if (ok)
	return ok;
    }
    sib = next;
  }
  return 0;
}

/* Sequence-checked struct/union field lookup for stable readers. */
int lj_ctype_getfieldq_snapshot(CTState *cts, const CType *root,
				GCstr *name, CTSize *ofsp, CTInfo *qualp,
				CType *out)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top, sib;
  CTInfo info;
  CTInfo qual = qualp ? *qualp : 0;
  MSize budget;
  int ok;
  if (seq0 & 1u)
    return -1;
  info = ctype_info_acq(root);
  if (!ctype_isstruct(info))
    return ctype_snapshot_done(cts, seq0, 0);
  sib = ctype_sib_acq(root);
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 4u : 1u;
  ok = ctype_getfieldq_snapshot_rec(tabh, top, sib, name, 0, 0, ofsp,
				    qualp ? &qual : NULL, out, &budget);
  if (ok >= 0) {
    uint32_t seq1 = ctype_parse_token_acq(cts);
    ok = (seq0 == seq1 && !(seq1 & 1u)) ? ok : -1;
    if (ok >= 0 && qualp)
      *qualp = qual;
  }
  return ok;
}

static int ctype_getfieldq_snapshot_id(CTState *cts, CTypeID rootid,
				       GCstr *name, CTSize *ofsp,
				       CTInfo *qualp, CType *out)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top, sib;
  CType root;
  CTInfo info;
  CTInfo qual = qualp ? *qualp : 0;
  MSize budget;
  int ok;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  if (!ctype_snapshot_copy(tabh, top, rootid, &root))
    return ctype_snapshot_done(cts, seq0, 0);
  info = ctype_info_acq(&root);
  if (!ctype_isstruct(info))
    return ctype_snapshot_done(cts, seq0, 0);
  sib = ctype_sib_acq(&root);
  budget = top ? (MSize)top * 4u : 1u;
  ok = ctype_getfieldq_snapshot_rec(tabh, top, sib, name, 0, 0, ofsp,
				    qualp ? &qual : NULL, out, &budget);
  if (ok >= 0) {
    ok = ctype_snapshot_done(cts, seq0, ok);
    if (ok >= 0 && qualp)
      *qualp = qual;
  }
  return ok;
}

int lj_ctype_getfieldq_wait(lua_State *L, CTState *cts, CTypeID rootid,
			    GCstr *name, CTSize *ofsp, CTInfo *qualp,
			    CType *out)
{
  for (;;) {
    int ok = ctype_getfieldq_snapshot_id(cts, rootid, name, ofsp, qualp,
					 out);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* Sequence-checked pointer-to-struct test for stable auto-deref readers. */
int lj_ctype_ptrstruct_snapshot(CTState *cts, CTypeID id, CTypeID *cidp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top;
  MSize budget;
  CType ct;
  CTInfo info;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 2u : 1u;
  if (id == 0 || id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
    return -1;
  if (!ctype_snapshot_copy(tabh, top, id, &ct))
    return ctype_snapshot_done(cts, seq0, 0);
  info = ctype_info_acq(&ct);
  if (!ctype_isptr(info))
    return ctype_snapshot_done(cts, seq0, 0);
  id = ctype_cid(info);
  for (;;) {
    if (id == 0 || id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return ctype_snapshot_done(cts, seq0, 0);
    info = ctype_info_acq(&ct);
    if (!ctype_isattrib(info))
      break;
    id = ctype_cid(info);
  }
  {
    uint32_t seq1 = ctype_parse_token_acq(cts);
    if (seq0 != seq1 || (seq1 & 1u))
      return -1;
  }
  if (ctype_isstruct(info)) {
    *cidp = id;
    return 1;
  }
  return 0;
}

int lj_ctype_info_predefined(CTState *cts, CTypeID id, CTInfo *infop,
			     CTSize *szp, CTypeID *ridp, CType *rawp);

/* Sequence-checked type info/raw-type snapshot for stable layout readers. */
int lj_ctype_info_snapshot(CTState *cts, CTypeID id, CTInfo *infop,
			   CTSize *szp, CTypeID *ridp, CType *rawp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top;
  MSize budget;
  CType ct;
  CTInfo qual = 0;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 4u : 1u;
  if (rawp || ridp) {
    CTypeID rid = id;
    for (;;) {
      CTInfo info;
      if (budget-- == 0)
	return -1;
      if (!ctype_snapshot_copy(tabh, top, rid, &ct))
	return ctype_snapshot_done(cts, seq0, 0);
      info = ctype_info_acq(&ct);
      if (!ctype_isattrib(info)) {
	if (ridp)
	  *ridp = rid;
	if (rawp)
	  *rawp = ct;
	break;
      }
      rid = ctype_cid(info);
    }
  }
  for (;;) {
    CTInfo info;
    CTSize size;
    if (budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return ctype_snapshot_done(cts, seq0, 0);
    info = ctype_info_acq(&ct);
    size = ctype_size_acq(&ct);
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(size);
    } else {
      uint32_t seq1;
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      seq1 = ctype_parse_token_acq(cts);
      return (seq0 == seq1 && !(seq1 & 1u)) ? 1 : -1;
    }
    id = ctype_cid(info);
  }
}

int lj_ctype_info_wait(lua_State *L, CTState *cts, CTypeID id,
		       CTInfo *infop, CTSize *szp, CTypeID *ridp,
		       CType *rawp)
{
  int ok = lj_ctype_info_predefined(cts, id, infop, szp, ridp, rawp);
  if (ok > 0)
    return ok;
  for (;;) {
    ok = lj_ctype_info_snapshot(cts, id, infop, szp, ridp, rawp);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* Sequence-checked ctype metamethod lookup for stable ctype readers. */
int lj_ctype_metatv_snapshot(CTState *cts, TValue *out, CTypeID id, MMS mm)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top;
  CType ct;
  CTInfo info;
  MSize budget;
  cTValue *tv = NULL;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 4u : 1u;
  for (;;) {
    if (budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return ctype_snapshot_done(cts, seq0, 0);
    info = ctype_info_acq(&ct);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      break;
    id = ctype_cid(info);
  }
  if (ctype_isptr(info)) {
    CType cct;
    CTypeID cid = ctype_cid(info);
    if (budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, cid, &cct))
      return ctype_snapshot_done(cts, seq0, 0);
    if (ctype_isfunc(ctype_info_acq(&cct))) {
      GCtab *miscmap;
      TValue tabv;
      if (ctype_snapshot_done(cts, seq0, 1) < 0)
	return -1;
      miscmap = ctype_miscmap_acq(cts);
      tv = miscmap ? lj_tab_getstr(miscmap, &cts->g->strempty) : NULL;
      if (tv) {
	lj_tv_load_acq(&tabv, tv);
	if (tvistab(&tabv)) {
	  tv = lj_tab_getstr(tabV(&tabv), mmname_str(cts->g, mm));
	  if (tv) {
	    lj_tv_load_acq(out, tv);
	    return tvisnil(out) ? 0 : 1;
	  }
	}
      }
      setnilV(out);
      return 0;
    }
  }
  if (ctype_snapshot_done(cts, seq0, 1) < 0)
    return -1;
  {
    GCtab *mt = ctype_meta_tab(cts, id);
    if (mt) {
      tv = lj_tab_getstr(mt, mmname_str(cts->g, mm));
      if (tv) {
	lj_tv_load_acq(out, tv);
	return tvisnil(out) ? 0 : 1;
      }
    }
  }
  setnilV(out);
  return 0;
}

cTValue *lj_ctype_metatv_wait(lua_State *L, CTState *cts, TValue *out,
			      CTypeID id, MMS mm)
{
  if (lj_ctype_predefined_nometa(cts, id)) {
    setnilV(out);
    return NULL;
  }
  for (;;) {
    int ok = lj_ctype_metatv_snapshot(cts, out, id, mm);
    if (ok > 0)
      return out;
    if (ok == 0)
      return NULL;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* -- C type information -------------------------------------------------- */

/* Follow references and get raw type for a C type ID. */
CType *lj_ctype_rawref(CTState *cts, CTypeID id)
{
  return ctype_get(cts, ctype_rawrefid(cts, id));
}

/* Sequence-checked raw-ref type snapshot for stable recorder readers. */
int lj_ctype_rawref_snapshot(CTState *cts, CTypeID id, CTypeID *ridp,
			     CType *out)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top;
  MSize budget;
  CType ct;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 4u : 1u;
  for (;;) {
    CTInfo info;
    if (budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return ctype_snapshot_done(cts, seq0, 0);
    info = ctype_info_acq(&ct);
    if (!(ctype_isattrib(info) || ctype_isref(info))) {
      if (ridp)
	*ridp = id;
      if (out)
	*out = ct;
      return ctype_snapshot_done(cts, seq0, 1);
    }
    id = ctype_cid(info);
  }
}

/* Get size for a C type ID. Does NOT support VLA/VLS. */
CTSize lj_ctype_size(CTState *cts, CTypeID id)
{
  CType *ct = ctype_raw(cts, id);
  CTInfo info = ctype_info_acq(ct);
  return ctype_hassize(info) ? ctype_size_acq(ct) : CTSIZE_INVALID;
}

/* Sequence-checked size snapshot for stable non-parser ctype readers. */
int lj_ctype_size_snapshot(CTState *cts, CTypeID id, CTSize *szp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top;
  MSize budget;
  if (seq0 & 1u)
    return -1;
  if (id == 0)
    return ctype_snapshot_done(cts, seq0, 0);
  top = ctype_top_acq(cts);
  if (id >= top)
    return ctype_snapshot_done(cts, seq0, 0);
  tabh = ctype_tabh_acq(cts);
  if ((MSize)id >= ctype_tab_sizetab_acq(tabh))
    return -1;
  budget = top ? (MSize)top * 2u : 1u;
  for (;;) {
    CType *ct;
    CTInfo info;
    CTSize size;
    if (id == 0 || id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (budget-- == 0)
      return -1;
    ct = ctype_tab_slot(tabh, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    if (ctype_isabandoned(info))
      return ctype_snapshot_done(cts, seq0, 0);
    if (!ctype_isattrib(info)) {
      uint32_t seq1;
      *szp = ctype_hassize(info) ? size : CTSIZE_INVALID;
      seq1 = ctype_parse_token_acq(cts);
      return (seq0 == seq1 && !(seq1 & 1u)) ? 1 : -1;
    }
    id = ctype_cid(info);
  }
}

static int ctype_predefined_id(CTypeID id)
{
  return id > CTID_NONE && id <= CTID_CTYPEID;
}

int lj_ctype_rawref_predefined(CTState *cts, CTypeID id, CTypeID *ridp,
			       CType *out)
{
  CTypeTab *tabh;
  CTypeID top = CTID_CTYPEID + 1;
  MSize budget = (MSize)top * 4u;
  CType ct;
  if (!ctype_predefined_id(id))
    return -1;
  tabh = ctype_tabh_acq(cts);
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(tabh))
    return -1;
  for (;;) {
    CTInfo info;
    if (!ctype_predefined_id(id) || budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return 0;
    info = ctype_info_acq(&ct);
    if (!(ctype_isattrib(info) || ctype_isref(info))) {
      if (ridp)
	*ridp = id;
      if (out)
	*out = ct;
      return 1;
    }
    id = ctype_cid(info);
  }
}

int lj_ctype_predefined_nometa(CTState *cts, CTypeID id)
{
  CTypeTab *tabh;
  CType *ct;
  CTInfo info;
  if (!ctype_predefined_id(id))
    return 0;
  tabh = ctype_tabh_acq(cts);
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(tabh))
    return 0;
  ct = ctype_tab_slot(tabh, id);
  info = ctype_info_acq(ct);
  if (ctype_isabandoned(info) || ctype_isstruct(info) ||
      ctype_iscomplex(info) || ctype_isvector(info))
    return 0;
  if (ctype_isptr(info)) {
    CTypeID cid = ctype_cid(info);
    if (!ctype_predefined_id(cid))
      return 0;
    info = ctype_info_acq(ctype_tab_slot(tabh, cid));
    if (ctype_isabandoned(info) || ctype_isfunc(info))
      return 0;
  }
  return 1;
}

int lj_ctype_info_predefined(CTState *cts, CTypeID id, CTInfo *infop,
			     CTSize *szp, CTypeID *ridp, CType *rawp)
{
  CTypeTab *tabh;
  CTypeID top = CTID_CTYPEID + 1;
  MSize budget = (MSize)top * 4u;
  CType ct;
  CTInfo qual = 0;
  if (!ctype_predefined_id(id))
    return 0;
  tabh = ctype_tabh_acq(cts);
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(tabh))
    return 0;
  if (rawp || ridp) {
    CTypeID rid = id;
    for (;;) {
      CTInfo info;
      if (!ctype_predefined_id(rid) || budget-- == 0)
	return 0;
      if (!ctype_snapshot_copy(tabh, top, rid, &ct))
	return 0;
      info = ctype_info_acq(&ct);
      if (!ctype_isattrib(info)) {
	if (ridp)
	  *ridp = rid;
	if (rawp)
	  *rawp = ct;
	break;
      }
      rid = ctype_cid(info);
    }
  }
  for (;;) {
    CTInfo info;
    CTSize size;
    if (!ctype_predefined_id(id) || budget-- == 0)
      return 0;
    if (!ctype_snapshot_copy(tabh, top, id, &ct))
      return 0;
    info = ctype_info_acq(&ct);
    size = ctype_size_acq(&ct);
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(size);
    } else {
      if (!(qual & CTFP_ALIGNED))
	qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      return 1;
    }
    id = ctype_cid(info);
  }
}

int lj_ctype_size_predefined(CTState *cts, CTypeID id, CTSize *szp)
{
  CTypeTab *tabh;
  CTypeID top = CTID_CTYPEID + 1;
  MSize budget = (MSize)top * 2u;
  if (!ctype_predefined_id(id))
    return -1;
  tabh = ctype_tabh_acq(cts);
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(tabh))
    return -1;
  for (;;) {
    CType ct;
    CTInfo info;
    CTSize size;
    if (!ctype_predefined_id(id) || budget-- == 0)
      return -1;
    if (!ctype_snapshot_copy(tabh, top, id, &ct)) {
      *szp = CTSIZE_INVALID;
      return 0;
    }
    info = ctype_info_acq(&ct);
    size = ctype_size_acq(&ct);
    if (!ctype_isattrib(info)) {
      *szp = ctype_hassize(info) ? size : CTSIZE_INVALID;
      return 1;
    }
    id = ctype_cid(info);
  }
}

/* Wait/retry helper for scalar size only. Callers must refetch CType pointers
** after this returns if they need to retain or return a CType across the wait.
*/
int lj_ctype_size_wait(lua_State *L, CTState *cts, CTypeID id, CTSize *szp)
{
  int ok = lj_ctype_size_predefined(cts, id, szp);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = lj_ctype_size_snapshot(cts, id, szp);
    if (ok > 0)
      return 1;
    if (ok == 0) {
      *szp = CTSIZE_INVALID;
      return 0;
    }
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* Sequence-checked enum string constant lookup for stable readers. */
int lj_ctype_enumconst_snapshot(CTState *cts, const CType *root,
				GCstr *name, CTSize *valp, CTypeID *cidp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top, id;
  MSize budget;
  if (seq0 & 1u)
    return -1;
  if (!ctype_isenum(ctype_info_acq(root)))
    return ctype_snapshot_done(cts, seq0, 0);
  id = ctype_sib_acq(root);
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  budget = top ? (MSize)top * 2u : 1u;
  while (id) {
    CType *ct;
    CTInfo info;
    CTSize size;
    GCobj *gco;
    if (id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (budget-- == 0)
      return -1;
    ct = ctype_tab_slot(tabh, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    gco = ctype_nameobj_acq(ct);
    if (ctype_isabandoned(info))
      return ctype_snapshot_done(cts, seq0, 0);
    if (gco == obj2gco(name) && ctype_isconstval(info)) {
      uint32_t seq1;
      *valp = size;
      *cidp = ctype_cid(info);
      seq1 = ctype_parse_token_acq(cts);
      return (seq0 == seq1 && !(seq1 & 1u)) ? 1 : -1;
    }
    id = ctype_sib_acq(ct);
  }
  {
    return ctype_snapshot_done(cts, seq0, 0);
  }
}

static int ctype_enumconst_snapshot_id(CTState *cts, CTypeID rootid,
				       GCstr *name, CTSize *valp,
				       CTypeID *cidp)
{
  uint32_t seq0 = ctype_parse_token_acq(cts);
  CTypeTab *tabh;
  CTypeID top, id;
  CType root;
  MSize budget;
  if (seq0 & 1u)
    return -1;
  top = ctype_top_acq(cts);
  tabh = ctype_tabh_acq(cts);
  if (!ctype_snapshot_copy(tabh, top, rootid, &root))
    return ctype_snapshot_done(cts, seq0, 0);
  if (!ctype_isenum(ctype_info_acq(&root)))
    return ctype_snapshot_done(cts, seq0, 0);
  id = ctype_sib_acq(&root);
  budget = top ? (MSize)top * 2u : 1u;
  while (id) {
    CType *ct;
    CTInfo info;
    CTSize size;
    GCobj *gco;
    if (id >= top || (MSize)id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (budget-- == 0)
      return -1;
    ct = ctype_tab_slot(tabh, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    gco = ctype_nameobj_acq(ct);
    if (ctype_isabandoned(info))
      return ctype_snapshot_done(cts, seq0, 0);
    if (gco == obj2gco(name) && ctype_isconstval(info)) {
      *valp = size;
      *cidp = ctype_cid(info);
      return ctype_snapshot_done(cts, seq0, 1);
    }
    id = ctype_sib_acq(ct);
  }
  return ctype_snapshot_done(cts, seq0, 0);
}

int lj_ctype_enumconst_wait(lua_State *L, CTState *cts, CTypeID rootid,
			    GCstr *name, CTSize *valp, CTypeID *cidp)
{
  for (;;) {
    int ok = ctype_enumconst_snapshot_id(cts, rootid, name, valp, cidp);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

/* Get size for a variable-length C type. Does NOT support other C types. */
CTSize lj_ctype_vlsize(CTState *cts, CType *ct, CTSize nelem)
{
  uint64_t xsz = 0;
  CTInfo info = ctype_info_acq(ct);
  CTSize size = ctype_size_acq(ct);
  if (ctype_isstruct(info)) {
    CTypeID arrid = 0, fid = ctype_sib_acq(ct);
    xsz = size;  /* Add the struct size. */
    while (fid) {
      CType *ctf = ctype_get(cts, fid);
      CTInfo finfo = ctype_info_acq(ctf);
      if (ctype_type(finfo) == CT_FIELD)
	arrid = ctype_cid(finfo);  /* Remember last field of VLS. */
      fid = ctype_sib_acq(ctf);
    }
    ct = ctype_raw(cts, arrid);
    info = ctype_info_acq(ct);
  }
  lj_assertCTS(ctype_isvlarray(info), "VLA expected");
  ct = ctype_rawchild(cts, ct);  /* Get array element. */
  info = ctype_info_acq(ct);
  size = ctype_size_acq(ct);
  lj_assertCTS(ctype_hassize(info), "bad VLA without size");
  /* Calculate actual size of VLA and check for overflow. */
  xsz += (uint64_t)size * nelem;
  return xsz < 0x80000000u ? (CTSize)xsz : CTSIZE_INVALID;
}

/* Get type, qualifiers, size and alignment for a C type ID. */
CTInfo lj_ctype_info(CTState *cts, CTypeID id, CTSize *szp)
{
  CTInfo qual = 0;
  CType *ct = ctype_get(cts, id);
  for (;;) {
    CTInfo info = ctype_info_acq(ct);
    CTSize size = ctype_size_acq(ct);
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      lj_assertCTS(ctype_hassize(info) || ctype_isfunc(info),
		   "ctype without size");
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      break;
    }
    ct = ctype_get(cts, ctype_cid(info));
  }
  return qual;
}

/* Ditto, but follow a reference. */
CTInfo lj_ctype_info_raw(CTState *cts, CTypeID id, CTSize *szp)
{
  CType *ct = ctype_get(cts, id);
  CTInfo info = ctype_info_acq(ct);
  if (ctype_isref(info)) id = ctype_cid(info);
  return lj_ctype_info(cts, id, szp);
}

/* Get ctype metamethod. */
cTValue *lj_ctype_meta(CTState *cts, CTypeID id, MMS mm)
{
  CType *ct = ctype_get(cts, id);
  cTValue *tv;
  CTInfo info;
  for (;;) {
    info = ctype_info_acq(ct);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      break;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
  }
  if (ctype_isptr(info) &&
      ctype_isfunc(ctype_info_acq(ctype_get(cts, ctype_cid(info)))))
    tv = lj_tab_getstr(ctype_miscmap_acq(cts), &cts->g->strempty);
  else {
    GCtab *mt = ctype_meta_tab(cts, id);
    tv = mt ? lj_tab_getstr(mt, mmname_str(cts->g, mm)) : NULL;
    return (tv && !tvisnil(tv)) ? tv : NULL;
  }
  if (tv && tvistab(tv) &&
      (tv = lj_tab_getstr(tabV(tv), mmname_str(cts->g, mm))) && !tvisnil(tv))
    return tv;
  return NULL;
}

cTValue *lj_ctype_metatv(CTState *cts, TValue *out, CTypeID id, MMS mm)
{
  CType *ct = ctype_get(cts, id);
  cTValue *tv;
  TValue tabv;
  CTInfo info;
  for (;;) {
    info = ctype_info_acq(ct);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      break;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
  }
  if (ctype_isptr(info) &&
      ctype_isfunc(ctype_info_acq(ctype_get(cts, ctype_cid(info)))))
    tv = lj_tab_getstr(ctype_miscmap_acq(cts), &cts->g->strempty);
  else {
    GCtab *mt = ctype_meta_tab(cts, id);
    if (mt) {
      tv = lj_tab_getstr(mt, mmname_str(cts->g, mm));
      if (tv) {
	lj_tv_load_acq(out, tv);
	return tvisnil(out) ? NULL : out;
      }
    }
    setnilV(out);
    return NULL;
  }
  if (tv) {
    lj_tv_load_acq(&tabv, tv);
    if (tvistab(&tabv)) {
      tv = lj_tab_getstr(tabV(&tabv), mmname_str(cts->g, mm));
      if (tv) {
	lj_tv_load_acq(out, tv);
	return tvisnil(out) ? NULL : out;
      }
    }
  }
  setnilV(out);
  return NULL;
}

/* -- C type representation ----------------------------------------------- */

/* Fixed max. length of a C type representation. */
#define CTREPR_MAX		512

typedef struct CTRepr {
  char *pb, *pe;
  CTState *cts;
  lua_State *L;
  int needsp;
  int ok;
  char buf[CTREPR_MAX];
} CTRepr;

/* Prepend string. */
static void ctype_prepstr(CTRepr *ctr, const char *str, MSize len)
{
  char *p = ctr->pb;
  if (ctr->buf + len+1 > p) { ctr->ok = 0; return; }
  if (ctr->needsp) *--p = ' ';
  ctr->needsp = 1;
  p -= len;
  while (len-- > 0) p[len] = str[len];
  ctr->pb = p;
}

#define ctype_preplit(ctr, str)	ctype_prepstr((ctr), "" str, sizeof(str)-1)

/* Prepend char. */
static void ctype_prepc(CTRepr *ctr, int c)
{
  if (ctr->buf >= ctr->pb) { ctr->ok = 0; return; }
  *--ctr->pb = c;
}

/* Prepend number. */
static void ctype_prepnum(CTRepr *ctr, uint32_t n)
{
  char *p = ctr->pb;
  if (ctr->buf + 10+1 > p) { ctr->ok = 0; return; }
  do { *--p = (char)('0' + n % 10); } while (n /= 10);
  ctr->pb = p;
  ctr->needsp = 0;
}

/* Append char. */
static void ctype_appc(CTRepr *ctr, int c)
{
  if (ctr->pe >= ctr->buf + CTREPR_MAX) { ctr->ok = 0; return; }
  *ctr->pe++ = c;
}

/* Append number. */
static void ctype_appnum(CTRepr *ctr, uint32_t n)
{
  char buf[10];
  char *p = buf+sizeof(buf);
  char *q = ctr->pe;
  if (q > ctr->buf + CTREPR_MAX - 10) { ctr->ok = 0; return; }
  do { *--p = (char)('0' + n % 10); } while (n /= 10);
  do { *q++ = *p++; } while (p < buf+sizeof(buf));
  ctr->pe = q;
}

/* Prepend qualifiers. */
static void ctype_prepqual(CTRepr *ctr, CTInfo info)
{
  if ((info & CTF_VOLATILE)) ctype_preplit(ctr, "volatile");
  if ((info & CTF_CONST)) ctype_preplit(ctr, "const");
}

/* Prepend named type. */
static void ctype_preptype(CTRepr *ctr, CTypeID id, CType *ct, CTInfo qual,
			   const char *t)
{
  GCstr *str = ctype_name_acq(ct);
  if (str) {
    ctype_prepstr(ctr, strdata(str), str->len);
  } else {
    if (ctr->needsp) ctype_prepc(ctr, ' ');
    ctype_prepnum(ctr, id);
    ctr->needsp = 1;
  }
  ctype_prepstr(ctr, t, (MSize)strlen(t));
  ctype_prepqual(ctr, qual);
}

static void ctype_repr(CTRepr *ctr, CTypeID id)
{
  CType *ct = ctype_get(ctr->cts, id);
  CTInfo qual = 0;
  int ptrto = 0;
  for (;;) {
    CTInfo info = ctype_info_acq(ct);
    CTSize size = ctype_size_acq(ct);
    switch (ctype_type(info)) {
    case CT_NUM:
      if ((info & CTF_BOOL)) {
	ctype_preplit(ctr, "bool");
      } else if ((info & CTF_FP)) {
	if (size == sizeof(double)) ctype_preplit(ctr, "double");
	else if (size == sizeof(float)) ctype_preplit(ctr, "float");
	else ctype_preplit(ctr, "long double");
      } else if (size == 1) {
	if (!((info ^ CTF_UCHAR) & CTF_UNSIGNED)) ctype_preplit(ctr, "char");
	else if (CTF_UCHAR) ctype_preplit(ctr, "signed char");
	else ctype_preplit(ctr, "unsigned char");
      } else if (size < 8) {
	if (size == 4) ctype_preplit(ctr, "int");
	else ctype_preplit(ctr, "short");
	if ((info & CTF_UNSIGNED)) ctype_preplit(ctr, "unsigned");
      } else {
	ctype_preplit(ctr, "_t");
	ctype_prepnum(ctr, size*8);
	ctype_preplit(ctr, "int");
	if ((info & CTF_UNSIGNED)) ctype_prepc(ctr, 'u');
      }
      ctype_prepqual(ctr, (qual|info));
      return;
    case CT_VOID:
      ctype_preplit(ctr, "void");
      ctype_prepqual(ctr, (qual|info));
      return;
    case CT_STRUCT:
      ctype_preptype(ctr, id, ct, qual, (info & CTF_UNION) ? "union" : "struct");
      return;
    case CT_ENUM:
      if (id == CTID_CTYPEID) {
	ctype_preplit(ctr, "ctype");
	return;
      }
      ctype_preptype(ctr, id, ct, qual, "enum");
      return;
    case CT_ATTRIB:
      if (ctype_attrib(info) == CTA_QUAL) qual |= size;
      break;
    case CT_PTR:
      if ((info & CTF_REF)) {
	ctype_prepc(ctr, '&');
      } else {
	ctype_prepqual(ctr, (qual|info));
	if (LJ_64 && size == 4) ctype_preplit(ctr, "__ptr32");
	ctype_prepc(ctr, '*');
      }
      qual = 0;
      ptrto = 1;
      ctr->needsp = 1;
      break;
    case CT_ARRAY:
      if (ctype_isrefarray(info)) {
	ctr->needsp = 1;
	if (ptrto) { ptrto = 0; ctype_prepc(ctr, '('); ctype_appc(ctr, ')'); }
	ctype_appc(ctr, '[');
	if (size != CTSIZE_INVALID) {
	  CTSize csize = ctype_size_acq(ctype_child(ctr->cts, ct));
	  ctype_appnum(ctr, csize ? size/csize : 0);
	} else if ((info & CTF_VLA)) {
	  ctype_appc(ctr, '?');
	}
	ctype_appc(ctr, ']');
      } else if ((info & CTF_COMPLEX)) {
	if (size == 2*sizeof(float)) ctype_preplit(ctr, "float");
	ctype_preplit(ctr, "complex");
	return;
      } else {
	ctype_preplit(ctr, ")))");
	ctype_prepnum(ctr, size);
	ctype_preplit(ctr, "__attribute__((vector_size(");
      }
      break;
    case CT_FUNC:
      ctr->needsp = 1;
      if (ptrto) { ptrto = 0; ctype_prepc(ctr, '('); ctype_appc(ctr, ')'); }
      ctype_appc(ctr, '(');
      ctype_appc(ctr, ')');
      break;
    default:
      lj_assertG_(ctr->cts->g, 0, "bad ctype %08x", info);
      break;
    }
    ct = ctype_get(ctr->cts, ctype_cid(info));
  }
}

/* Return a printable representation of a C type. */
GCstr *lj_ctype_repr(lua_State *L, CTypeID id, GCstr *name)
{
  global_State *g = G(L);
  CTRepr ctr;
  ctr.pb = ctr.pe = &ctr.buf[CTREPR_MAX/2];
  ctr.cts = ctype_ctsG(g);
  ctr.L = L;
  ctr.ok = 1;
  ctr.needsp = 0;
  if (name) ctype_prepstr(&ctr, strdata(name), name->len);
  ctype_repr(&ctr, id);
  if (LJ_UNLIKELY(!ctr.ok)) return lj_str_newlit(L, "?");
  return lj_str_new(L, ctr.pb, ctr.pe - ctr.pb);
}

/* Convert int64_t/uint64_t to string with 'LL' or 'ULL' suffix. */
GCstr *lj_ctype_repr_int64(lua_State *L, uint64_t n, int isunsigned)
{
  char buf[1+20+3];
  char *p = buf+sizeof(buf);
  int sign = 0;
  *--p = 'L'; *--p = 'L';
  if (isunsigned) {
    *--p = 'U';
  } else if ((int64_t)n < 0) {
    n = ~n+1u;
    sign = 1;
  }
  do { *--p = (char)('0' + n % 10); } while (n /= 10);
  if (sign) *--p = '-';
  return lj_str_new(L, p, (size_t)(buf+sizeof(buf)-p));
}

/* Convert complex to string with 'i' or 'I' suffix. */
GCstr *lj_ctype_repr_complex(lua_State *L, void *sp, CTSize size)
{
  SBuf *sb = lj_buf_tmp_(L);
  TValue re, im;
  if (size == 2*sizeof(double)) {
    re.n = *(double *)sp; im.n = ((double *)sp)[1];
  } else {
    re.n = (double)*(float *)sp; im.n = (double)((float *)sp)[1];
  }
  lj_strfmt_putfnum(sb, STRFMT_G14, re.n);
  if (!(im.u32.hi & 0x80000000u) || im.n != im.n) lj_buf_putchar(sb, '+');
  lj_strfmt_putfnum(sb, STRFMT_G14, im.n);
  lj_buf_putchar(sb, sb->w[-1] >= 'a' ? 'I' : 'i');
  return lj_buf_str(L, sb);
}

uint32_t lj_ctype_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  CTState *cts = ctype_ctsG(g);
  CTypeTab *ret;
  uint32_t reclaimed = 0;
  if (!cts || completed_epoch == 0)
    return 0;
  ret = ctype_retiredtab_xchg_acqrel(cts, NULL);
  while (ret) {
    CTypeTab *next = ctype_tab_retired_next_acq(ret);
    ctype_tab_retired_next_rel(ret, NULL);
    if (ctype_tab_retire_epoch_acq(ret) < completed_epoch) {
      ctype_tab_free(g, ret);
      reclaimed++;
    } else {
      ctype_tab_retired_push(cts, ret);
    }
    ret = next;
  }
  return reclaimed;
}

static void ctype_freeretired(global_State *g, CTState *cts)
{
  CTypeTab *ret = ctype_retiredtab_xchg_acqrel(cts, NULL);
  while (ret) {
    CTypeTab *next = ctype_tab_retired_next_acq(ret);
    ctype_tab_free(g, ret);
    ret = next;
  }
}

CTState *LJ_FASTCALL lj_ctype_ctsG_acq(global_State *g)
{
  return ctype_ctsG(g);
}

/* -- C type state -------------------------------------------------------- */

/* Initialize C type table and state. */
CTState *lj_ctype_init(lua_State *L)
{
  CTState *cts = lj_mem_newt(L, sizeof(CTState), CTState);
  CTypeTab *tabh = ctype_tab_new(L, CTTYPETAB_MIN);
  CType *ct = tabh->tab;
  const char *name = lj_ctype_typenames;
  CTypeID id;
  memset(cts, 0, sizeof(CTState));
  ctype_tabh_rel(cts, tabh);
  ctype_top_rel(cts, CTTYPEINFO_NUM);
  ctype_metamap_init_l(L, cts);
  ctype_cbblack_init_l(L, cts);
  cts->g = G(L);
  for (id = 0; id < CTTYPEINFO_NUM; id++, ct++) {
    CTInfo info = lj_ctype_typeinfo[id];
    ctype_size_rel(ct, (CTSize)((int32_t)(info << 16) >> 26));
    ctype_info_rel(ct, info & 0xffff03ffu);
    ctype_sib_rel(ct, 0);
    if (ctype_type(info) == CT_KW || ctype_istypedef(info)) {
      size_t len = strlen(name);
      GCstr *str = lj_str_new(L, name, len);
      ctype_setname(ct, str);
      name += len+1;
      lj_ctype_addname(cts, ct, id);
    } else {
      ctype_clearname(ct);
      ctype_next_rel(ct, 0);
      if (!ctype_isenum(info)) ctype_addtype(cts, ct, id);
    }
  }
  setmrefrel(G(L)->ctype_state, cts);  /* 11.2 CTState global publish. */
  {
    TValue *anchor = L->top;
    GCtab *t = ctype_fin_tab_new_l(L, 1);
    FinRegGen *gen;
    settabV(L, L->top++, t);
    gen = ctype_fin_gen_new_l(L, t);
    fin_gen_head_rel(cts, gen);
    L->top = anchor;
  }
  return cts;
}

/* Free C type table and state. */
void lj_ctype_freestate(global_State *g)
{
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    lj_ccallback_mcode_free(cts);
    lj_ctype_fin_freetabs(g, cts);
    ctype_freeretired(g, cts);
    ctype_tab_free(g, ctype_tabh_acq(cts));
    lj_mem_freevec(g, ctype_metamap_acq(cts),
		   ctype_metamap_size_acq(cts), GCRef);
    lj_mem_freevec(g, ctype_cbblack_acq(cts),
		   ctype_cbblack_size_acq(cts), uint64_t);
    lj_mem_freevec(g, ctype_cb_cbid_acq(cts), ctype_cb_sizeid_acq(cts),
		   CTypeID1);
    lj_mem_freevec(g, ctype_cb_owner_acq(cts), ctype_cb_sizeid_acq(cts),
		   lua_State *);
    lj_mem_freevec(g, ctype_cb_func_acq(cts), ctype_cb_sizeid_acq(cts),
		   TValue);
    lj_mem_freet(g, cts);
  }
}

#endif
