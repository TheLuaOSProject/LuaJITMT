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
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_ccallback.h"
#include "lj_buf.h"
#include "lj_dispatch.h"
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
  (void)lj_thr_retry_yield(L);
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

/* Immutable layout oracle for cdata materialized before CTState publication.
** Only fixed-size predefined payloads are accepted; VLA/void/abandoned entries
** cannot be validated without the live ctype graph. */
int lj_ctype_predefined_payload_size(CTypeID id, CTSize *sizep)
{
  CTSize size;
  switch (id) {
#define CTYPE_PREDEF_SIZE(name, sz, ct, info) \
  case CTID_##name: \
    if ((int)(sz) < 0 || (ct) == CT_ATTRIB || (ct) == CT_VOID) return 0; \
    size = (CTSize)(sz); \
    break;
    CTTYDEF(CTYPE_PREDEF_SIZE)
#undef CTYPE_PREDEF_SIZE
  default:
    return 0;
  }
  if (sizep)
    *sizep = size;
  return 1;
}

#undef ctype_string_match

#define CTTYPEINFO_NUM		(sizeof(lj_ctype_typeinfo)/sizeof(CTInfo)-1)
/*
** Keep the production initial table size fixed. Tests that need to cover
** CTState table moves now force them by declaring enough ordinary ctypes at
** runtime, so the runtime has no build-only narrow-table path.
*/
#define CTCBBLACK_SIZE		4096u

/* -- C type interning ---------------------------------------------------- */

#define ct_hashtype(info, size)	(hashrot(info, size) & CTHASH_MASK)
#define ct_hashname(name) \
  (hashrot(u32ptr(name), u32ptr(name) + HASH_BIAS) & CTHASH_MASK)

void lj_ctype_parse_wait(CTState *cts, lua_State *L, uint32_t seq)
{
  ERRNO_SAVE
  if ((seq & 1u) == 0)
    return;
#if defined(LA_HAS_FUTEX)
  {
    uint32_t actions;
    int had_stopreq = lj_safepoint_had_stopreq(L);
    if (L) {
      lj_state_stack_pubrange(L, L);
      lj_native_enter(L2TG(L));
    }
    (void)ctype_parse_token_wait(cts, seq, 1000000);
    actions = L ? lj_native_leave(L) : 0;
    ERRNO_RESTORE
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
    /* 11.2: ctype waits may park in native state. */
  }
#else
  {
    int had_stopreq = lj_safepoint_had_stopreq(L);
    /*
    ** Non-futex platforms use the same short native-visible retry/yield
    ** discipline as the other FFI publication waits instead of parking for a
    ** fixed timer tick. The caller rechecks the parse sequence after return.
    */
    uint32_t actions = lj_thr_retry_yield(L);
    ERRNO_RESTORE
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
  }
#endif
  ERRNO_RESTORE
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
  ERRNO_SAVE
  uint32_t seq = ctype_parse_token_acq(cts);
  lj_assertCTS((seq & 1u) != 0, "cparse mutation sequence not held");
  ctype_parse_token_rel(cts, seq + 1u);  /* 11.2: publish parser mutations. */
#if defined(LA_HAS_FUTEX)
  /*
  ** Both token acquirers and sequence readers park on parse_token. After the
  ** parser publishes an even sequence, all waiters can re-check immediately;
  ** waking one reader would leave the rest parked until their STOPREQ timeout.
  */
  (void)ctype_parse_token_wake(cts, 0x7fffffff);
#endif
  ERRNO_RESTORE
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
  tabh = &cts->tab;
  if ((MSize)id >= ctype_tab_sizetab_acq(tabh))
    return -1;
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

/* Replace the temporary mode-string root with the complete weak table. */
static GCtab *ctype_fin_tab_new_anchor_l(lua_State *L, cTValue *key)
{
  LJTabRoot root;
  GCtab *t;
  GCstr *mode;
  lj_state_checkstack(L, 1);
  mode = lj_str_newlit(L, "k");
  setstrV(L, L->top, mode);
  L->top++;
  lj_state_stack_pubtv(L, L, L->top-1);
  t = lj_tab_new_finreg_rooted(L, mode, key, &root);
  settabV(L, L->top-1, t);
  lj_state_stack_pubtv(L, L, L->top-1);
  lj_tab_root_release(&root);
  return t;
}

static FinRegGen *ctype_fin_gen_new_l(lua_State *L, GCtab *t)
{
  FinRegGen *gen = lj_mem_newt(L, sizeof(FinRegGen), FinRegGen);
  gen->registration.lo = gen->registration.hi = 0;
  fin_gen_tab_rel(gen, t);
  fin_gen_next_rel(gen, NULL);
  gen->retired_next = NULL;
  setgcrefnull(gen->registered_key);
  gen->insert_nodes = NULL;
  gen->insert_hmask = 0;
  return gen;
}

/* Announce before sampling either head. An unlinked record and its weak table
** remain collector roots until every reader that could have seen them leaves. */
static void ctype_fin_read_enter(CTState *cts)
{
  if (la_add32_acqrel(&cts->fin_native_readers, 1) == UINT32_MAX)
    abort();
  la_fence_seq();
}

static void ctype_fin_read_leave(global_State *g, CTypeFinRead reader)
{
  if (reader == LJ_CTYPE_FIN_READ_SMR)
    lj_gc2_smr_read_leave(g);
  else if (reader == LJ_CTYPE_FIN_READ_NATIVE) {
    uint32_t old = la_sub32_acqrel(&ctype_ctsG(g)->fin_native_readers, 1);
    lj_assertG(old != 0, "unbalanced FINREG native reader");
    UNUSED(old);
  }
}

void lj_ctype_fin_lease_release(CTypeFinLease *lease)
{
  global_State *g;
  CTypeFinRead reader;
  if (!lease)
    return;
  g = lease->g;
  reader = lease->reader;
  /* Clear the public state first: cleanup paths and cpcall error recovery may
  ** call release more than once, but an admission must be dropped exactly once. */
  lease->g = NULL;
  lease->tab = NULL;
  lease->generation = NULL;
  lease->reader = LJ_CTYPE_FIN_READ_NONE;
  lj_gc2_lease_release(&lease->table_lease);
  ctype_fin_read_leave(g, reader);
}

/* Acquire the raw generation record only long enough to snapshot its list
** edge and install an exact expected-TAB body lease. The returned token keeps
** the table mapped. The caller supplies the lifetime of the generation. */
static int ctype_fin_gen_tab_acquire_next_smr_mode(CTState *cts, FinRegGen *gen,
						CTypeFinLease *lease,
						FinRegGen **nextp,
						int semantic)
{
  global_State *g;
  LJGC2Lease genlease = { 0 };
  GCtab *t;
  FinRegGen *next;
  int rc;
  if (!lease)
    return LJ_CTYPE_FIN_RETRY;
  memset(lease, 0, sizeof(*lease));
  if (nextp)
    *nextp = NULL;
  if (!cts || !gen || !(g = cts->g))
    return LJ_CTYPE_FIN_MISS;

  lease->g = g;
  if (lj_gc2_mem_lease_acquire(g, gen, &genlease) < 0) {
    lj_ctype_fin_lease_release(lease);
    return LJ_CTYPE_FIN_RETRY;
  }
  t = fin_gen_tab_acq(gen);
  next = fin_gen_next_acq(gen);
  if (!t) {
    lj_gc2_lease_release(&genlease);
    if (nextp)
      *nextp = next;
    lj_ctype_fin_lease_release(lease);
    return LJ_CTYPE_FIN_MISS;
  }
  if (semantic) {
    rc = lj_gc2_obj_lease_acquire(g, obj2gco(t), (uint32_t)~LJ_TTAB,
				NULL, &lease->table_lease);
  } else {
    TValue candidate;
    /* Membership inspection must not create a semantic SWEEP publication.
    ** Table traversal asks this predicate while consuming queued graph work;
    ** republishing every FINREG table here would recreate that work forever.
    ** Construct only the expected tag, then validate it under an observational
    ** lease before any table byte is read. Both STALE and RETRY fail admission.
    */
    setgcVraw(&candidate, obj2gco(t), LJ_TTAB);
    rc = lj_gc2_tv_lease_acquire(g, &candidate, &lease->table_lease) ==
	 LJ_GC2_TV_EDGE_VALID ? 0 : -1;
  }
  if (rc < 0 || fin_gen_tab_acq(gen) != t) {
    lj_gc2_lease_release(&genlease);
    lj_ctype_fin_lease_release(lease);
    return LJ_CTYPE_FIN_RETRY;
  }
  if (nextp)
    *nextp = next;
  lease->tab = t;
  lease->generation = gen;
  lj_gc2_lease_release(&genlease);
  return LJ_CTYPE_FIN_FOUND;
}

static int ctype_fin_gen_tab_acquire_next_smr(CTState *cts, FinRegGen *gen,
					      CTypeFinLease *lease,
					      FinRegGen **nextp)
{
  return ctype_fin_gen_tab_acquire_next_smr_mode(cts, gen, lease, nextp, 1);
}

static int ctype_fin_gen_next_smr(global_State *g, FinRegGen *gen,
				   FinRegGen **nextp)
{
  LJGC2Lease lease = { 0 };
  if (!gen) {
    *nextp = NULL;
    return 1;
  }
  if (lj_gc2_mem_lease_acquire(g, gen, &lease) < 0) {
    lj_gc2_lease_release(&lease);
    return 0;
  }
  *nextp = fin_gen_next_acq(gen);
  lj_gc2_lease_release(&lease);
  return 1;
}

static int ctype_fin_order_next_smr(global_State *g, FinRegOrderNode *ord,
				     int lifetime, FinRegOrderNode **nextp)
{
  LJGC2Lease lease = { 0 };
  if (!ord) {
    *nextp = NULL;
    return 1;
  }
  if (lj_gc2_mem_lease_acquire(g, ord, &lease) < 0) {
    lj_gc2_lease_release(&lease);
    return 0;
  }
  *nextp = lifetime ? fin_order_lifetime_next_acq(ord) : fin_order_next_acq(ord);
  lj_gc2_lease_release(&lease);
  return 1;
}

static LJ_NORET void ctype_fin_spine_cycle(global_State *g)
{
  lj_assertG_(g, 0, "cycle in FINREG native spine");
  UNUSED(g);
  abort();
}

int lj_ctype_fin_gen_tab_acquire(CTState *cts, FinRegGen *gen,
				 CTypeFinLease *lease)
{
  int rc;
  if (!lease)
    return LJ_CTYPE_FIN_RETRY;
  memset(lease, 0, sizeof(*lease));
  if (!cts || !cts->g || !gen)
    return LJ_CTYPE_FIN_MISS;
  if (!lj_gc2_smr_read_try(cts->g))
    return LJ_CTYPE_FIN_RETRY;
  rc = ctype_fin_gen_tab_acquire_next_smr(cts, gen, lease, NULL);
  if (rc == LJ_CTYPE_FIN_FOUND)
    lease->reader = LJ_CTYPE_FIN_READ_SMR;
  else
    lj_gc2_smr_read_leave(cts->g);
  return rc;
}

FinRegOrderNode *lj_ctype_fin_order_new_nothrow(lua_State *L)
{
  FinRegOrderNode *ord = lj_mem_new_deferred_nothrow(L, sizeof(*ord));
  if (!ord)
    return NULL;
  fin_order_obj_clear(ord);
  fin_order_tab_rel(ord, NULL);
  fin_order_slot_rel(ord, NULL);
  fin_order_next_rel(ord, NULL);
  fin_order_lifetime_next_rel(ord, NULL);
  ord->retired_next = NULL;
  ord->reclaim_selected = 0;
  ord->registration_id = 0;
  la_store32_rel(&ord->submission_done, 0);
  la_store32_rel(&ord->queue_visible, 0);
  fin_order_active_rel(ord, 0);
  return ord;
}

void lj_ctype_fin_order_publish(CTState *cts, FinRegOrderNode *ord,
				GCobj *o, GCtab *t, TValue *slot)
{
  FinRegOrderNode *head;
  if (!cts || !ord || !o || !t || (!slot && !ord->registration_id))
    return;
  fin_order_obj_rel(ord, o);
  fin_order_tab_rel(ord, t);
  fin_order_slot_rel(ord, slot);
  fin_order_active_rel(ord, 1);
  (void)lj_gc2_markmem(cts->g, ord);
  /* Keep each node alive before exposing ordered membership. This lets
  ** every contender retire the membership with one CAS. */
  do {
    head = fin_order_lifetime_acq(cts);
    fin_order_lifetime_next_rel(ord, head);
  } while (!fin_order_lifetime_cas(cts, &head, ord));
  do {
    head = fin_order_head_acq(cts);
    fin_order_next_rel(ord, head);
  } while (!fin_order_head_cas(cts, &head, ord));
  /* Close mark-before-publish versus IDLE->MARK clear/root-snapshot. */
  (void)lj_gc2_markmem(cts->g, ord);
  /* 11.4 FINREG ordered registration publish. */
}

static void ctype_fin_order_deactivate(CTState *cts, FinRegOrderNode *ord)
{
  if (fin_order_active_retire(ord)) {
    la_store32_rel(&cts->fin_order_has_inactive, 1);
    lj_gc2_finreg_cdata_note_order_retired(cts->g);
  }
}

int lj_ctype_fin_order_retire(CTState *cts, FinRegOrderNode *prev,
			      FinRegOrderNode *ord, FinRegOrderNode *next)
{
  FinRegOrderNode *expect;
  LJGC2Lease ordlease = { 0 }, prevlease = { 0 };
  int ok = 0;
  UNUSED(next);
  if (!cts || !cts->g || !ord || !lj_gc2_smr_read_try(cts->g))
    return 0;
  if (lj_gc2_mem_lease_acquire(cts->g, ord, &ordlease) < 0)
    goto out;
  next = fin_order_next_acq(ord);  /* Ignore an unprotected caller snapshot. */
  if (prev && lj_gc2_mem_lease_acquire(cts->g, prev, &prevlease) < 0)
    goto out;
  ctype_fin_order_deactivate(cts, ord);
  if (prev) {
    if (lj_gc2_mem_registered(cts->g, prev)) {
      expect = ord;
      if (fin_order_active_acq(prev) == 1)
	(void)fin_order_next_cas(prev, &expect, next);
    }
  } else {
    expect = ord;
    (void)fin_order_head_cas(cts, &expect, next);
  }
  ok = 1;  /* 11.4 ordered FINREG logical retire plus best-effort splice. */
out:
  lj_gc2_lease_release(&prevlease);
  lj_gc2_lease_release(&ordlease);
  lj_gc2_smr_read_leave(cts->g);
  return ok;
}

int lj_ctype_fin_order_retire_obj_complete(CTState *cts, GCobj *target,
					    size_t *retiredp)
{
  FinRegOrderNode *prev, *ord, *fast;
  size_t n = 0;
  int complete = 0;
  if (retiredp)
    *retiredp = 0;
  if (!cts || !cts->g || !target || !lj_gc2_smr_read_try(cts->g))
    return 0;
restart:
  prev = NULL;
  ord = fin_order_head_acq(cts);
  fast = ord;
  while (ord) {
    LJGC2Lease ordlease = { 0 };
    FinRegOrderNode *next;
    uint32_t active;
    GCobj *obj;
    if (lj_gc2_mem_lease_acquire(cts->g, ord, &ordlease) < 0) {
      lj_gc2_lease_release(&ordlease);
      goto transient;
    }
    next = fin_order_next_acq(ord);
    active = fin_order_active_acq(ord);
    obj = active == 1 ? fin_order_obj_acq(ord) : NULL;
    lj_gc2_lease_release(&ordlease);
    if (active == 1 && obj == target) {
      int retired = lj_ctype_fin_order_retire(cts, prev, ord, next);
      n += (size_t)retired;
      if (!retired)
	goto transient;
    } else if (active == 1) {
      prev = ord;
    }
    ord = next;
    if (fast && !ctype_fin_order_next_smr(cts->g, fast, 0, &fast))
      goto transient;
    if (fast && !ctype_fin_order_next_smr(cts->g, fast, 0, &fast))
      goto transient;
    if (ord && fast == ord) {
      lj_gc2_smr_read_leave(cts->g);
      ctype_fin_spine_cycle(cts->g);
    }
  }
  complete = 1;
  goto out;
transient:
  /* A logical retire cannot be rolled back. If this pass already committed
  ** any target node, restart from the current head while FINCLAIM and the
  ** caller's outer lifetime certificates still exclude a same-object writer.
  ** Recomputing prev also avoids depending on an adjacent node concurrently
  ** retired by another cdata transaction. A pre-commit transient remains
  ** rollbackable by the caller. */
  if (n != 0) {
    (void)lj_thr_retry_yield(NULL);
    goto restart;
  }
out:
  lj_gc2_smr_read_leave(cts->g);
  if (retiredp)
    *retiredp = n;
  return complete;
}

/* The caller retains the native spine while following the registration ID. */
void lj_ctype_fin_order_retire_id(CTState *cts, GCobj *o, uint64_t id)
{
  FinRegOrderNode *ord = fin_order_head_acq(cts);
  size_t remaining = LJ_ROOT_SCAN_LIMIT;
  while (ord && remaining--) {
    LJGC2Lease scope = { 0 };
    FinRegOrderNode *next;
    uint32_t active;
    if (lj_gc2_mem_lease_acquire(cts->g, ord, &scope) < 0) {
      lj_gc2_lease_release(&scope);
      return;
    }
    next = fin_order_next_acq(ord);
    active = fin_order_active_acq(ord);
    if (active == 1 && ord->registration_id == id &&
        fin_order_obj_acq(ord) == o) {
      ctype_fin_order_deactivate(cts, ord);
      lj_gc2_lease_release(&scope);
      return;
    }
    lj_gc2_lease_release(&scope);
    ord = next;
  }
}

size_t lj_ctype_fin_order_retire_obj(CTState *cts, GCobj *target)
{
  size_t n = 0;
  (void)lj_ctype_fin_order_retire_obj_complete(cts, target, &n);
  return n;
}

int lj_ctype_fin_head(CTState *cts, CTypeFinLease *lease)
{
  FinRegGen *gen;
  int rc;
  if (!lease)
    return LJ_CTYPE_FIN_RETRY;
  memset(lease, 0, sizeof(*lease));
  if (!cts || !cts->g)
    return LJ_CTYPE_FIN_MISS;
  if (!lj_gc2_smr_read_try(cts->g))
    return LJ_CTYPE_FIN_RETRY;
  gen = fin_gen_head_acq(cts);
  rc = gen ? ctype_fin_gen_tab_acquire_next_smr(cts, gen, lease, NULL) :
    LJ_CTYPE_FIN_MISS;
  if (rc == LJ_CTYPE_FIN_FOUND)
    lease->reader = LJ_CTYPE_FIN_READ_SMR;
  else
    lj_gc2_smr_read_leave(cts->g);
  return rc;
}

/* The caller holds the spine until every sampled successor has been consumed. */
static int ctype_fin_get_held(CTState *cts, FinRegGen *gen, cTValue *key,
			     CTypeFinLease *lease)
{
  while (gen != NULL) {
    CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
    FinRegGen *next = NULL;
    LJGC2Lease generation = { 0 };
    int rc, same_key;
    if (lj_gc2_mem_lease_acquire(cts->g, gen, &generation) < 0) {
      lj_gc2_lease_release(&generation);
      return LJ_CTYPE_FIN_RETRY;
    }
    next = fin_gen_next_acq(gen);
    same_key = gcval(key) == gcref(gen->registered_key) &&
               finreg_state(finreg_load(gen)) != FINREG_RETIRING;
    lj_gc2_lease_release(&generation);
    if (!same_key) {
      gen = next;
      continue;
    }
    rc = ctype_fin_gen_tab_acquire_next_smr(cts, gen, &held, NULL);
    if (rc == LJ_CTYPE_FIN_RETRY)
      return LJ_CTYPE_FIN_RETRY;
    if (rc == LJ_CTYPE_FIN_FOUND && fin_gen_tab_enabled_acq(held.tab)) {
      /* A weak-table clear may have erased the hidden key while the
      ** native pending state preserves the exact allocation. */
      *lease = held;
      return LJ_CTYPE_FIN_FOUND;
    } else {
      lj_ctype_fin_lease_release(&held);
    }
    gen = next;
  }
  return LJ_CTYPE_FIN_MISS;
}

int lj_ctype_fin_get_native(CTState *cts, cTValue *key, CTypeFinLease *lease)
{
  FinRegGen *gen;
  int rc;
  if (!lease)
    return LJ_CTYPE_FIN_RETRY;
  memset(lease, 0, sizeof(*lease));
  if (!cts || !cts->g || !key || !tviscdata(key))
    return LJ_CTYPE_FIN_MISS;
  ctype_fin_read_enter(cts);
  gen = fin_gen_head_acq(cts);
  rc = ctype_fin_get_held(cts, gen, key, lease);
  if (rc == LJ_CTYPE_FIN_FOUND) {
    lease->reader = LJ_CTYPE_FIN_READ_NATIVE;
    lj_gc2_lease_release(&lease->table_lease);
  } else {
    ctype_fin_read_leave(cts->g, LJ_CTYPE_FIN_READ_NATIVE);
  }
  return rc;
}

#define CTYPE_FIN_RETRY_MAX 256u

int lj_ctype_fin_newgen(lua_State *L, CTState *cts, cTValue *key,
			CTypeFinLease *lease)
{
  uint32_t retry;
  GCtab *t;
  FinRegGen *gen;
  ptrdiff_t anchor;
  if (!L || !cts || !key || !lease)
    return 0;

  memset(lease, 0, sizeof(*lease));
  /* Every reachable generation has immutable key membership. A new
  ** registration publishes a private table through the head CAS, so an
  ** absence proof against its exact predecessor remains authoritative. */
  anchor = savestack(L, L->top);
  t = ctype_fin_tab_new_anchor_l(L, key);
  gen = ctype_fin_gen_new_l(L, t);
  setgcrefp(gen->registered_key, gcval(key));
  gen->insert_nodes = lj_tab_node_acq(t);
  gen->insert_hmask = lj_tab_hmask_acq(t);

  for (retry = 0; retry < CTYPE_FIN_RETRY_MAX; retry++) {
    CTypeFinLease prepared = CTYPE_FIN_LEASE_INIT;
    CTypeFinLease found = CTYPE_FIN_LEASE_INIT;
    FinRegGen *head;
    int rc;
    ctype_fin_read_enter(cts);
    head = fin_gen_head_acq(cts);
    rc = ctype_fin_get_held(cts, head, key, &found);
    if (rc == LJ_CTYPE_FIN_FOUND) {
      lj_ctype_fin_lease_release(&found);
      ctype_fin_read_leave(cts->g, LJ_CTYPE_FIN_READ_NATIVE);
      lj_mem_freet(G(L), gen);
      L->top = restorestack(L, anchor);
      return -1;
    }
    lj_ctype_fin_lease_release(&found);
    if (rc == LJ_CTYPE_FIN_RETRY || fin_gen_head_acq(cts) != head) {
      ctype_fin_read_leave(cts->g, LJ_CTYPE_FIN_READ_NATIVE);
      ctype_fin_claim_wait(L);
      continue;
    }
    rc = ctype_fin_gen_tab_acquire_next_smr(cts, gen, &prepared, NULL);
    if (rc == LJ_CTYPE_FIN_FOUND) {
      fin_gen_next_rel(gen, head);
      (void)lj_gc2_markmem(G(L), gen);
      lj_gc_pubobjroot(L, obj2gco(t));
#if defined(LJ_CDATA_TEST_HELPERS)
      lj_cdata_test_fin_pause_at(LJ_CDATA_FIN_PAUSE_FIRST_KEY);
#endif
      if (fin_gen_head_cas(cts, &head, gen)) {
        /* The already validated token moves to the caller. No throwing allocation
        ** or fresh body admission is needed past the publication CAS. */
        (void)lj_gc2_markmem(G(L), gen);
        lj_gc_pubobjroot(L, obj2gco(t));
        prepared.reader = LJ_CTYPE_FIN_READ_NATIVE;
        lj_gc2_lease_release(&prepared.table_lease);
        *lease = prepared;
        L->top = restorestack(L, anchor);
        return 1;
      }
    }
    lj_ctype_fin_lease_release(&prepared);
    ctype_fin_read_leave(cts->g, LJ_CTYPE_FIN_READ_NATIVE);
    ctype_fin_claim_wait(L);
  }
  lj_mem_freet(G(L), gen);
  L->top = restorestack(L, anchor);
  return LJ_CTYPE_FIN_ERROR;
}

int lj_ctype_fin_istab(global_State *g, GCtab *t)
{
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen;
  if (!cts || !t)
    return LJ_CTYPE_FIN_MISS;
  if (!lj_gc2_smr_read_try(g))
    return LJ_CTYPE_FIN_RETRY;
  gen = fin_gen_head_acq(cts);
  while (gen != NULL) {
    CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
    FinRegGen *next = NULL;
    int rc = ctype_fin_gen_tab_acquire_next_smr_mode(
      cts, gen, &held, &next, 0);
    if (rc == LJ_CTYPE_FIN_RETRY) {
      lj_gc2_smr_read_leave(g);
      return LJ_CTYPE_FIN_RETRY;
    }
    if (rc == LJ_CTYPE_FIN_FOUND && held.tab == t &&
	fin_gen_tab_enabled_acq(held.tab)) {
      lj_ctype_fin_lease_release(&held);
      lj_gc2_smr_read_leave(g);
      return LJ_CTYPE_FIN_FOUND;
    }
    lj_ctype_fin_lease_release(&held);
    gen = next;
  }
  lj_gc2_smr_read_leave(g);
  return LJ_CTYPE_FIN_MISS;
}

int lj_ctype_fin_mark(global_State *g, void (*mark)(global_State *, GCobj *),
		      void (*markmem)(global_State *, void *))
{
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen, *genfast;
  if (!cts)
    return 1;
  if (!mark || !markmem || !lj_gc2_smr_read_try(g))
    return 0;
  gen = fin_gen_head_acq(cts);
  genfast = gen;
  while (gen != NULL) {
    LJGC2Lease genlease = { 0 };
    FinRegGen *next;
    GCtab *t;
    if (lj_gc2_mem_lease_acquire(g, gen, &genlease) < 0) {
      lj_gc2_lease_release(&genlease);
      goto retry;
    }
    markmem(g, gen);
    {
      la_u128 registration = finreg_load(gen);
      TValue fin;
      tv_rawstore(&fin, registration.lo);
      if (finreg_state(registration) && tvisgcv(&fin))
        mark(g, gcV(&fin));
      if (finreg_state(registration) == FINREG_PENDING)
        mark(g, gcref(gen->registered_key));
    }
    t = fin_gen_tab_acq(gen);
    next = fin_gen_next_acq(gen);
    /* The semantic marker is itself the retain-first validity check. A queued
    ** prefilter here would drop a newly published table in the exact window
    ** where root closure needs to discover and queue it.
    */
    if (t)
      mark(g, obj2gco(t));
    lj_gc2_lease_release(&genlease);
    gen = next;
    if (genfast && !ctype_fin_gen_next_smr(g, genfast, &genfast))
      goto retry;
    if (genfast && !ctype_fin_gen_next_smr(g, genfast, &genfast))
      goto retry;
    if (gen && genfast == gen) {
      lj_gc2_smr_read_leave(g);
      ctype_fin_spine_cycle(g);
    }
  }
  {
    FinRegOrderNode *ord, *fast;
    ord = fin_order_lifetime_acq(cts);
    fast = ord;
    while (ord != NULL) {
      LJGC2Lease ordlease = { 0 };
      FinRegOrderNode *next;
      if (lj_gc2_mem_lease_acquire(g, ord, &ordlease) < 0) {
	lj_gc2_lease_release(&ordlease);
	goto retry;
      }
      markmem(g, ord);
      next = fin_order_lifetime_next_acq(ord);
      lj_gc2_lease_release(&ordlease);
      ord = next;
      if (fast && !ctype_fin_order_next_smr(g, fast, 1, &fast))
	goto retry;
      if (fast && !ctype_fin_order_next_smr(g, fast, 1, &fast))
	goto retry;
      if (ord && fast == ord) {
	lj_gc2_smr_read_leave(g);
	ctype_fin_spine_cycle(g);
      }
    }
  }
  /* A publisher may hold a removed record across a full collection. Keep
  ** its weak table reachable without pinning the arena across sweep. */
  for (gen = cts->fin_retired_gens; gen; gen = gen->retired_next) {
    markmem(g, gen);
    mark(g, obj2gco(fin_gen_tab_acq(gen)));
  }
  {
    FinRegOrderNode *ord;
    for (ord = cts->fin_retired_orders; ord; ord = ord->retired_next)
      markmem(g, ord);
  }
  lj_gc2_smr_read_leave(g);
  return 1;
retry:
  lj_gc2_smr_read_leave(g);
  return 0;
}

static uint32_t ctype_fin_free_retired(global_State *g, CTState *cts);

void lj_ctype_fin_freetabs(global_State *g, CTState *cts)
{
  FinRegGen *gen = fin_gen_head_xchg_acqrel(cts, NULL);
  FinRegOrderNode *ord = fin_order_lifetime_xchg_acqrel(cts, NULL);
  (void)fin_order_head_xchg_acqrel(cts, NULL);
  (void)ctype_fin_free_retired(g, cts);
  while (ord && lj_gc2_mem_registered(g, ord)) {
    FinRegOrderNode *next = fin_order_lifetime_next_acq(ord);
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
  (void)lj_thr_retry_yield(L);
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

CType *lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  CType *dst = ctype_get(cts, id);
  if (dst != src)
    ctype_copy_rel(dst, src);
  return dst;
}

static int ctype_hash_try_prepend(CTState *cts, uint32_t h, CType *src,
				  CTypeID id, CTypeID *head)
{
  CType *ct = lj_ctype_publish(cts, id, src);
  ctype_next_rel(ct, *head);
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
                                   CTSize size, int independent)
{
  while (id) {
    CType *ct = ctype_get(cts, id);
    if (ctype_info_acq(ct) == info && ctype_size_acq(ct) == size) {
      uint32_t state = la_load32_acq(&ct->internstate);
      if (state == CTINTERN_PENDING && independent &&
	  la_cas32(&ct->internstate, &state, CTINTERN_PUBLIC, LA_ACQ_REL, LA_ACQ))
	return id;
      if (state == CTINTERN_PUBLIC || state == CTINTERN_PENDING) return id;
    }
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

void lj_ctype_abandon(CTState *cts, CTypeID id)
{
  CType *ct = ctype_get(cts, id);
  CType tmp;
  uint32_t state = CTINTERN_PENDING;
  if (!la_cas32(&ct->internstate, &state, CTINTERN_ABANDONED,
		LA_ACQ_REL, LA_ACQ) &&
      state == CTINTERN_PUBLIC)
    return;
  ctype_copy_rel(&tmp, ct);
  ctype_info_rel(&tmp, CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD)));
  ctype_size_rel(&tmp, 0);
  ctype_sib_rel(&tmp, 0);
  ctype_clearname(&tmp);
  /* Keep ct->next so hash walkers can skip through abandoned entries. */
  lj_ctype_publish(cts, id, &tmp);
  lj_assertCTS(ctype_isabandoned(ctype_info_acq(ctype_get(cts, id))),
	       "abandoned ctype not visible in current table");
}

/* Each slot keeps the same address until the state closes. A grower can
** finish publishing a span allocated by another thread. */
static void ctype_tab_grow_l(lua_State *L, CTState *cts, CTypeID id)
{
  CTypeTab *tabh = &cts->tab;
  global_State *g = G(L);
  MSize capacity;
  while (id >= (capacity = ctype_tab_sizetab_acq(tabh))) {
    MSize i = (MSize)lj_fls(capacity) - CTYPE_TAB_MIN_BITS;
    GCSize bytes = capacity * sizeof(CType);
    CType *span = ctype_tab_span_acq(tabh, i);
    int installed = 0;
    if (!span) {
      CType *fresh = (CType *)lj_mem_new_deferred_nothrow(L, bytes);
      if (fresh) {
	memset(fresh, 0, bytes);
	(void)lj_gc2_markmem(g, fresh);
	installed = la_casptr((void **)&tabh->span[i], (void **)&span, fresh,
			      LA_ACQ_REL, LA_ACQ);
	if (installed)
	  span = fresh;
	else
	  lj_mem_free(g, fresh, bytes);
      } else {
	span = ctype_tab_span_acq(tabh, i);
	if (!span) lj_err_mem(L);
      }
    }
    (void)lj_gc2_markmem(g, span);
    (void)la_cas32(&tabh->sizetab, &capacity, capacity * 2, LA_ACQ_REL, LA_ACQ);
    if (installed) lj_mem_account_deferred(L, bytes);
  }
}

static CTypeID ctype_top_reserve_l(lua_State *L, CTState *cts, CType **ctp)
{
  for (;;) {
    CTypeTab *tabh = &cts->tab;
    CTypeID id = ctype_top_acq(cts);
    uint32_t expect = id;
    if (LJ_UNLIKELY(id >= CTID_MAX)) lj_err_msg(L, LJ_ERR_TABOV);
    if (LJ_UNLIKELY((MSize)id >= ctype_tab_sizetab_acq(tabh))) {
      ctype_tab_grow_l(L, cts, id);
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
  int independent = newp == NULL;  /* The parser tracks new IDs for rollback. */
  CTypeID id = ctype_hash_findtype(cts, head, info, size, independent);
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
    la_store32_rel(&ct->internstate, CTINTERN_PENDING);
    head = ctype_hash_load(cts, h);
    for (;;) {
      CTypeID winner = ctype_hash_findtype(cts, head, info, size, independent);
      if (winner) {
	lj_ctype_abandon(cts, id);
	return winner;
      }
      if (ctype_hash_try_prepend(cts, h, ct, id, &head)) {
	if (newp) *newp = 1;
	else la_store32_rel(&ct->internstate, CTINTERN_PUBLIC);
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
	lj_ctype_abandon(cts, id);
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
  *ctp = ctype_tab_slot(&cts->tab, 0);  /* Simplify caller logic. */
  return 0;
}

/* New parser records may already be on a name chain. Follow their immutable
** next links to the committed prefix without inspecting provisional names. */
static int ctype_getname_at(CTState *cts, uint64_t version, GCstr *name,
                            uint32_t tmask, CTypeID *idp, CType *out,
                            GCstr **redirp)
{
  CTypeID id = ctype_hash_load(cts, ct_hashname(name));
  CTypeID top = ctype_top_acq(cts);
  CTypeTab *tabh = &cts->tab;
  CTypeID limit = (version & 1u) ? la_load32_acq(&cts->layout_top) : top;
  MSize budget = top;
  while (id) {
    CType ct;
    CTInfo info;
    if (id >= top || id >= ctype_tab_sizetab_acq(tabh) || budget-- == 0)
      return -1;
    if (id >= limit) {
      id = ctype_next_acq(ctype_tab_slot(tabh, id));
      continue;
    }
    ctype_layout_copy(cts, version, tabh, id, &ct);
    info = ct.info;
    if (!ctype_isabandoned(info) && ctype_nameobj_acq(&ct) == obj2gco(name) &&
        ((tmask >> ctype_type(info)) & 1)) {
      if (redirp) {
        *redirp = name;
        if (ct.sib) {
          CType sib;
          if (ct.sib >= limit || ct.sib >= ctype_tab_sizetab_acq(tabh))
            return -1;
          ctype_layout_copy(cts, version, tabh, ct.sib, &sib);
          if (ctype_isabandoned(sib.info)) return 0;
          if (ctype_isxattrib(sib.info, CTA_REDIR)) {
            *redirp = ctype_name_acq(&sib);
            if (!*redirp) return -1;
          }
        }
      }
      *out = ct;
      *idp = id;
      return 1;
    }
    id = ct.next;
  }
  return 0;
}

/* Speculative callers may decline the operation during a declaration. */
int lj_ctype_getname_snapshot(CTState *cts, GCstr *name, uint32_t tmask,
                              CTypeID *idp, CType *out, GCstr **redirp)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  int ok;
  if (seq & 1u) return -1;
  ok = ctype_getname_at(cts, ctype_layout_version(cts), name, tmask,
                        idp, out, redirp);
  return ctype_snapshot_done(cts, seq, ok);
}

int lj_ctype_getname_wait(lua_State *L, CTState *cts, GCstr *name,
                          uint32_t tmask, CTypeID *idp, CType *out,
                          GCstr **redirp)
{
  for (;;) {
    uint64_t version = ctype_layout_version(cts);
    int ok = ctype_getname_at(cts, version, name, tmask, idp, out, redirp);
    if (ctype_layout_version(cts) != version) continue;
    if (ok >= 0) return ok;
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

static int ctype_getfieldq_at(CTState *cts, uint64_t version,
			      CTypeTab *tabh, CTypeID top, CTypeID sib,
			      GCstr *name, CTSize baseofs, CTInfo basequal,
			      CTSize *ofsp, CTInfo *qualp, CType *out,
			      MSize *budget)
{
  while (sib) {
    CType ct;
    CTInfo info;
    CTSize size;
    if (sib >= top || sib >= ctype_tab_sizetab_acq(tabh) ||
	(*budget)-- == 0)
      return -1;
    if (!ctype_layout_at(cts, version, tabh, top, sib, &ct))
      return 0;
    info = ct.info;
    size = ct.size;
    if (ctype_nameobj_acq(&ct) == obj2gco(name)) {
      *out = ct;
      *ofsp = baseofs + size;
      if (qualp)
	*qualp |= basequal;
      return 1;
    }
    if (ctype_isxattrib(info, CTA_SUBTYPE)) {
      CTInfo q = basequal;
      CTypeID cid = ctype_cid(info);
      CType cct;
      int ok;
      for (;;) {
	CTInfo cinfo;
	if (cid >= top || cid >= ctype_tab_sizetab_acq(tabh) ||
	    (*budget)-- == 0)
	  return -1;
	if (!ctype_layout_at(cts, version, tabh, top, cid, &cct))
	  return 0;
	cinfo = cct.info;
	if (!ctype_isattrib(cinfo))
	  break;
	if (ctype_attrib(cinfo) == CTA_QUAL)
	  q |= cct.size;
	cid = ctype_cid(cinfo);
      }
      ok = ctype_getfieldq_at(cts, version, tabh, top, cct.sib,
			      name, baseofs + size, q, ofsp, qualp, out, budget);
      if (ok)
	return ok;
    }
    sib = ct.sib;
  }
  return 0;
}

/* Sequence-checked struct/union field lookup for stable readers. */
int lj_ctype_getfieldq_snapshot(CTState *cts, const CType *root,
				GCstr *name, CTSize *ofsp, CTInfo *qualp,
				CType *out)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  CTypeID top;
  CTInfo qual = qualp ? *qualp : 0;
  MSize budget;
  int ok;
  if (seq & 1u)
    return -1;
  if (!ctype_isstruct(ctype_info_acq(root)))
    return ctype_snapshot_done(cts, seq, 0);
  top = ctype_top_acq(cts);
  budget = top ? (MSize)top * 4u : 1u;
  ok = ctype_getfieldq_at(cts, ctype_layout_version(cts), &cts->tab,
			 top, ctype_sib_acq(root), name, 0, 0, ofsp,
			 qualp ? &qual : NULL, out, &budget);
  ok = ctype_snapshot_done(cts, seq, ok);
  if (ok > 0 && qualp)
    *qualp = qual;
  return ok;
}

int lj_ctype_getfieldq_wait(lua_State *L, CTState *cts, CTypeID rootid,
			    GCstr *name, CTSize *ofsp, CTInfo *qualp,
			    CType *out)
{
  for (;;) {
    uint32_t seq = ctype_parse_token_acq(cts);
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, rootid);
    CTypeTab *tabh = &cts->tab;
    CType root, field;
    CTInfo qual = qualp ? *qualp : 0;
    CTSize ofs;
    int ok = 0;
    if (ctype_layout_at(cts, version, tabh, top, rootid, &root) &&
	ctype_isstruct(root.info)) {
      MSize budget = (MSize)top * 4u;
      ok = ctype_getfieldq_at(cts, version, tabh, top, root.sib, name,
			     0, 0, &ofs, qualp ? &qual : NULL, &field, &budget);
    }
    if (ctype_layout_version(cts) != version) continue;
    if (ok > 0) {
      *out = field;
      *ofsp = ofs;
      if (qualp)
	*qualp = qual;
      return 1;
    }
    if (!ok && (rootid < top || !(version & 1u))) return 0;
    lj_ctype_parse_wait(cts, L, seq);
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
  tabh = &cts->tab;
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

void lj_ctype_save_layout(lua_State *L, CTState *cts, CTypeID id,
			  const CType *old)
{
  uint32_t i = id >> CTYPE_LAYOUT_PAGE_BITS;
  CTypeLayoutPage *page = ctype_layout_page(cts, i);
  CTypeLayoutUndo *undo;
  if (!page) {
    page = lj_mem_newt(L, sizeof(*page), CTypeLayoutPage);
    memset(page, 0, sizeof(*page));
    (void)lj_gc2_markmem(G(L), page);
    la_storeptr_rel((void **)&cts->layout_pages[i], page);
    (void)lj_gc2_markmem(G(L), page);
  }
  undo = &page->slots[id & (CTYPE_LAYOUT_PAGE_SIZE-1)];
  ctype_copy_rel(&undo->old, old);
  /* Publish the rollback image before any provisional live field. */
  la_store64_rel(&undo->version, ctype_layout_version(cts));
}

/* Read a committed parser layout or an independently published structural type. */
int lj_ctype_read(lua_State *L, CTState *cts, CTypeID id, CType *out)
{
  if (id <= CTID_CTYPEID) {
    if (!id) return 0;
    ctype_copy_rel(out, ctype_get(cts, id));
    return !ctype_isabandoned(out->info);
  }
  for (;;) {
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, id);
    int ok = ctype_layout_at(cts, version, &cts->tab, top, id, out);
    if (ctype_layout_version(cts) != version) continue;
    if (id < top || !(version & 1u)) return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int ctype_info_at(CTState *cts, uint64_t version, CTypeID top,
                         CTypeID id, CTInfo *infop, CTSize *szp,
                         CTypeID *ridp, CType *rawp)
{
  CTypeTab *tabh = &cts->tab;
  MSize budget = top ? (MSize)top * 4u : 1u;
  CType ct;
  CTInfo qual = 0;
  if (rawp || ridp) {
    CTypeID rid = id;
    for (;;) {
      CTInfo info;
      if (budget-- == 0)
	return -1;
      if (!ctype_layout_at(cts, version, tabh, top, rid, &ct))
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
    if (budget-- == 0)
      return -1;
    if (!ctype_layout_at(cts, version, tabh, top, id, &ct))
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
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      return 1;
    }
    id = ctype_cid(info);
  }
}

int lj_ctype_info_snapshot(CTState *cts, CTypeID id, CTInfo *infop,
                           CTSize *szp, CTypeID *ridp, CType *rawp)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  int ok;
  if (seq & 1u) return -1;
  ok = ctype_info_at(cts, ctype_layout_version(cts), ctype_top_acq(cts),
                     id, infop, szp, ridp, rawp);
  return ctype_snapshot_done(cts, seq, ok);
}

int lj_ctype_info_wait(lua_State *L, CTState *cts, CTypeID id,
                       CTInfo *infop, CTSize *szp, CTypeID *ridp,
                       CType *rawp)
{
  int ok = lj_ctype_info_predefined(cts, id, infop, szp, ridp, rawp);
  if (ok > 0) return ok;
  for (;;) {
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, id);
    ok = ctype_info_at(cts, version, top, id, infop, szp, ridp, rawp);
    if (ctype_layout_version(cts) != version) continue;
    if (ok >= 0 && (id < top || !(version & 1u))) return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

int lj_ctype_metaroot_init_nothrow(lua_State *L, LJCTypeMetaRoot *root)
{
  TValue nilv, *slot;
  TGState *tg;
  uint32_t idx;
  if (!L || !root)
    return 0;
  root->tg = NULL;
  root->idx = 0;
  root->active = 0;
  tg = L2TG(L);
  if (!tg || !lj_tg_root_anchor_reserve_nothrow(L, tg))
    return 0;
  setnilV(&nilv);
  slot = lj_tg_root_anchor_push(L, tg, &nilv, &idx);
  if (!slot)
    return 0;
  root->tg = tg;
  root->idx = idx;
  root->active = 1;
  return 1;
}

TValue *lj_ctype_metaroot_tv(const LJCTypeMetaRoot *root)
{
  return root && root->active ?
    lj_tg_root_anchor_slot_acq(root->tg, root->idx) : NULL;
}

void lj_ctype_metaroot_release(LJCTypeMetaRoot *root)
{
  if (!root || !root->active)
    return;
  lj_assertX(lj_tg_root_anchor_top_acq(root->tg) == root->idx + 1u,
	     "ctype metamethod roots must be released in LIFO order");
  lj_tg_root_anchor_pop(root->tg, root->idx);
  root->tg = NULL;
  root->idx = 0;
  root->active = 0;
}

/* Root one ctype metamethod without waiting. CType slots and the metatable
** directory live until state close. Each lookup pins its hash vector and
** result through publication into the next root. Parser
** sequencing validates only CType topology; metatype CAS and ordinary table
** mutation linearize independently through acquire/held table reads.
**
** The function-pointer case is semantic topology, not an ABI/signature shape:
** every pointer-to-function CType uses the same miscmap -> shared metatable
** hop, irrespective of argument and result classes. */
static int ctype_metatv_rooted_try(lua_State *L, CTState *cts, TValue *outroot,
				  CTypeID id, MMS mm, int committed)
{
  global_State *g;
  LJGC2Lease sourcelease = { 0 }, mtlease = { 0 }, resultlease = { 0 };
  CTypeTab *tabh;
  CTypeID top;
  CType ct;
  CTInfo info;
  MSize budget;
  uint32_t seq0;
  uint64_t version;
  TValue sourcev, mtv, result, nilv;
  GCtab *source, *mt;
  int function_meta = 0;
  int lookup;
  int rc = LJ_CTYPE_METATV_RETRY;

  if (!L || !cts || !outroot)
    return LJ_CTYPE_METATV_RETRY;
  g = G(L);
  setnilV(&nilv);
  if (lj_ctype_predefined_nometa(cts, id)) {
    copyTVrel(L, outroot, &nilv);
    return LJ_CTYPE_METATV_ABSENT;
  }
  seq0 = ctype_parse_token_acq(cts);
  if (!committed && (seq0 & 1u))
    goto busy;
  version = ctype_layout_version(cts);

  top = committed ? ctype_layout_limit(cts, version, id) : ctype_top_acq(cts);
  tabh = &cts->tab;
  budget = top ? (MSize)top * 4u : 1u;
  for (;;) {
    if (budget-- == 0)
      goto retry;
    if (committed && (version & 1u) && id >= top)
      goto unsettled;
    if (!ctype_layout_at(cts, version, tabh, top, id, &ct))
      goto absent;
    info = ctype_info_acq(&ct);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      break;
    id = ctype_cid(info);
  }
  if (ctype_isptr(info)) {
    CType cct;
    CTypeID cid = ctype_cid(info);
    if (budget-- == 0)
      goto retry;
    if (committed && (version & 1u) && cid >= top)
      goto unsettled;
    if (!ctype_layout_at(cts, version, tabh, top, cid, &cct))
      goto absent;
    function_meta = ctype_isfunc(ctype_info_acq(&cct));
  }

  if (function_meta) {
    source = ctype_miscmap_acq(cts);
  } else {
    source = ctype_meta_tab(cts, id);
  }
  if (!source)
    goto absent;

  /* CTState carries raw GC roots, not TValues. Manufacture only the known TAB
  ** tag, then validate/retain the exact incarnation before dereferencing it. */
  setgcVraw(&sourcev, obj2gco(source), LJ_TTAB);
  if (lj_gc2_tv_lease_acquire(g, &sourcev, &sourcelease) !=
      LJ_GC2_TV_EDGE_VALID)
    goto retry;
  mt = source;

  if (function_meta) {
    lookup = lj_tab_getstr_leased_try(g, source, &g->strempty, &mtv, &mtlease);
    if (lookup == LJ_TAB_GC_LOOKUP_RETRY)
      goto retry;
    if (lookup != LJ_TAB_GC_LOOKUP_FOUND)
      goto absent;
    if (!tvistab(&mtv))
      goto absent;
    mt = tabV(&mtv);
  }

  lookup = lj_tab_getstr_leased_try(g, mt, mmname_str(g, mm), &result,
                                    &resultlease);
  if (lookup == LJ_TAB_GC_LOOKUP_RETRY)
    goto retry;
  if (lookup != LJ_TAB_GC_LOOKUP_FOUND)
    goto absent;
  rc = LJ_CTYPE_METATV_FOUND;
  goto publish;

absent:
  rc = LJ_CTYPE_METATV_ABSENT;
publish:
  if (committed ? ctype_layout_version(cts) != version :
		  ctype_parse_token_acq(cts) != seq0) {
    if (committed)
      goto retry;
    goto busy;
  }
  copyTVrel(L, outroot, rc == LJ_CTYPE_METATV_FOUND ? &result : &nilv);
  goto done;

unsettled:
  if (ctype_layout_version(cts) != version)
    goto retry;
busy:
  rc = LJ_CTYPE_METATV_CTBUSY;
  goto done;

retry:
  rc = LJ_CTYPE_METATV_RETRY;

done:
  /* Publish only a terminal, sequence-valid result. The exact result lease
  ** remains held across GC2's publication hook and is released afterwards. */
  if (rc == LJ_CTYPE_METATV_FOUND)
    lj_gc_pubroot(L, outroot);
  lj_gc2_lease_release(&resultlease);
  lj_gc2_lease_release(&mtlease);
  lj_gc2_lease_release(&sourcelease);
  return rc;
}

int lj_ctype_metatv_rooted_try(lua_State *L, CTState *cts, TValue *outroot,
			       CTypeID id, MMS mm)
{
  return ctype_metatv_rooted_try(L, cts, outroot, id, mm, 0);
}

int lj_ctype_metatv_committed_try(lua_State *L, CTState *cts, TValue *outroot,
				 CTypeID id, MMS mm)
{
  return ctype_metatv_rooted_try(L, cts, outroot, id, mm, 1);
}

/* -- C type information -------------------------------------------------- */

/* Follow references and get raw type for a C type ID. */
CType *lj_ctype_rawref(CTState *cts, CTypeID id)
{
  return ctype_get(cts, ctype_rawrefid(cts, id));
}

static int ctype_rawref_at(CTState *cts, uint64_t version, CTypeID top,
			   CTypeID id, CTypeID *ridp, CType *out)
{
  CTypeTab *tabh = &cts->tab;
  MSize budget = top ? (MSize)top * 4u : 1u;
  CType ct;
  while (budget--) {
    if (!ctype_layout_at(cts, version, tabh, top, id, &ct))
      return 0;
    if (!(ctype_isattrib(ct.info) || ctype_isref(ct.info))) {
      if (ridp)
	*ridp = id;
      if (out)
	*out = ct;
      return 1;
    }
    id = ctype_cid(ct.info);
  }
  return -1;
}

/* Sequence-checked raw-ref type snapshot for stable recorder readers. */
int lj_ctype_rawref_snapshot(CTState *cts, CTypeID id, CTypeID *ridp,
			     CType *out)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  int ok;
  if (seq & 1u)
    return -1;
  ok = ctype_rawref_at(cts, ctype_layout_version(cts), ctype_top_acq(cts),
		       id, ridp, out);
  return ctype_snapshot_done(cts, seq, ok);
}

int lj_ctype_rawref_wait(lua_State *L, CTState *cts, CTypeID id,
			 CTypeID *ridp, CType *out)
{
  int ok = lj_ctype_rawref_predefined(cts, id, ridp, out);
  if (ok >= 0)
    return ok;
  for (;;) {
    uint32_t seq = ctype_parse_token_acq(cts);
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, id);
    ok = ctype_rawref_at(cts, version, top, id, ridp, out);
    if (ctype_layout_version(cts) != version) continue;
    if (ok >= 0 && (id < top || !(version & 1u))) return ok;
    lj_ctype_parse_wait(cts, L, seq);
  }
}

/* Get size for a C type ID. Does NOT support VLA/VLS. */
CTSize lj_ctype_size(CTState *cts, CTypeID id)
{
  CType *ct = ctype_raw(cts, id);
  CTInfo info = ctype_info_acq(ct);
  return ctype_hassize(info) ? ctype_size_acq(ct) : CTSIZE_INVALID;
}

static int ctype_size_at(CTState *cts, uint64_t version, CTypeID top,
			 CTypeID id, CTSize *szp)
{
  CTypeTab *tabh = &cts->tab;
  MSize budget = top ? (MSize)top * 2u : 1u;
  if (id == 0 || id >= top)
    return 0;
  while (budget--) {
    CType ct;
    if (id == 0 || id >= top || id >= ctype_tab_sizetab_acq(tabh))
      return -1;
    if (!ctype_layout_at(cts, version, tabh, top, id, &ct))
      return 0;
    if (!ctype_isattrib(ct.info)) {
      *szp = ctype_hassize(ct.info) ? ct.size : CTSIZE_INVALID;
      return 1;
    }
    id = ctype_cid(ct.info);
  }
  return -1;
}

/* Sequence-checked size snapshot for stable non-parser ctype readers. */
int lj_ctype_size_snapshot(CTState *cts, CTypeID id, CTSize *szp)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  int ok;
  if (seq & 1u)
    return -1;
  ok = ctype_size_at(cts, ctype_layout_version(cts), ctype_top_acq(cts),
		     id, szp);
  return ctype_snapshot_done(cts, seq, ok);
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
  tabh = &cts->tab;
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
  CTInfo info;
  UNUSED(cts);
  if (!ctype_predefined_id(id))
    return 0;
  /* Bootstrap records have immutable type and metatable eligibility. */
  info = lj_ctype_typeinfo[id] & 0xffff03ffu;
  if (ctype_isstruct(info) || ctype_iscomplex(info) || ctype_isvector(info))
    return 0;
  if (ctype_isptr(info)) {
    CTypeID cid = ctype_cid(info);
    if (!ctype_predefined_id(cid))
      return 0;
    info = lj_ctype_typeinfo[cid] & 0xffff03ffu;
    if (ctype_isfunc(info))
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
  tabh = &cts->tab;
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
  tabh = &cts->tab;
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
    uint32_t seq = ctype_parse_token_acq(cts);
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, id);
    ok = ctype_size_at(cts, version, top, id, szp);
    if (ctype_layout_version(cts) != version) continue;
    if (ok > 0) return 1;
    if (!ok && (id < top || !(version & 1u))) {
      *szp = CTSIZE_INVALID;
      return 0;
    }
    lj_ctype_parse_wait(cts, L, seq);
  }
}

static int ctype_enumconst_at(CTState *cts, uint64_t version, CTypeTab *tabh,
			      CTypeID top, CTypeID id, GCstr *name,
			      CTSize *valp, CTypeID *cidp)
{
  MSize budget = top ? (MSize)top * 2u : 1u;
  while (id) {
    CType ct;
    if (id >= top || id >= ctype_tab_sizetab_acq(tabh) || budget-- == 0)
      return -1;
    if (!ctype_layout_at(cts, version, tabh, top, id, &ct))
      return 0;
    if (ctype_nameobj_acq(&ct) == obj2gco(name) && ctype_isconstval(ct.info)) {
      *valp = ct.size;
      *cidp = ctype_cid(ct.info);
      return 1;
    }
    id = ct.sib;
  }
  return 0;
}

/* Sequence-checked enum string constant lookup for stable readers. */
int lj_ctype_enumconst_snapshot(CTState *cts, const CType *root,
				GCstr *name, CTSize *valp, CTypeID *cidp)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  int ok;
  if (seq & 1u)
    return -1;
  if (!ctype_isenum(ctype_info_acq(root)))
    return ctype_snapshot_done(cts, seq, 0);
  ok = ctype_enumconst_at(cts, ctype_layout_version(cts), &cts->tab,
			 ctype_top_acq(cts), ctype_sib_acq(root), name, valp, cidp);
  return ctype_snapshot_done(cts, seq, ok);
}

int lj_ctype_enumconst_wait(lua_State *L, CTState *cts, CTypeID rootid,
			    GCstr *name, CTSize *valp, CTypeID *cidp)
{
  for (;;) {
    uint32_t seq = ctype_parse_token_acq(cts);
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, rootid);
    CTypeTab *tabh = &cts->tab;
    CType root;
    CTypeID cid;
    CTSize val;
    int ok = 0;
    if (ctype_layout_at(cts, version, tabh, top, rootid, &root) &&
	ctype_isenum(root.info))
      ok = ctype_enumconst_at(cts, version, tabh, top, root.sib, name,
			     &val, &cid);
    if (ctype_layout_version(cts) != version) continue;
    if (ok > 0) {
      *valp = val;
      *cidp = cid;
      return 1;
    }
    if (!ok && (rootid < top || !(version & 1u))) return 0;
    lj_ctype_parse_wait(cts, L, seq);
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

/* -- C type representation ----------------------------------------------- */

/* Fixed max. length of a C type representation. */
#define CTREPR_MAX		512

typedef struct CTRepr {
  char *pb, *pe;
  CTState *cts;
  CTypeTab *tabh;
  CTypeID top;
  uint64_t version;
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

static int ctype_repr_copy(CTRepr *ctr, CTypeID id, CType *out)
{
  if (ctr->tabh)
    return ctype_layout_at(ctr->cts, ctr->version, ctr->tabh, ctr->top,
			   id, out);
  ctype_copy_rel(out, ctype_get(ctr->cts, id));
  return 1;
}

static void ctype_repr(CTRepr *ctr, CTypeID id)
{
  CTInfo qual = 0;
  int ptrto = 0;
  while (ctr->ok) {
    CType ct;
    CTInfo info;
    CTSize size;
    if (!ctype_repr_copy(ctr, id, &ct)) { ctr->ok = 0; return; }
    info = ct.info;
    size = ct.size;
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
      ctype_preptype(ctr, id, &ct, qual, (info & CTF_UNION) ? "union" : "struct");
      return;
    case CT_ENUM:
      if (id == CTID_CTYPEID) {
	ctype_preplit(ctr, "ctype");
	return;
      }
      ctype_preptype(ctr, id, &ct, qual, "enum");
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
	  CType child;
	  if (!ctype_repr_copy(ctr, ctype_cid(info), &child)) {
	    ctr->ok = 0;
	    return;
	  }
	  ctype_appnum(ctr, child.size ? size/child.size : 0);
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
      ctr->ok = 0;
      return;
    }
    id = ctype_cid(info);
  }
}

static void ctype_repr_format(CTRepr *ctr, CTypeID id, GCstr *name)
{
  ctr->pb = ctr->pe = &ctr->buf[CTREPR_MAX/2];
  ctr->ok = 1;
  ctr->needsp = 0;
  if (name) ctype_prepstr(ctr, strdata(name), name->len);
  ctype_repr(ctr, id);
}

/* Format the parser's own provisional type for a parser diagnostic. */
GCstr *lj_ctype_repr(lua_State *L, CTypeID id, GCstr *name)
{
  CTRepr ctr;
  ctr.cts = ctype_ctsG(G(L));
  ctr.tabh = NULL;
  ctype_repr_format(&ctr, id, name);
  if (LJ_UNLIKELY(!ctr.ok)) return lj_str_newlit(L, "?");
  return lj_str_new(L, ctr.pb, ctr.pe - ctr.pb);
}

GCstr *lj_ctype_repr_wait(lua_State *L, CTypeID id, GCstr *name)
{
  CTState *cts = ctype_ctsG(G(L));
  if (id <= CTID_CTYPEID)
    return lj_ctype_repr(L, id, name);
  for (;;) {
    uint32_t seq = ctype_parse_token_acq(cts);
    uint64_t version = ctype_layout_version(cts);
    CTypeID top = ctype_layout_limit(cts, version, id);
    CTRepr ctr;
    if (id >= top && (version & 1u)) {
      lj_ctype_parse_wait(cts, L, seq);
      continue;
    }
    ctr.cts = cts;
    ctr.tabh = &cts->tab;
    ctr.top = top;
    ctr.version = version;
    ctype_repr_format(&ctr, id, name);
    if (ctype_layout_version(cts) != version) continue;
    if (LJ_UNLIKELY(!ctr.ok)) return lj_str_newlit(L, "?");
    return lj_str_new(L, ctr.pb, ctr.pe - ctr.pb);
  }
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

/* New registrations prepend to both spines. Select a finished cohort before
** unlinking anything: a publisher suspended between its two head CASes may
** still add its node to the ordered list. Only the metadata writer changes
** interior links during this pass. A raced head CAS leaves work for a later
** pass, while preserving the newly prepended registrations. */
static void ctype_fin_order_reclaim(global_State *g, CTState *cts)
{
  FinRegOrderNode *cur, *prev = NULL;
  if (!la_xchg32_acqrel(&cts->fin_order_has_inactive, 0))
    return;
  for (cur = fin_order_lifetime_acq(cts); cur;
       cur = fin_order_lifetime_next_acq(cur)) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known_reclaim_held(g, cur))) {
      lj_assertG(0, "invalid FINREG lifetime record in reclaim");
      abort();
    }
    if (!fin_order_active_acq(cur)) {
      if (!cur->registration_id || la_load32_acq(&cur->submission_done))
        cur->reclaim_selected = 1;
      else
        la_store32_rel(&cts->fin_order_has_inactive, 1);
    }
  }
  for (cur = fin_order_head_acq(cts); cur;) {
    FinRegOrderNode *next = fin_order_next_acq(cur);
    if (!cur->reclaim_selected)
      prev = cur;
    else if (prev)
      fin_order_next_rel(prev, next);
    else {
      FinRegOrderNode *expect = cur;
      if (!fin_order_head_cas(cts, &expect, next))
        goto retry;
    }
    cur = next;
  }
  prev = NULL;
  for (cur = fin_order_lifetime_acq(cts); cur;) {
    FinRegOrderNode *next = fin_order_lifetime_next_acq(cur);
    if (cur->reclaim_selected) {
      if (prev)
        fin_order_lifetime_next_rel(prev, next);
      else {
        FinRegOrderNode *expect = cur;
        if (!fin_order_lifetime_cas(cts, &expect, next))
          goto retry;
      }
      cur->retired_next = cts->fin_retired_orders;
      cts->fin_retired_orders = cur;
    } else {
      prev = cur;
    }
    cur = next;
  }
  return;
retry:
  la_store32_rel(&cts->fin_order_has_inactive, 1);
}

static int ctype_fin_generation_empty(global_State *g, FinRegGen *gen)
{
  GCtab *tab = fin_gen_tab_acq(gen);
  Node *nodes = gen->insert_nodes;
  MSize i, hmask = gen->insert_hmask;
  if (finreg_state(finreg_load(gen)) || !nodes || !tab ||
      !lj_gc2_mem_registered_known_reclaim_held(g, tab) ||
      tab->gct != (uint8_t)~LJ_TTAB ||
      lj_tab_node_acq(tab) != nodes || lj_tab_hmask_acq(tab) != hmask ||
      !lj_gc2_mem_registered_known_reclaim_held(g, lj_tab_node_hdrw(nodes)) ||
      lj_tab_node_is_retiring(nodes))
    return 0;
  for (i = 0; i <= hmask; i++) {
    TValue key;
    lj_tv_load_acq(&key, &nodes[i].key);
    /* The private generation contains __mode and its one cdata key.
    ** Preserve a record if any other slot could still carry a value. */
    if (!tvisstr(&key) && !lj_tv_isnil_acq(&nodes[i].val))
      return 0;
  }
  return 1;
}

/* Seal an empty callback with the same CAS that registers it. A delayed
** publisher may retain the record, but it must retry on a sealed generation.
** Unlink before testing reader quiescence, including for a raced head insert. */
static void ctype_fin_empty_generations_reclaim(global_State *g, CTState *cts)
{
  FinRegGen *gen = fin_gen_head_acq(cts), *prev = NULL;
  while (gen) {
    FinRegGen *next;
    la_u128 empty = { 0, 0 }, retired = { 0, FINREG_RETIRING };
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known_reclaim_held(g, gen))) {
      lj_assertG(0, "invalid FINREG generation in reclaim");
      abort();
    }
    next = fin_gen_next_acq(gen);
    if (finreg_state(finreg_load(gen)) == FINREG_RETIRING ||
        (ctype_fin_generation_empty(g, gen) && finreg_cas(gen, &empty, retired))) {
#if defined(LJ_CDATA_TEST_HELPERS)
      lj_cdata_test_fin_pause_at(LJ_CDATA_FIN_PAUSE_GEN_RETIRED);
#endif
      if (prev)
        fin_gen_next_rel(prev, next);
      else {
        FinRegGen *expect = gen;
        if (!fin_gen_head_cas(cts, &expect, next))
          return;
      }
      gen->retired_next = cts->fin_retired_gens;
      cts->fin_retired_gens = gen;
    } else {
      prev = gen;
    }
    gen = next;
  }
}

static uint32_t ctype_fin_free_retired(global_State *g, CTState *cts)
{
  uint32_t n = 0;
  la_fence_seq();  /* Unlink -> zero readers pairs with enter -> head load. */
  if (la_load32_acq(&cts->fin_native_readers))
    return 0;
  while (cts->fin_retired_orders) {
    FinRegOrderNode *ord = cts->fin_retired_orders;
    cts->fin_retired_orders = ord->retired_next;
    lj_mem_freet(g, ord);
    n++;
  }
  while (cts->fin_retired_gens) {
    FinRegGen *gen = cts->fin_retired_gens;
    cts->fin_retired_gens = gen->retired_next;
    lj_mem_freet(g, gen);
    n++;
  }
  return n;
}

uint32_t lj_ctype_fin_reclaim(global_State *g)
{
  CTState *cts = ctype_ctsG(g);
  if (!cts) return 0;
  ctype_fin_order_reclaim(g, cts);
  ctype_fin_empty_generations_reclaim(g, cts);
  return ctype_fin_free_retired(g, cts);
}

CTState *LJ_FASTCALL lj_ctype_ctsG_acq(global_State *g)
{
  CTState *cts;
  /* The callback trampoline calls this immediately before both the incoming
  ** prepare helper and the outgoing leave helper. Keep that assembler ABI,
  ** but make the intervening C boundary explicitly transparent to the native
  ** errno/LastError pair (including compiler instrumentation builds). */
  ERRNO_SAVE
  cts = ctype_ctsG(g);
  ERRNO_RESTORE
  return cts;
}

/* -- C type state -------------------------------------------------------- */

/* Initialize C type table and state. */
CTState *lj_ctype_init(lua_State *L)
{
  CTState *cts = lj_mem_newt(L, sizeof(CTState), CTState);
  const char *name = lj_ctype_typenames;
  CTypeID id;
  memset(cts, 0, sizeof(CTState));
  LJ_STATIC_ASSERT((CTYPE_TAB_MIN << CTYPE_TAB_SPANS) == CTID_MAX);
  LJ_STATIC_ASSERT(CTTYPEINFO_NUM <= CTYPE_TAB_MIN);
  cts->tab.sizetab = CTYPE_TAB_MIN;
  ctype_top_rel(cts, CTTYPEINFO_NUM);
  ctype_metamap_init_l(L, cts);
  ctype_cbblack_init_l(L, cts);
  cts->g = G(L);
  for (id = 0; id < CTTYPEINFO_NUM; id++) {
    CType *ct = ctype_tab_slot(&cts->tab, id);
    CTInfo info = lj_ctype_typeinfo[id];
    la_store32_rel(&ct->internstate, CTINTERN_PUBLIC);
    ctype_size_rel(ct, (CTSize)((int32_t)(info << 16) >> 26));
    ctype_info_rel(ct, info & 0xffff03ffu);
    ctype_sib_rel(ct, 0);
    if (ctype_type(info) == CT_KW || ctype_istypedef(info)) {
      size_t len = strlen(name);
      GCstr *str = lj_str_new(L, name, len);
      ctype_setname(cts->g, ct, str);
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
    ptrdiff_t anchor = savestack(L, L->top);
    GCtab *t = ctype_fin_tab_new_anchor_l(L, NULL);
    FinRegGen *gen, *empty = NULL;
    gen = ctype_fin_gen_new_l(L, t);
    (void)lj_gc2_markmem(G(L), gen);
    lj_gc_pubobjroot(L, obj2gco(t));
    if (fin_gen_head_cas(cts, &empty, gen))
      (void)lj_gc2_markmem(G(L), gen);
    else
      lj_mem_freet(G(L), gen);
    lj_gc_pubobjroot(L, obj2gco(t));
    L->top = restorestack(L, anchor);
  }
  return cts;
}

/* Free C type table and state. */
void lj_ctype_freestate(global_State *g)
{
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    uint32_t i;
    for (i = 0; i < CTYPE_LAYOUT_PAGES; i++) {
      CTypeLayoutPage *page = ctype_layout_page(cts, i);
      if (page) lj_mem_freet(g, page);
    }
    lj_ccallback_mcode_free(cts);
    lj_ctype_fin_freetabs(g, cts);
    for (i = 0; i < CTYPE_TAB_SPANS; i++) {
      CType *span = ctype_tab_span_acq(&cts->tab, i);
      if (span) lj_mem_freevec(g, span, CTYPE_TAB_MIN << i, CType);
    }
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
