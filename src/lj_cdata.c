/*
** C data management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_thr.h"

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
  char *p = lj_mem_newt(L, extra + sz, char);
  uintptr_t adata = (uintptr_t)p + sizeof(GCcdataVar) + sizeof(GCcdata);
  uintptr_t almask = (1u << align) - 1u;
  GCcdata *cd = (GCcdata *)(((adata + almask) & ~almask) - sizeof(GCcdata));
  lj_assertL((char *)cd - p < 65536, "excessive cdata alignment");
  cdatav(cd)->offset = (uint16_t)((char *)cd - p);
  cdatav(cd)->extra = extra;
  cdatav(cd)->len = sz;
  g = G(L);
  cd->gct = ~LJ_TCDATA;
  cd->ctypeid = id;
  cdata_flags_rel(cd, 0);
  newwhite(g, obj2gco(cd));
  lj_obj_addgcflags(obj2gco(cd), 0x80);
  lj_gc_linkobj(g, obj2gco(cd));
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

/* Free a C data object. */
void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd)
{
  if (LJ_UNLIKELY(lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN)) {
    cdata_free_finalizer_invariant(g);
  } else if (LJ_LIKELY(!cdataisv(cd))) {
    CType *ct = ctype_raw(ctype_ctsG(g), cd->ctypeid);
    CTInfo info = ctype_info_acq(ct);
    CTSize sz = ctype_hassize(info) ? ctype_size_acq(ct) : CTSIZE_PTR;
    lj_assertG(ctype_hassize(info) || ctype_isfunc(info) ||
	       ctype_isextern(info), "free of ctype without a size");
    lj_mem_free(g, cd, sizeof(GCcdata) + sz);
  } else {
    lj_mem_free(g, memcdatav(cd), sizecdatav(cd));
  }
}

#define LJ_CDATA_FINCLAIM_U64 \
  ((((uint64_t)LJ_TLIGHTUD) << 47) | (((uint64_t)1 << 47) - 1u))

static void cdata_fin_setclaim(TValue *tv)
{
  tv_rawstore(tv, LJ_CDATA_FINCLAIM_U64);
}

int lj_cdata_fin_isclaim(cTValue *tv)
{
  return tv_rawload(tv) == LJ_CDATA_FINCLAIM_U64;
}

static void cdata_fin_claim_wait_no_l(void)
{
  (void)lj_thr_sleep_ns(NULL, 1000000);
}

static int cdata_fin_claim(TValue *tv, TValue *old, int nonnil)
{
  TValue claim;
  cdata_fin_setclaim(&claim);
  for (;;) {
    lj_tv_load_acq(old, tv);
    if (lj_cdata_fin_isclaim(old)) {
      cdata_fin_claim_wait_no_l();
      continue;
    }
    if (nonnil && tvisnil(old))
      return 0;
    if (lj_tv_cas(tv, old, &claim))
      return 1;  /* 11.4 FINREG slot claim. */
    cdata_fin_claim_wait_no_l();  /* CAS loser: yield before retrying. */
  }
}

int lj_cdata_fin_claim_any(TValue *tv, TValue *old)
{
  return cdata_fin_claim(tv, old, 0);
}

int lj_cdata_fin_claim_func(TValue *tv, TValue *old)
{
  return cdata_fin_claim(tv, old, 1);
}

void lj_cdata_fin_storenil(lua_State *L, TValue *tv)
{
  TValue nilv;
  setnilV(&nilv);
  copyTVrel(L, tv, &nilv);
}

static void cdata_fin_store(lua_State *L, global_State *g, CTState *cts,
			    GCtab *t, GCcdata *cd, TValue *tv, TValue *val,
			    int enabled, FinRegOrderNode **ordp)
{
  if (enabled) {
    TValue key;
    setcdataV(L, &key, cd);
    if (ordp && *ordp) {
      /*
      ** Publish the ordered node while the slot still contains the claim
      ** sentinel. Ordered FINREG scans wait for claim resolution before the
      ** cdata can become a visible finalizer candidate.
      */
      lj_ctype_fin_order_publish(cts, *ordp, obj2gco(cd), t, tv);
      *ordp = NULL;
    }
    lj_gc2_barrier_weak_key(L, t, &key);
    lj_obj_addgcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 1);
    copyTVrel(L, tv, val);
  } else {
    lj_cdata_fin_storenil(L, tv);
    lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
  }
  lj_gc_pubtab(L, t);  /* 11.4 FINREG publish after claim resolution. */
}

void lj_cdata_setfin(lua_State *L, GCcdata *cd, GCobj *obj, uint32_t it)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  TValue *anchor = L->top;
  GCtab *t;
  TValue *tv, key, val, old;
  FinRegOrderNode *ord = NULL;
  int enabled = (it != LJ_TNIL);
  if (LJ_UNLIKELY(cts == NULL))
    return;
  setcdataV(L, &key, cd);
  if (enabled) {
    setgcV(L, &val, obj, it);
    if (LJ_LIKELY(anchor + 2 < tvref(L->maxstack))) {
      copyTVrel(L, anchor, &key);
      copyTVrel(L, anchor + 1, &val);
      L->top = anchor + 2;
    }
    lj_gc_pubroot(L, &val);
    ord = lj_ctype_fin_order_new(L);
  } else if (LJ_LIKELY(anchor + 1 < tvref(L->maxstack))) {
    copyTVrel(L, anchor, &key);
    L->top = anchor + 1;
  }
  lj_gc_pubroot(L, &key);
  for (;;) {
    tv = (TValue *)lj_ctype_fin_get(L, cts, &key, &t);
    if (tv == niltv(L)) {
      if (!enabled) {  /* Missing clear is a no-op; avoid structural insert. */
	lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	goto done;
      }
      t = lj_ctype_fin_head(cts);
      if (!t || !gcref_acq(t->metatable))
	goto done;
      cdata_fin_setclaim(&old);
      switch (lj_tab_try_newkey_anchor(L, t, &key, &old, &tv)) {
      case 1:
	if (!gcref_acq(t->metatable)) {
	  lj_cdata_fin_storenil(L, tv);
	  lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	  lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	  goto done;
	}
	cdata_fin_store(L, g, cts, t, cd, tv, &val, enabled, &ord);
	goto done;
      case -1:
	continue;  /* Racing insert published the key; claim existing slot. */
      default:
	break;
      }
      switch (lj_tab_try_newkey_chain(L, t, &key, &old, &tv)) {
      case 1:
	if (!gcref_acq(t->metatable)) {
	  lj_cdata_fin_storenil(L, tv);
	  lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	  lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	  goto done;
	}
	cdata_fin_store(L, g, cts, t, cd, tv, &val, enabled, &ord);
	goto done;
      case -1:
	continue;  /* Racing collision insert published the key. */
      default:
	break;
      }
      switch (lj_ctype_fin_newgen(L, cts, &key, &old, &t, &tv)) {
      case 1:
	if (!gcref_acq(t->metatable)) {
	  lj_cdata_fin_storenil(L, tv);
	  lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	  lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	  goto done;
	}
	cdata_fin_store(L, g, cts, t, cd, tv, &val, enabled, &ord);
	goto done;
      case -1:
	continue;  /* Racing generation already has this cdata key. */
      default:
	goto done;
      }
    }
    (void)lj_cdata_fin_claim_any(tv, &old);
    if (!gcref_acq(t->metatable)) {
      lj_cdata_fin_storenil(L, tv);
      lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
      lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
      goto done;
    }
    cdata_fin_store(L, g, cts, t, cd, tv, &val, enabled, &ord);
    goto done;
  }
done:
  if (ord)
    lj_ctype_fin_order_free(g, ord);
  L->top = anchor;
}

/* -- C data indexing ----------------------------------------------------- */

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
  ct = ctype_get(cts, id);
  info = ctype_info_acq(ct);
  size = ctype_size_acq(ct);

  /* Resolve reference for cdata object. */
  if (ctype_isref(info)) {
    lj_assertCTS(size == CTSIZE_PTR, "ref is not pointer-sized");
    p = *(uint8_t **)p;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
  }

collect_attrib:
  /* Skip attributes and collect qualifiers. */
  while (ctype_isattrib(info)) {
    if (ctype_attrib(info) == CTA_QUAL) *qual |= size;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
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
      ct = ctype_get(cts, id);
      info = ctype_info_acq(ct);
      size = ctype_size_acq(ct);
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
    CTypeID kid = ctype_rawid(cts, cdk->ctypeid);
    CType *ctk = ctype_get(cts, kid);
    CTInfo kinfo = ctype_info_acq(ctk);
    if (ctype_isenum(kinfo)) {
      kid = ctype_cid(kinfo);
      ctk = ctype_get(cts, kid);
      kinfo = ctype_info_acq(ctk);
    }
    if (ctype_isinteger(kinfo)) {
      lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT_PSZ), CTID_INT_PSZ,
		       ctk, kid,
		       (uint8_t *)&idx, cdataptr(cdk), 0);
      goto integer_key;
    }
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
      ct = ctype_get(cts, id);
      info = ctype_info_acq(ct);
      size = ctype_size_acq(ct);
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
      ct = ctype_get(cts, sid);  /* Resolve metamethods for constructors. */
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
    ct = ctype_get(cts, id);
    info = ctype_info_acq(ct);
    size = ctype_size_acq(ct);
    if (ok > 0 && ctype_isstruct(sinfo)) {
      lj_assertCTS(ctype_isptr(info) && ctype_cid(info) == elemid,
		   "cdata auto-deref type changed");
      p = (uint8_t *)cdata_getptr(p, size);
      *qual |= ((info|sinfo) & CTF_QUAL);
      id = cid;
      ct = ctype_get(cts, id);
      info = ctype_info_acq(ct);
      size = ctype_size_acq(ct);
      goto collect_attrib;
    }
  }
  if (idp) *idp = id;
  *qual |= 1;  /* Lookup failed. */
  return ct;  /* But return the resolved raw type. */
}

/* -- C data getters ------------------------------------------------------ */

/* Get constant value and convert to TValue. */
static void cdata_getconst(CTState *cts, TValue *o, CType *ct)
{
  CType *ctt = ctype_child(cts, ct);
  CTInfo tinfo = ctype_info_acq(ctt);
  CTSize size = ctype_size_acq(ct);
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

  if (ctype_isconstval(info)) {
    cdata_getconst(cts, o, s);
    return 0;  /* No GC step needed. */
  } else if (ctype_isbitfield(info)) {
    return lj_cconv_tv_bf_l(L, cts, s, o, sp);
  }

  /* Get child type of pointer/array/field. */
  lj_assertCTS(ctype_ispointer(info) || ctype_isfield(info),
	       "pointer or field expected");
  sid = ctype_cid(info);
  s = ctype_get(cts, sid);
  info = ctype_info_acq(s);

  /* Resolve reference for field. */
  if (ctype_isref(info)) {
    lj_assertCTS(ctype_size_acq(s) == CTSIZE_PTR, "ref is not pointer-sized");
    sp = *(uint8_t **)sp;
    sid = ctype_cid(info);
    s = ctype_get(cts, sid);
    info = ctype_info_acq(s);
  }

  /* Skip attributes. */
  while (ctype_isattrib(info)) {
    s = ctype_child(cts, s);
    info = ctype_info_acq(s);
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
  d = ctype_get(cts, did);
  info = ctype_info_acq(d);
  size = ctype_size_acq(d);

  /* Resolve reference for field. */
  if (ctype_isref(info)) {
    lj_assertCTS(size == CTSIZE_PTR, "ref is not pointer-sized");
    dp = *(uint8_t **)dp;
    did = ctype_cid(info);
    d = ctype_get(cts, did);
    info = ctype_info_acq(d);
    size = ctype_size_acq(d);
  }

  /* Skip attributes and collect qualifiers. */
  for (;;) {
    if (ctype_isattrib(info)) {
      if (ctype_attrib(info) == CTA_QUAL) qual |= size;
    } else {
      break;
    }
    did = ctype_cid(info);
    d = ctype_get(cts, did);
    info = ctype_info_acq(d);
    size = ctype_size_acq(d);
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
