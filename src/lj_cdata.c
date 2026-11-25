/*
** C data management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_err.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_oserr.h"
#include "lj_thr.h"
#include "lj_vm.h"

/* -- C data allocation --------------------------------------------------- */

GCcdata *lj_cdata_new_forjit(lua_State *L, CTypeID id, CTSize sz)
{
  return lj_cdata_new_(L, id, sz);
}

/* Allocate a new C data object holding a reference to another object. */
GCcdata *lj_cdata_newref_l(lua_State *L, CTState *cts, const void *p,
			   CTypeID id)
{
  CTypeID refid = lj_ctype_intern_l(L, cts, CTINFO_REF(id), CTSIZE_PTR);
  GCcdata *cd = lj_cdata_new_l(L, cts, refid, CTSIZE_PTR);
  *(const void **)cdataptr(cd) = p;
  return cd;
}

/* Allocate variable-sized or specially aligned C data object. */
GCcdata *lj_cdata_newv(lua_State *L, CTypeID id, CTSize sz, CTSize align)
{
  global_State *g;
  MSize extra = sizeof(GCcdataVar) + sizeof(GCcdata) +
		(align > CT_MEMALIGN ? (1u<<align) - (1u<<CT_MEMALIGN) : 0);
  GCSize allocsz = (GCSize)extra + (GCSize)sz;
  char *p;
  uintptr_t adata;
  uintptr_t almask;
  GCcdata *cd;
  LJOSerrState oserr;
  /* Same contract for traced VLA/VLS and over-aligned CNEW materialization. */
  lj_oserr_save(&oserr);
  p = (char *)lj_mem_newgco_raw_nothrow(
    L, allocsz, LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  if (LJ_UNLIKELY(p == NULL)) {
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  adata = (uintptr_t)p + sizeof(GCcdataVar) + sizeof(GCcdata);
  almask = (1u << align) - 1u;
  cd = (GCcdata *)(((adata + almask) & ~almask) - sizeof(GCcdata));
  lj_assertL((char *)cd - p < 65536, "excessive cdata alignment");
  cdatav(cd)->offset = (uint16_t)((char *)cd - p);
  /* Keep the same offset at the allocation base. With ordinary alignment this
  ** aliases cdatav.offset; with over-alignment it occupies otherwise-unused
  ** prefix padding. Bitmap sweep can therefore recover the exact GC header
  ** from an allocation start without repurposing gcw before an SMR grace. */
  *(uint16_t *)(void *)p = cdatav(cd)->offset;
  cdatav(cd)->extra = extra;
  cdatav(cd)->len = sz;
  g = G(L);
  cd->gct = ~LJ_TCDATA;
  cd->ctypeid = id;
  cdata_flags_rel(cd, cdata_size_flags(allocsz));
  newwhite(g, obj2gco(cd));
  lj_obj_addgcflags(obj2gco(cd), 0x80);
  if (LJ_UNLIKELY(!lj_mem_publish_interior_cdata(L, p, allocsz))) {
    lj_mem_freegco_unpublished(g, p, allocsz);
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  lj_gc_linkobj_new_at(g, obj2gco(cd), p);
  lj_oserr_restore(&oserr);
  return cd;
}

GCcdata *lj_cdata_newx_l(lua_State *L, CTState *cts, CTypeID id, CTSize sz,
			 CTInfo info)
{
  if (!(info & CTF_VLA) && ctype_align(info) <= CT_MEMALIGN)
    return lj_cdata_new_l(L, cts, id, sz);
  else
    return lj_cdata_newv(L, id, sz, ctype_align(info));
}

static LJ_NORET LJ_NOINLINE void cdata_free_finalizer_invariant(global_State *g)
{
  lj_gc2_finreg_cdata_note_sweep_queued(g);
  lj_assertG_(g, 0, "cdata finalizer reached sweep/free outside FINREG");
  abort();
}

static int cdata_raw_info_safe(CTState *cts, CTypeID id, CTInfo *infop,
			       CTSize *sizep)
{
  CTypeTab *tabh;
  CTypeID top;
  MSize sizetab;
  uint32_t guard = 0;
  if (!cts || id == 0)
    return 0;
  tabh = &cts->tab;
  top = ctype_top_acq(cts);
  sizetab = ctype_tab_sizetab_acq(tabh);
  for (;;) {
    CType *ct;
    CTInfo info;
    if (id == 0 || id >= top || id >= sizetab || ++guard > CTID_MAX)
      return 0;
    ct = ctype_tab_slot(tabh, id);
    info = ctype_info_acq(ct);
    if (ctype_isabandoned(info))
      return 0;
    if (!ctype_isattrib(info)) {
      if (infop) *infop = info;
      if (sizep) *sizep = ctype_size_acq(ct);
      return 1;
    }
    id = ctype_cid(info);
  }
}

int lj_cdata_validate(global_State *g, GCcdata *cd, void **basep,
		      GCSize *sizep)
{
  CTState *cts;
  CTInfo info;
  CTSize sz;
  void *base;
  GCSize size;
  if (!g || !cd || cd->gct != ~LJ_TCDATA)
    return 0;
  cts = ctype_ctsG(g);
  if (!cts) {
    /* Fixed predefined cdata may be materialized by VM/JIT construction before
    ** the FFI CTState is published. Its immutable CTTYDEF layout is sufficient
    ** to validate the exact base, tail certificate and allocation size. Never
    ** accept variable/interior cdata through this bootstrap-only fallback. */
    if (cdataisv(cd) ||
	!lj_ctype_predefined_payload_size(cd->ctypeid, &sz) ||
	sz > LJ_MAX_MEM32 - sizeof(GCcdata))
      return 0;
    base = cd;
    size = (GCSize)(sizeof(GCcdata) + sz);
    if (!cdata_size_matches(cd, (size_t)size))
      return 0;
    if (basep) *basep = base;
    if (sizep) *sizep = size;
    return 1;
  }
  if (!cdata_raw_info_safe(cts, cd->ctypeid, &info, &sz))
    return 0;
  if (cdataisv(cd)) {
    GCcdataVar *cv = cdatav(cd);
    MSize offset = cv->offset;
    MSize extra = cv->extra;
    MSize len = cv->len;
    if (extra < sizeof(GCcdataVar) + sizeof(GCcdata) ||
	offset < sizeof(GCcdataVar) ||
	offset > extra - sizeof(GCcdata) ||
	len > LJ_MAX_MEM32 - extra)
      return 0;
    base = (void *)((char *)cd - offset);
    size = (GCSize)(len + extra);
  } else {
    if (ctype_hassize(info)) {
      if (sz > LJ_MAX_MEM32 - sizeof(GCcdata))
	return 0;
      size = (GCSize)(sizeof(GCcdata) + sz);
    } else if (ctype_isfunc(info) || ctype_isextern(info)) {
      size = (GCSize)(sizeof(GCcdata) + CTSIZE_PTR);
    } else {
      return 0;
    }
    base = cd;
  }
  if (!cdata_size_matches(cd, (size_t)size))
    return 0;
  if (basep) *basep = base;
  if (sizep) *sizep = size;
  return 1;
}

/* Free a C data object. */
void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd)
{
  if (LJ_UNLIKELY((lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN) &&
                  lj_cdata_fin_native_pending(g, obj2gco(cd)))) {
    cdata_free_finalizer_invariant(g);
  } else {
    void *base;
    GCSize size;
    if (LJ_UNLIKELY(!lj_cdata_validate(g, cd, &base, &size))) {
      obj2gco(cd)->gch.gct = 0;
      return;
    }
    lj_mem_free(g, base, size);
  }
}

int lj_cdata_fin_isclaim(cTValue *tv)
{
  return lj_cdata_fin_isclaim_inline(tv);
}

#if defined(LJ_CDATA_TEST_HELPERS)
static uint32_t cdata_fin_test_fail_order;
static uint32_t cdata_fin_test_pause_armed[LJ_CDATA_FIN_PAUSE__MAX];
static uint32_t cdata_fin_test_pause_waiting[LJ_CDATA_FIN_PAUSE__MAX];
static uint32_t cdata_fin_test_pause_release[LJ_CDATA_FIN_PAUSE__MAX];

void lj_cdata_test_fail_order_once(void)
{
  la_store32_rel(&cdata_fin_test_fail_order, 1);
}

void lj_cdata_test_fin_pause_arm(uint32_t point)
{
  if (point == 0 || point >= LJ_CDATA_FIN_PAUSE__MAX)
    abort();
  la_store32_rel(&cdata_fin_test_pause_waiting[point], 0);
  la_store32_rel(&cdata_fin_test_pause_release[point], 0);
  la_store32_rel(&cdata_fin_test_pause_armed[point], 1);
}

int lj_cdata_test_fin_pause_waiting(uint32_t point)
{
  return point > 0 && point < LJ_CDATA_FIN_PAUSE__MAX &&
    la_load32_acq(&cdata_fin_test_pause_waiting[point]) != 0;
}

void lj_cdata_test_fin_pause_release(uint32_t point)
{
  if (point == 0 || point >= LJ_CDATA_FIN_PAUSE__MAX)
    abort();
  la_store32_rel(&cdata_fin_test_pause_release[point], 1);
}

void lj_cdata_test_fin_pause_at(uint32_t point)
{
  uint32_t expect = 1;
  if (point == 0 || point >= LJ_CDATA_FIN_PAUSE__MAX ||
      !la_cas32(&cdata_fin_test_pause_armed[point], &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&cdata_fin_test_pause_waiting[point], 1);
  while (la_load32_acq(&cdata_fin_test_pause_release[point]) == 0)
    (void)lj_thr_retry_yield(NULL);
  la_store32_rel(&cdata_fin_test_pause_waiting[point], 0);
}
#else
#define lj_cdata_test_fin_pause_at(point) ((void)0)
#endif

static void cdata_fin_claim_wait(lua_State *L)
{
  /*
  ** FINREG claims are short publication windows. When a Lua state is available,
  ** wait as native time for its TG so safepoint handshakes can complete while
  ** a peer resolves the slot.
  */
  (void)lj_thr_retry_yield(L);
}

static uint64_t cdata_fin_newid(CTState *cts)
{
  uint64_t old = la_load64_acq(&cts->fin_registration_id);
  while (old < FINREG_MAX_ID) {
    uint64_t next = old + 1;
    if (la_cas64(&cts->fin_registration_id, &old, next, LA_RLX, LA_RLX))
      return next;
  }
  return 0;
}

void lj_cdata_setfin(lua_State *L, GCcdata *cd, GCobj *obj, uint32_t it)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  TValue key, val;
  ptrdiff_t anchor;
  int enabled = it != LJ_TNIL;
  if (!cts)
    return;
  lj_state_checkstack(L, enabled ? 2u : 1u);
  anchor = savestack(L, L->top);
  setcdataV(L, &key, cd);
  copyTVrel(L, L->top++, &key);
  lj_gc2_publish_read_root(L, L->top - 1, 1);
  if (enabled) {
    setgcV(L, &val, obj, it);
    copyTVrel(L, L->top++, &val);
    lj_gc2_publish_read_root(L, L->top - 1, 1);
  } else {
    setnilV(&val);
  }
  for (;;) {
    CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
    FinRegOrderNode *ord = NULL;
    la_u128 old, next = { 0, 0 };
    int rc = lj_ctype_fin_get_native(cts, &key, &held);
    if (rc == LJ_CTYPE_FIN_MISS && !enabled) {
      lj_cdata_test_fin_pause_at(LJ_CDATA_FIN_PAUSE_CLEAR_MISS);
      break;
    }
    if (rc == LJ_CTYPE_FIN_MISS) {
      TValue nilv;
      setnilV(&nilv);
      rc = lj_ctype_fin_newgen(L, cts, &key, &nilv, &held);
    }
    if (rc != LJ_CTYPE_FIN_FOUND) {
      lj_ctype_fin_lease_release(&held);
      cdata_fin_claim_wait(L);
      continue;
    }
    old = finreg_load(held.generation);
    if (enabled && finreg_state(old) == FINREG_ACTIVE &&
        tv_rawload(&val) == old.lo) {
      lj_cdata_test_fin_pause_at(LJ_CDATA_FIN_PAUSE_ENABLE_UNCHANGED);
      lj_ctype_fin_lease_release(&held);
      break;
    }
    if (enabled) {
      uint64_t id = cdata_fin_newid(cts);
      if (LJ_UNLIKELY(!id)) {
        lj_ctype_fin_lease_release(&held);
        lj_err_callermsg(L, "too many cdata finalizer registrations");
      }
#if defined(LJ_CDATA_TEST_HELPERS)
      if (!la_xchg32_acqrel(&cdata_fin_test_fail_order, 0))
#endif
        ord = lj_ctype_fin_order_new_nothrow(L);
      if (LJ_UNLIKELY(!ord)) {
        lj_ctype_fin_lease_release(&held);
        lj_err_mem(L);
      }
      ord->registration_id = id;
      next.lo = tv_rawload(&val);
      next.hi = (id << 2) | FINREG_ACTIVE;
      lj_obj_addgcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
      lj_ctype_fin_order_publish(cts, ord, obj2gco(cd), held.tab, NULL);
    }
    lj_cdata_test_fin_pause_at(enabled ? LJ_CDATA_FIN_PAUSE_ENABLE_PREPARE :
                                        LJ_CDATA_FIN_PAUSE_CLEAR_PREPARE);
    rc = finreg_cas(held.generation, &old, next);
    if (ord)
      la_store32_rel(&ord->submission_done, 1);
    if (rc) {
      lj_cdata_test_fin_pause_at(LJ_CDATA_FIN_PAUSE_TRANSITION);
      if (finreg_id(old))
        lj_ctype_fin_order_retire_id(cts, obj2gco(cd), finreg_id(old));
      if (enabled && finreg_state(old) != FINREG_ACTIVE)
        lj_gc2_finreg_cdata_note_transition(g, 1);
      else if (!enabled && finreg_state(old) == FINREG_ACTIVE)
        lj_gc2_finreg_cdata_note_transition(g, 0);
    } else if (ord) {
      lj_ctype_fin_order_retire_id(cts, obj2gco(cd), ord->registration_id);
    }
    lj_ctype_fin_lease_release(&held);
    if (enabled)
      lj_mem_account_deferred(L, sizeof(FinRegOrderNode));
    if (rc) {
      lj_cdata_test_fin_pause_at(enabled ? LJ_CDATA_FIN_PAUSE_ENABLE_ORDER :
                                          LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
      break;
    }
  }
  L->top = restorestack(L, anchor);
}

int lj_cdata_fin_native_pending(global_State *g, GCobj *o)
{
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen;
  int smr, pending = 0;
  if (!cts)
    return 0;
  smr = lj_gc2_smr_read_try(g);
  for (gen = fin_gen_head_acq(cts); gen != NULL; ) {
    FinRegGen *next;
    LJGC2Lease scope = { 0 };
    if (smr ? lj_gc2_mem_lease_acquire(g, gen, &scope) < 0 :
              !lj_gc2_mem_registered_known_reclaim_held(g, gen)) {
      lj_gc2_lease_release(&scope);
      pending = 1;
      break;
    }
    next = fin_gen_next_acq(gen);
    if (gcref(gen->registered_key) == o) {
      pending = finreg_state(finreg_load(gen)) != 0;
      lj_gc2_lease_release(&scope);
      break;
    }
    lj_gc2_lease_release(&scope);
    gen = next;
  }
  if (smr)
    lj_gc2_smr_read_leave(g);
  return pending;
}

/* -- C data indexing ----------------------------------------------------- */

/*
** Cdata objects carry already-published ctype IDs. Container snapshots stay
** shallow so numeric indexing of stable int*-style records does not serialize
** with an unrelated cdef. Child/layout walks that can observe rollback state
** use the ID-rooted sequence-checked helpers below.
*/
static int cdata_ctype_snapshot_shallow(CTState *cts, CTypeID id, CType *out)
{
  CTypeID top;
  CTypeTab *tabh;
  CType *ct;
  CTInfo info;
  GCobj *name;
  if (id == 0)
    return 0;
  top = ctype_top_acq(cts);
  if (id >= top)
    return 0;
  tabh = &cts->tab;
  if ((MSize)id >= ctype_tab_sizetab_acq(tabh))
    return -1;  /* Table/top raced a grow; retry through the wait path. */
  ct = ctype_tab_slot(tabh, id);
  info = ctype_info_acq(ct);
  out->info = info;
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  name = ctype_nameobj_acq(ct);
  setgcrefp(out->name, name);
  return !ctype_isabandoned(info);
}

static int cdata_ctype_snapshot_wait(lua_State *L, CTState *cts,
				     CTypeID id, CType *out)
{
  int ok = cdata_ctype_snapshot_shallow(cts, id, out);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = lj_ctype_snapshot(cts, id, out);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int cdata_ctype_refresh(lua_State *L, CTState *cts, CTypeID id,
			       CType *snap, CTInfo *infop, CTSize *sizep)
{
  if (!cdata_ctype_snapshot_wait(L, cts, id, snap))
    return 0;
  *infop = ctype_info_acq(snap);
  *sizep = ctype_size_acq(snap);
  return 1;
}

/* Index C data by a TValue. Return CType, pointer and resolved container ID. */
CType *lj_cdata_index_l(lua_State *L, CTState *cts, GCcdata *cd,
			cTValue *key, uint8_t **pp, CTInfo *qual,
			CType *snap, CTypeID *idp)
{
  uint8_t *p;
  CTypeID id;
  CType *ct;
  CTInfo info;
  CTSize size;
  ptrdiff_t idx;

  p = (uint8_t *)cdataptr(cd);
  id = cd->ctypeid;
  *qual = 0;
  ct = snap;
  if (!cdata_ctype_refresh(L, cts, id, snap, &info, &size))
    lj_err_caller(L, LJ_ERR_FFI_INVSIZE);

  /* Resolve reference for cdata object. */
  if (ctype_isref(info)) {
    lj_assertCTS(size == CTSIZE_PTR, "ref is not pointer-sized");
    p = *(uint8_t **)p;
    id = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, id, snap, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }

collect_attrib:
  /* Skip attributes and collect qualifiers. */
  while (ctype_isattrib(info)) {
    if (ctype_attrib(info) == CTA_QUAL) *qual |= size;
    id = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, id, snap, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }
  /* Interning rejects refs to refs. */
  lj_assertCTS(!ctype_isref(info), "bad ref of ref");

  if (tvisint(key)) {
    idx = (ptrdiff_t)intV(key);
    goto integer_key;
  } else if (tvisnum(key)) {  /* Numeric key. */
    idx = lj_num2int_type(numV(key), ptrdiff_t);
  integer_key:
    if (ctype_ispointer(info)) {
      CTypeID elemid = ctype_cid(info);
      CTSize sz;
      (void)lj_ctype_size_wait(L, cts, elemid, &sz);
      if (sz == CTSIZE_INVALID) {
	lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
      }
      lj_assertCTS(ctype_ispointer(info) && ctype_cid(info) == elemid,
		   "cdata numeric index type changed across ctype wait");
      if (ctype_isptr(info)) {
	p = (uint8_t *)cdata_getptr(p, size);
      } else if ((info & (CTF_VECTOR|CTF_COMPLEX))) {
	if ((info & CTF_COMPLEX)) idx &= 1;
	*qual |= CTF_CONST;  /* Valarray elements are constant. */
      }
      *pp = p + idx*(int32_t)sz;
      return ct;
    }
  } else if (tviscdata(key)) {  /* Integer cdata key. */
    GCcdata *cdk = cdataV(key);
    CTypeID kid = 0;
    CType ksnap, intsnap;
    CTInfo kinfo = 0;
    CTSize ksize = CTSIZE_INVALID;
    if (lj_ctype_info_wait(L, cts, cdk->ctypeid, &kinfo, &ksize,
			   &kid, &ksnap) <= 0)
      goto cdata_key_done;
    if (ctype_isenum(ctype_info_acq(&ksnap))) {
      kid = ctype_cid(ctype_info_acq(&ksnap));
      if (!cdata_ctype_snapshot_wait(L, cts, kid, &ksnap))
	goto cdata_key_done;
      kinfo = ctype_info_acq(&ksnap);
    }
    if (ctype_isinteger(kinfo)) {
      if (!cdata_ctype_snapshot_wait(L, cts, CTID_INT_PSZ, &intsnap))
	lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
      lj_cconv_ct_ct_l(L, cts, &intsnap, CTID_INT_PSZ, &ksnap, kid,
		       (uint8_t *)&idx, cdataptr(cdk), 0);
      goto integer_key;
    }
  cdata_key_done:
    ;
  } else if (tvisstr(key)) {  /* String key. */
    GCstr *name = strV(key);
    if (ctype_isstruct(info)) {
      CTSize ofs;
      CTInfo q = *qual;
      int ok = lj_ctype_getfieldq_wait(L, cts, id, name, &ofs, &q, snap);
      if (ok) {
	*qual = q;
	*pp = p + ofs;
	return snap;
      }
      if (!cdata_ctype_refresh(L, cts, id, snap, &info, &size))
	lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
    } else if (ctype_iscomplex(info)) {
      if (name->len == 2) {
	*qual |= CTF_CONST;  /* Complex fields are constant. */
	if (strdata(name)[0] == 'r' && strdata(name)[1] == 'e') {
	  *pp = p;
	  return ct;
	} else if (strdata(name)[0] == 'i' && strdata(name)[1] == 'm') {
	  *pp = p + (size >> 1);
	  return ct;
	}
      }
    } else if (cd->ctypeid == CTID_CTYPEID) {
      /* Allow indexing a (pointer to) struct constructor to get constants. */
      CTypeID sid = 0;
      CType ssnap;
      CTInfo sinfo = 0;
      CTSize ssize = CTSIZE_INVALID;
      if (lj_ctype_info_wait(L, cts, *(CTypeID *)p, &sinfo, &ssize,
			     &sid, &ssnap) <= 0)
	goto ctypeid_done;
      if (ctype_isptr(sinfo)) {
	CTInfo rawinfo = ctype_info_acq(&ssnap);
	if (lj_ctype_info_wait(L, cts, ctype_cid(rawinfo), &sinfo, &ssize,
			       &sid, &ssnap) <= 0)
	  goto ctypeid_done;
      }
      if (ctype_isstruct(sinfo)) {
	CTSize ofs;
	int ok = lj_ctype_getfieldq_wait(L, cts, sid, name, &ofs, NULL, snap);
	if (ok && ctype_isconstval(ctype_info_acq(snap)))
	  return snap;
	if (lj_ctype_info_wait(L, cts, sid, &sinfo, &ssize, &sid, &ssnap) <= 0)
	  goto ctypeid_done;
      }
      *snap = ssnap;  /* Resolve metamethods for constructors. */
      id = sid;
      info = sinfo;
      size = ssize;
ctypeid_done:
      ;
    }
  }
  if (ctype_isptr(info)) {  /* Automatically perform '->'. */
    CTypeID cid, elemid = ctype_cid(info);
    CType ssnap;
    CTInfo sinfo;
    CTSize ssize;
    int ok = lj_ctype_info_wait(L, cts, elemid, &sinfo, &ssize, &cid,
				&ssnap);
    if (!cdata_ctype_refresh(L, cts, id, snap, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
    if (ok > 0 && ctype_isstruct(sinfo)) {
      lj_assertCTS(ctype_isptr(info) && ctype_cid(info) == elemid,
		   "cdata auto-deref type changed");
      p = (uint8_t *)cdata_getptr(p, size);
      *qual |= ((info|sinfo) & CTF_QUAL);
      id = cid;
      *snap = ssnap;
      info = sinfo;
      size = ssize;
      goto collect_attrib;
    }
  }
  if (idp) *idp = id;
  *qual |= 1;  /* Lookup failed. */
  return ct;  /* But return the resolved raw type. */
}

/* -- C data getters ------------------------------------------------------ */

/* Get constant value and convert to TValue. */
static void cdata_getconst(lua_State *L, CTState *cts, TValue *o, CType *ct)
{
  CType cttsnap, *ctt = &cttsnap;
  CTInfo info = ctype_info_acq(ct);
  CTInfo tinfo;
  CTSize size = ctype_size_acq(ct);
  if (!cdata_ctype_snapshot_wait(L, cts, ctype_cid(info), ctt))
    lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  tinfo = ctype_info_acq(ctt);
  lj_assertCTS(ctype_isinteger(tinfo) && ctype_size_acq(ctt) <= 4,
	       "only 32 bit const supported");  /* NYI */
  /* Constants are already zero-extended/sign-extended to 32 bits. */
  if ((tinfo & CTF_UNSIGNED) && (int32_t)size < 0)
    setnumV(o, (lua_Number)(uint32_t)size);
  else
    setintV(o, (int32_t)size);
}

/* Get C data value and convert to TValue. */
int lj_cdata_get_l(lua_State *L, CTState *cts, CType *s, TValue *o,
		   uint8_t *sp)
{
  CTypeID sid;
  CTInfo info = ctype_info_acq(s);
  CTSize size;

  if (ctype_isconstval(info)) {
    cdata_getconst(L, cts, o, s);
    return 0;  /* No GC step needed. */
  } else if (ctype_isbitfield(info)) {
    return lj_cconv_tv_bf_l(L, cts, s, o, sp);
  }

  /* Get child type of pointer/array/field. */
  lj_assertCTS(ctype_ispointer(info) || ctype_isfield(info),
	       "pointer or field expected");
  sid = ctype_cid(info);
  if (!cdata_ctype_refresh(L, cts, sid, s, &info, &size))
    lj_err_caller(L, LJ_ERR_FFI_INVSIZE);

  /* Resolve reference for field. */
  if (ctype_isref(info)) {
    lj_assertCTS(size == CTSIZE_PTR, "ref is not pointer-sized");
    sp = *(uint8_t **)sp;
    sid = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, sid, s, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }

  /* Skip attributes. */
  while (ctype_isattrib(info)) {
    sid = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, sid, s, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }

  return lj_cconv_tv_ct_l(L, cts, s, sid, o, sp);
}

/* -- C data setters ------------------------------------------------------ */

/* Convert TValue and set C data value. */
void lj_cdata_set_l(lua_State *L, CTState *cts, CType *d, CTypeID did,
		    uint8_t *dp, TValue *o, CTInfo qual)
{
  CTInfo info = ctype_info_acq(d);
  CTSize size;
  if (ctype_isconstval(info)) {
    goto err_const;
  } else if (ctype_isbitfield(info)) {
    if (((info|qual) & CTF_CONST)) goto err_const;
    lj_cconv_bf_tv_l(L, cts, d, dp, o);
    return;
  }

  /* Get child type of pointer/array/field. */
  lj_assertCTS(ctype_ispointer(info) || ctype_isfield(info),
	       "pointer or field expected");
  did = ctype_cid(info);
  if (!cdata_ctype_refresh(L, cts, did, d, &info, &size))
    lj_err_caller(L, LJ_ERR_FFI_INVSIZE);

  /* Resolve reference for field. */
  if (ctype_isref(info)) {
    lj_assertCTS(size == CTSIZE_PTR, "ref is not pointer-sized");
    dp = *(uint8_t **)dp;
    did = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, did, d, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }

  /* Skip attributes and collect qualifiers. */
  for (;;) {
    if (ctype_isattrib(info)) {
      if (ctype_attrib(info) == CTA_QUAL) qual |= size;
    } else {
      break;
    }
    did = ctype_cid(info);
    if (!cdata_ctype_refresh(L, cts, did, d, &info, &size))
      lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
  }

  lj_assertCTS(ctype_hassize(info), "store to ctype without size");
  lj_assertCTS(!ctype_isvoid(info), "store to void type");

  if (((info|qual) & CTF_CONST)) {
  err_const:
    lj_err_caller(L, LJ_ERR_FFI_WRCONST);
  }

  lj_cconv_ct_tv_l(L, cts, d, did, dp, o, 0);
}

#endif
