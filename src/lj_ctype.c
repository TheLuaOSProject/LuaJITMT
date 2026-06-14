/*
** C type management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_strfmt.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"
#include "lj_buf.h"
#include "lj_safepoint.h"

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

#define CTTYPEINFO_NUM		(sizeof(lj_ctype_typeinfo)/sizeof(CTInfo)-1)
#ifdef LUAJIT_CTYPE_CHECK_ANCHOR
#define CTTYPETAB_MIN		CTTYPEINFO_NUM
#else
#define CTTYPETAB_MIN		128
#endif

/* -- C type interning ---------------------------------------------------- */

#define ct_hashtype(info, size)	(hashrot(info, size) & CTHASH_MASK)
#define ct_hashname(name) \
  (hashrot(u32ptr(name), u32ptr(name) + HASH_BIAS) & CTHASH_MASK)

void lj_ctype_parse_lock(CTState *cts, lua_State *L)
{
  for (;;) {
    uint32_t expect = 0;
    if (la_cas32(&cts->parse_token, &expect, 1, LA_ACQ_REL, LA_ACQ)) {
      return;  /* 11.2: acquire the cparse CTState mutation token. */
    }
#if defined(__linux__)
    {
      uint32_t actions;
      lj_native_enter(L2TG(L));
      (void)la_futex_wait(&cts->parse_token, 1, 1000000);
      actions = lj_native_leave(L);
      lj_safepoint_checkstop(L, actions);  /* 11.2: cdef may park. */
    }
#else
    UNUSED(L);
    la_cpu_pause();  /* 11.2: non-Linux fallback outside the target matrix. */
#endif
  }
}

void lj_ctype_parse_unlock(CTState *cts)
{
  la_store32_rel(&cts->parse_token, 0);  /* 11.2: publish parser mutations. */
#if defined(__linux__)
  (void)la_futex_wake(&cts->parse_token, 1);  /* 11.2: wake next cparse waiter. */
#endif
}

void lj_ctype_fin_lock(CTState *cts)
{
  for (;;) {
    uint32_t expect = 0;
    if (la_cas32(&cts->fin_token, &expect, 1, LA_ACQ_REL, LA_ACQ))
      return;  /* 11.4 bridge: serialize hidden finalizer table mutation. */
#if defined(__linux__)
    (void)la_futex_wait(&cts->fin_token, 1, 1000000);
#else
    la_cpu_pause();
#endif
  }
}

void lj_ctype_fin_unlock(CTState *cts)
{
  la_store32_rel(&cts->fin_token, 0);  /* 11.4: publish finalizer table update. */
#if defined(__linux__)
  (void)la_futex_wake(&cts->fin_token, 1);
#endif
}

void lj_ctype_misc_lock(CTState *cts)
{
  for (;;) {
    uint32_t expect = 0;
    if (la_cas32(&cts->misc_token, &expect, 1, LA_ACQ_REL, LA_ACQ))
      return;  /* 11.2 bridge: serialize miscmap structural mutation. */
#if defined(__linux__)
    (void)la_futex_wait(&cts->misc_token, 1, 1000000);
#else
    la_cpu_pause();
#endif
  }
}

void lj_ctype_misc_unlock(CTState *cts)
{
  la_store32_rel(&cts->misc_token, 0);  /* 11.2: publish miscmap update. */
#if defined(__linux__)
  (void)la_futex_wake(&cts->misc_token, 1);
#endif
}

static LJ_AINLINE CTypeID ctype_hash_load(CTState *cts, uint32_t h)
{
  return (CTypeID)(la_load32_acq(&cts->hash[h]) & 0xffffu);  /* 11.2: ctype hash publication. */
}

static LJ_AINLINE int ctype_hash_cas(CTState *cts, uint32_t h,
				     CTypeID *oldid, CTypeID newid)
{
  uint32_t old = (uint32_t)*oldid;
  int ok = la_cas32(&cts->hash[h], &old, (uint32_t)newid,
		    LA_ACQ_REL, LA_ACQ);
  *oldid = (CTypeID)(old & 0xffffu);
  return ok;  /* 11.2: CAS-prepend ctype hash publication. */
}

static void ctype_hash_setnext(CTState *cts, CType *src, CTypeID id,
			       CTypeID next)
{
  CType *tab, *dst;
  do {
    dst = ctype_get(cts, id);
    if (dst != src)
      *dst = *src;
    dst->next = (CTypeID1)next;
    tab = ctype_tab_acq(cts);
  } while (dst != &tab[id]);
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
    if (!ctype_isabandoned(ct->info) && ct->info == info && ct->size == size)
      return id;
    id = ct->next;
  }
  return 0;
}

static void ctype_abandon(CTState *cts, CTypeID id)
{
  CType *ct = ctype_get(cts, id);
  ct->info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD));
  ct->size = 0;
  ct->sib = 0;
  setgcrefnull(ct->name);
  /* Keep ct->next so hash walkers can skip through abandoned entries. */
}

static GCSize ctype_tab_size(MSize sizetab)
{
  return (GCSize)(sizeof(CTypeTab) + (sizetab - 1u)*sizeof(CType));
}

static CTypeTab *ctype_tab_new(lua_State *L, MSize sizetab)
{
  CTypeTab *tabh = (CTypeTab *)lj_mem_new(L, ctype_tab_size(sizetab));
  tabh->sizetab = sizetab;
  tabh->retire_epoch = 0;
  tabh->retired_next = NULL;
  return tabh;
}

static void ctype_tab_free(global_State *g, CTypeTab *tabh)
{
  lj_mem_free(g, tabh, ctype_tab_size(tabh->sizetab));
}

static void ctype_tab_retired_push(CTState *cts, CTypeTab *ret)
{
  void *head = la_loadptr_acq((void *const *)&cts->retiredtab);
  do {
    ret->retired_next = (CTypeTab *)head;
  } while (!la_casptr((void **)&cts->retiredtab, &head, ret,
		      LA_ACQ_REL, LA_ACQ));  /* 11.2 CTState table SMR. */
}

static void ctype_tab_retire(CTState *cts, CTypeTab *ret)
{
  la_store64_rel(&ret->retire_epoch, la_load64_acq(&cts->g->gc2.hs_epoch));
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
    MSize osz = oldh->sizetab;
    MSize nsz;
    CTypeTab *newh;
    void *expect;
    if ((MSize)id < osz)
      return oldh->tab;
    nsz = ctype_tab_growsize(osz, id);
    newh = ctype_tab_new(L, nsz);
    memcpy(newh->tab, oldh->tab, osz*sizeof(CType));
    memset(newh->tab + osz, 0, (nsz - osz)*sizeof(CType));
    expect = oldh;
    if (la_casptr((void **)&cts->tabh, &expect, newh, LA_ACQ_REL, LA_ACQ)) {
      cts->tab = newh->tab;  /* Token-held/diagnostic mirror. */
      cts->sizetab = nsz;
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
    if (LJ_UNLIKELY((MSize)id >= tabh->sizetab)) {
      (void)ctype_tab_grow_l(L, cts, id);
      continue;
    }
    if (la_cas32(&cts->top, &expect, id+1u, LA_ACQ_REL, LA_ACQ)) {
      *ctp = &tabh->tab[id];
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
  ct->info = 0;
  ct->size = 0;
  ct->sib = 0;
  ct->next = 0;
  setgcrefnull(ct->name);
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
    ct->info = info;
    ct->size = size;
    ct->sib = 0;
    ct->next = 0;
    setgcrefnull(ct->name);
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
  uint32_t h = ct_hashtype(ct->info, ct->size);
  ctype_hash_prepend(cts, h, ct, id);
}

/* Add named element to hash table. */
void lj_ctype_addname(CTState *cts, CType *ct, CTypeID id)
{
  uint32_t h = ct_hashname(ctype_name_acq(ct));
  ctype_hash_prepend(cts, h, ct, id);
}

/* Get a C type by name, matching the type mask. */
CTypeID lj_ctype_getname(CTState *cts, CType **ctp, GCstr *name, uint32_t tmask)
{
  CTypeID id = ctype_hash_load(cts, ct_hashname(name));
  while (id) {
    CType *ct = ctype_get(cts, id);
    if (!ctype_isabandoned(ct->info) && ctype_name_acq(ct) == name &&
	((tmask >> ctype_type(ct->info)) & 1)) {
      *ctp = ct;
      return id;
    }
    id = ct->next;
  }
  *ctp = &ctype_tab_acq(cts)[0];  /* Simplify caller logic. */
  return 0;
}

/* Get a struct/union/enum/function field by name. */
CType *lj_ctype_getfieldq(CTState *cts, CType *ct, GCstr *name, CTSize *ofs,
			  CTInfo *qual)
{
  while (ct->sib) {
    ct = ctype_get(cts, ct->sib);
    if (ctype_name_acq(ct) == name) {
      *ofs = ct->size;
      return ct;
    }
    if (ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
      CType *fct, *cct = ctype_child(cts, ct);
      CTInfo q = 0;
      while (ctype_isattrib(cct->info)) {
	if (ctype_attrib(cct->info) == CTA_QUAL) q |= cct->size;
	cct = ctype_child(cts, cct);
      }
      fct = lj_ctype_getfieldq(cts, cct, name, ofs, qual);
      if (fct) {
	if (qual) *qual |= q;
	*ofs += ct->size;
	return fct;
      }
    }
  }
  return NULL;  /* Not found. */
}

/* -- C type information -------------------------------------------------- */

/* Follow references and get raw type for a C type ID. */
CType *lj_ctype_rawref(CTState *cts, CTypeID id)
{
  return ctype_get(cts, ctype_rawrefid(cts, id));
}

/* Get size for a C type ID. Does NOT support VLA/VLS. */
CTSize lj_ctype_size(CTState *cts, CTypeID id)
{
  CType *ct = ctype_raw(cts, id);
  return ctype_hassize(ct->info) ? ct->size : CTSIZE_INVALID;
}

/* Get size for a variable-length C type. Does NOT support other C types. */
CTSize lj_ctype_vlsize(CTState *cts, CType *ct, CTSize nelem)
{
  uint64_t xsz = 0;
  if (ctype_isstruct(ct->info)) {
    CTypeID arrid = 0, fid = ct->sib;
    xsz = ct->size;  /* Add the struct size. */
    while (fid) {
      CType *ctf = ctype_get(cts, fid);
      if (ctype_type(ctf->info) == CT_FIELD)
	arrid = ctype_cid(ctf->info);  /* Remember last field of VLS. */
      fid = ctf->sib;
    }
    ct = ctype_raw(cts, arrid);
  }
  lj_assertCTS(ctype_isvlarray(ct->info), "VLA expected");
  ct = ctype_rawchild(cts, ct);  /* Get array element. */
  lj_assertCTS(ctype_hassize(ct->info), "bad VLA without size");
  /* Calculate actual size of VLA and check for overflow. */
  xsz += (uint64_t)ct->size * nelem;
  return xsz < 0x80000000u ? (CTSize)xsz : CTSIZE_INVALID;
}

/* Get type, qualifiers, size and alignment for a C type ID. */
CTInfo lj_ctype_info(CTState *cts, CTypeID id, CTSize *szp)
{
  CTInfo qual = 0;
  CType *ct = ctype_get(cts, id);
  for (;;) {
    CTInfo info = ct->info;
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= ct->size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(ct->size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      lj_assertCTS(ctype_hassize(info) || ctype_isfunc(info),
		   "ctype without size");
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : ct->size;
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
  if (ctype_isref(ct->info)) id = ctype_cid(ct->info);
  return lj_ctype_info(cts, id, szp);
}

/* Get ctype metamethod. */
cTValue *lj_ctype_meta(CTState *cts, CTypeID id, MMS mm)
{
  CType *ct = ctype_get(cts, id);
  cTValue *tv;
  while (ctype_isattrib(ct->info) || ctype_isref(ct->info)) {
    id = ctype_cid(ct->info);
    ct = ctype_get(cts, id);
  }
  if (ctype_isptr(ct->info) &&
      ctype_isfunc(ctype_get(cts, ctype_cid(ct->info))->info))
    tv = lj_tab_getstr(cts->miscmap, &cts->g->strempty);
  else
    tv = lj_tab_getinth(cts->miscmap, -(int32_t)id);
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
  while (ctype_isattrib(ct->info) || ctype_isref(ct->info)) {
    id = ctype_cid(ct->info);
    ct = ctype_get(cts, id);
  }
  if (ctype_isptr(ct->info) &&
      ctype_isfunc(ctype_get(cts, ctype_cid(ct->info))->info))
    tv = lj_tab_getstr(cts->miscmap, &cts->g->strempty);
  else
    tv = lj_tab_getinth(cts->miscmap, -(int32_t)id);
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
    CTInfo info = ct->info;
    CTSize size = ct->size;
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
	  CTSize csize = ctype_child(ctr->cts, ct)->size;
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
  ret = (CTypeTab *)la_xchgptr_acqrel((void **)&cts->retiredtab, NULL);
  while (ret) {
    CTypeTab *next = ret->retired_next;
    ret->retired_next = NULL;
    if (la_load64_acq(&ret->retire_epoch) < completed_epoch) {
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
  CTypeTab *ret = (CTypeTab *)la_xchgptr_acqrel((void **)&cts->retiredtab,
						NULL);
  while (ret) {
    CTypeTab *next = ret->retired_next;
    ctype_tab_free(g, ret);
    ret = next;
  }
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
  la_storeptr_rel((void **)&cts->tabh, tabh);
  cts->tab = ct;
  cts->sizetab = CTTYPETAB_MIN;
  la_store32_rel(&cts->top, CTTYPEINFO_NUM);
  cts->g = G(L);
  for (id = 0; id < CTTYPEINFO_NUM; id++, ct++) {
    CTInfo info = lj_ctype_typeinfo[id];
    ct->size = (CTSize)((int32_t)(info << 16) >> 26);
    ct->info = info & 0xffff03ffu;
    ct->sib = 0;
    if (ctype_type(info) == CT_KW || ctype_istypedef(info)) {
      size_t len = strlen(name);
      GCstr *str = lj_str_new(L, name, len);
      ctype_setname(ct, str);
      name += len+1;
      lj_ctype_addname(cts, ct, id);
    } else {
      setgcrefnull(ct->name);
      ct->next = 0;
      if (!ctype_isenum(info)) ctype_addtype(cts, ct, id);
    }
  }
  setmref(G(L)->ctype_state, cts);
  return cts;
}

/* Create special weak-keyed finalizer table. */
void lj_ctype_initfin(lua_State *L)
{
  /* NOBARRIER: The table is new (marked white). */
  GCtab *t = lj_tab_new(L, 0, 1);
  setgcref(t->metatable, obj2gco(t));
  lj_tab_storestr(L, lj_tab_setstr(L, t, lj_str_newlit(L, "__mode")),
		  lj_str_newlit(L, "k"));
  t->nomm = (uint8_t)(~(1u<<MM_mode));
  setgcrefroot(G(L)->gcroot[GCROOT_FFI_FIN], obj2gco(t));
}

/* Free C type table and state. */
void lj_ctype_freestate(global_State *g)
{
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    lj_ccallback_mcode_free(cts);
    ctype_freeretired(g, cts);
    ctype_tab_free(g, ctype_tabh_acq(cts));
    lj_mem_freevec(g, cts->cb.cbid, cts->cb.sizeid, CTypeID1);
    lj_mem_freevec(g, cts->cb.owner, cts->cb.sizeid, lua_State *);
    lj_mem_freet(g, cts);
  }
}

#endif
