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

/* Free a C data object. */
void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd)
{
  if (LJ_UNLIKELY(lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN)) {
    GCobj *root;
    makewhite(g, obj2gco(cd));
    markfinalized(obj2gco(cd));
    lj_gc_arena_markobj(g, obj2gco(cd));
    lj_gc2_finreg_cdata_queue(g, obj2gco(cd));
    if ((root = gcref(g->gc.mmudata)) != NULL) {
      lj_obj_setgcwr(obj2gco(cd), *lj_obj_gcwref(root));
      setgcref(*lj_obj_gcwref(root), obj2gco(cd));
      setgcref(g->gc.mmudata, obj2gco(cd));
    } else {
      lj_obj_setgcw(obj2gco(cd), obj2gco(cd));
      setgcref(g->gc.mmudata, obj2gco(cd));
    }
  } else if (LJ_LIKELY(!cdataisv(cd))) {
    CType *ct = ctype_raw(ctype_ctsG(g), cd->ctypeid);
    CTSize sz = ctype_hassize(ct->info) ? ct->size : CTSIZE_PTR;
    lj_assertG(ctype_hassize(ct->info) || ctype_isfunc(ct->info) ||
	       ctype_isextern(ct->info), "free of ctype without a size");
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

static int cdata_fin_claim(TValue *tv, TValue *old, int nonnil)
{
  TValue claim;
  cdata_fin_setclaim(&claim);
  for (;;) {
    lj_tv_load_acq(old, tv);
    if (lj_cdata_fin_isclaim(old)) {
      la_cpu_pause();
      continue;
    }
    if (nonnil && tvisnil(old))
      return 0;
    if (lj_tv_cas(tv, old, &claim))
      return 1;  /* 11.4 FINREG slot claim. */
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

static void cdata_fin_store(lua_State *L, global_State *g, GCtab *t,
			    GCcdata *cd, TValue *tv, TValue *val,
			    int enabled)
{
  if (enabled) {
    copyTVrel(L, tv, val);
    lj_obj_addgcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 1);
  } else {
    lj_cdata_fin_storenil(L, tv);
    lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
  }
  lj_gc_pubtab(L, t);
}

void lj_cdata_setfin(lua_State *L, GCcdata *cd, GCobj *obj, uint32_t it)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  GCtab *t = gco2tab(gcref_acq(g->gcroot[GCROOT_FFI_FIN]));
  TValue *tv, key, val, old;
  int enabled = (it != LJ_TNIL);
  setcdataV(L, &key, cd);
  if (enabled)
    setgcV(L, &val, obj, it);
  for (;;) {
    if (!gcref_acq(t->metatable))
      return;
    tv = (TValue *)lj_tab_get(L, t, &key);
    if (tv == niltv(L)) {
      if (!enabled) {  /* Missing clear is a no-op; avoid fin_token insert. */
	lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	return;
      }
      if (lj_ctype_fin_anchor_begin(cts)) {
	cdata_fin_setclaim(&old);
	switch (lj_tab_try_newkey_anchor(L, t, &key, &old, &tv)) {
	case 1:
	  if (!gcref_acq(t->metatable)) {
	    lj_cdata_fin_storenil(L, tv);
	    lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
	    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
	    lj_ctype_fin_anchor_end(cts);
	    return;
	  }
	  cdata_fin_store(L, g, t, cd, tv, &val, enabled);
	  lj_ctype_fin_anchor_end(cts);
	  return;
	case -1:
	  lj_ctype_fin_anchor_end(cts);
	  continue;  /* Racing insert published the key; claim existing slot. */
	default:
	  lj_ctype_fin_anchor_end(cts);
	  break;  /* Collision/resize: structural insertion still uses fin_token. */
	}
      }
      break;
    }
    (void)lj_cdata_fin_claim_any(tv, &old);
    if (!gcref_acq(t->metatable)) {
      lj_cdata_fin_storenil(L, tv);
      lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
      lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
      return;
    }
    cdata_fin_store(L, g, t, cd, tv, &val, enabled);
    return;
  }
  lj_ctype_fin_lock(cts);
  if (gcref_acq(t->metatable)) {
    /* Add cdata to finalizer table, if still enabled. */
    tv = lj_tab_set(L, t, &key);
    (void)lj_cdata_fin_claim_any(tv, &old);
    cdata_fin_store(L, g, t, cd, tv, &val, enabled);
  }
  lj_ctype_fin_unlock(cts);
}

/* -- C data indexing ----------------------------------------------------- */

/* Index C data by a TValue. Return CType, pointer and resolved container ID. */
CType *lj_cdata_index_l(lua_State *L, CTState *cts, GCcdata *cd,
			cTValue *key, uint8_t **pp, CTInfo *qual,
			CTypeID *idp)
{
  uint8_t *p = (uint8_t *)cdataptr(cd);
  CTypeID id = cd->ctypeid;
  CType *ct = ctype_get(cts, id);
  ptrdiff_t idx;

  /* Resolve reference for cdata object. */
  if (ctype_isref(ct->info)) {
    lj_assertCTS(ct->size == CTSIZE_PTR, "ref is not pointer-sized");
    p = *(uint8_t **)p;
    id = ctype_cid(ct->info);
    ct = ctype_get(cts, id);
  }

collect_attrib:
  /* Skip attributes and collect qualifiers. */
  while (ctype_isattrib(ct->info)) {
    if (ctype_attrib(ct->info) == CTA_QUAL) *qual |= ct->size;
    id = ctype_cid(ct->info);
    ct = ctype_get(cts, id);
  }
  /* Interning rejects refs to refs. */
  lj_assertCTS(!ctype_isref(ct->info), "bad ref of ref");

  if (tvisint(key)) {
    idx = (ptrdiff_t)intV(key);
    goto integer_key;
  } else if (tvisnum(key)) {  /* Numeric key. */
    idx = lj_num2int_type(numV(key), ptrdiff_t);
  integer_key:
    if (ctype_ispointer(ct->info)) {
      CTSize sz = lj_ctype_size(cts, ctype_cid(ct->info));  /* Element size. */
      if (sz == CTSIZE_INVALID)
	lj_err_caller(L, LJ_ERR_FFI_INVSIZE);
      if (ctype_isptr(ct->info)) {
	p = (uint8_t *)cdata_getptr(p, ct->size);
      } else if ((ct->info & (CTF_VECTOR|CTF_COMPLEX))) {
	if ((ct->info & CTF_COMPLEX)) idx &= 1;
	*qual |= CTF_CONST;  /* Valarray elements are constant. */
      }
      *pp = p + idx*(int32_t)sz;
      return ct;
    }
  } else if (tviscdata(key)) {  /* Integer cdata key. */
    GCcdata *cdk = cdataV(key);
    CTypeID kid = ctype_rawid(cts, cdk->ctypeid);
    CType *ctk = ctype_get(cts, kid);
    if (ctype_isenum(ctk->info)) {
      kid = ctype_cid(ctk->info);
      ctk = ctype_get(cts, kid);
    }
    if (ctype_isinteger(ctk->info)) {
      lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT_PSZ), CTID_INT_PSZ,
		       ctk, kid,
		       (uint8_t *)&idx, cdataptr(cdk), 0);
      goto integer_key;
    }
  } else if (tvisstr(key)) {  /* String key. */
    GCstr *name = strV(key);
    if (ctype_isstruct(ct->info)) {
      CTSize ofs;
      CType *fct = lj_ctype_getfieldq(cts, ct, name, &ofs, qual);
      if (fct) {
	*pp = p + ofs;
	return fct;
      }
    } else if (ctype_iscomplex(ct->info)) {
      if (name->len == 2) {
	*qual |= CTF_CONST;  /* Complex fields are constant. */
	if (strdata(name)[0] == 'r' && strdata(name)[1] == 'e') {
	  *pp = p;
	  return ct;
	} else if (strdata(name)[0] == 'i' && strdata(name)[1] == 'm') {
	  *pp = p + (ct->size >> 1);
	  return ct;
	}
      }
    } else if (cd->ctypeid == CTID_CTYPEID) {
      /* Allow indexing a (pointer to) struct constructor to get constants. */
      CTypeID sid = ctype_rawid(cts, *(CTypeID *)p);
      CType *sct = ctype_get(cts, sid);
      if (ctype_isptr(sct->info)) {
	sid = ctype_rawid(cts, ctype_cid(sct->info));
	sct = ctype_get(cts, sid);
      }
      if (ctype_isstruct(sct->info)) {
	CTSize ofs;
	CType *fct = lj_ctype_getfield(cts, sct, name, &ofs);
	if (fct && ctype_isconstval(fct->info))
	  return fct;
      }
      ct = sct;  /* Allow resolving metamethods for constructors, too. */
      id = sid;
    }
  }
  if (ctype_isptr(ct->info)) {  /* Automatically perform '->'. */
    CTypeID cid = ctype_rawid(cts, ctype_cid(ct->info));
    if (ctype_isstruct(ctype_get(cts, cid)->info)) {
      p = (uint8_t *)cdata_getptr(p, ct->size);
      id = ctype_cid(ct->info);
      ct = ctype_get(cts, id);
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
  lj_assertCTS(ctype_isinteger(ctt->info) && ctt->size <= 4,
	       "only 32 bit const supported");  /* NYI */
  /* Constants are already zero-extended/sign-extended to 32 bits. */
  if ((ctt->info & CTF_UNSIGNED) && (int32_t)ct->size < 0)
    setnumV(o, (lua_Number)(uint32_t)ct->size);
  else
    setintV(o, (int32_t)ct->size);
}

/* Get C data value and convert to TValue. */
int lj_cdata_get_l(lua_State *L, CTState *cts, CType *s, TValue *o,
		   uint8_t *sp)
{
  CTypeID sid;

  if (ctype_isconstval(s->info)) {
    cdata_getconst(cts, o, s);
    return 0;  /* No GC step needed. */
  } else if (ctype_isbitfield(s->info)) {
    return lj_cconv_tv_bf_l(L, cts, s, o, sp);
  }

  /* Get child type of pointer/array/field. */
  lj_assertCTS(ctype_ispointer(s->info) || ctype_isfield(s->info),
	       "pointer or field expected");
  sid = ctype_cid(s->info);
  s = ctype_get(cts, sid);

  /* Resolve reference for field. */
  if (ctype_isref(s->info)) {
    lj_assertCTS(s->size == CTSIZE_PTR, "ref is not pointer-sized");
    sp = *(uint8_t **)sp;
    sid = ctype_cid(s->info);
    s = ctype_get(cts, sid);
  }

  /* Skip attributes. */
  while (ctype_isattrib(s->info))
    s = ctype_child(cts, s);

  return lj_cconv_tv_ct_l(L, cts, s, sid, o, sp);
}

/* -- C data setters ------------------------------------------------------ */

/* Convert TValue and set C data value. */
void lj_cdata_set_l(lua_State *L, CTState *cts, CType *d, CTypeID did,
		    uint8_t *dp, TValue *o, CTInfo qual)
{
  if (ctype_isconstval(d->info)) {
    goto err_const;
  } else if (ctype_isbitfield(d->info)) {
    if (((d->info|qual) & CTF_CONST)) goto err_const;
    lj_cconv_bf_tv_l(L, cts, d, dp, o);
    return;
  }

  /* Get child type of pointer/array/field. */
  lj_assertCTS(ctype_ispointer(d->info) || ctype_isfield(d->info),
	       "pointer or field expected");
  did = ctype_cid(d->info);
  d = ctype_get(cts, did);

  /* Resolve reference for field. */
  if (ctype_isref(d->info)) {
    lj_assertCTS(d->size == CTSIZE_PTR, "ref is not pointer-sized");
    dp = *(uint8_t **)dp;
    did = ctype_cid(d->info);
    d = ctype_get(cts, did);
  }

  /* Skip attributes and collect qualifiers. */
  for (;;) {
    if (ctype_isattrib(d->info)) {
      if (ctype_attrib(d->info) == CTA_QUAL) qual |= d->size;
    } else {
      break;
    }
    did = ctype_cid(d->info);
    d = ctype_get(cts, did);
  }

  lj_assertCTS(ctype_hassize(d->info), "store to ctype without size");
  lj_assertCTS(!ctype_isvoid(d->info), "store to void type");

  if (((d->info|qual) & CTF_CONST)) {
  err_const:
    lj_err_caller(L, LJ_ERR_FFI_WRCONST);
  }

  lj_cconv_ct_tv_l(L, cts, d, did, dp, o, 0);
}

#endif
