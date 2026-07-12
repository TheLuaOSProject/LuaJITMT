/*
** C data management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CDATA_H
#define _LJ_CDATA_H

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_ctype.h"
#include "lj_err.h"
#include "lj_oserr.h"

#if LJ_HASFFI

/* Get C data pointer. */
static LJ_AINLINE void *cdata_getptr(void *p, CTSize sz)
{
  if (LJ_64 && sz == 4) {  /* Support 32 bit pointers on 64 bit targets. */
    return ((void *)(uintptr_t)*(uint32_t *)p);
  } else {
    lj_assertX(sz == CTSIZE_PTR, "bad pointer size %d", sz);
    return *(void **)p;
  }
}

/* Set C data pointer. */
static LJ_AINLINE void cdata_setptr(void *p, CTSize sz, const void *v)
{
  if (LJ_64 && sz == 4) {  /* Support 32 bit pointers on 64 bit targets. */
    *(uint32_t *)p = (uint32_t)(uintptr_t)v;
  } else {
    lj_assertX(sz == CTSIZE_PTR, "bad pointer size %d", sz);
    *(void **)p = (void *)v;
  }
}

static LJ_AINLINE GCcdata *lj_cdata_new_l(lua_State *L, CTState *cts,
					  CTypeID id, CTSize sz)
{
  GCcdata *cd;
  CTypeID checked;
  global_State *g = G(L);
  LJOSerrState oserr;
#ifdef LUA_USE_ASSERT
  CType *ct = ctype_raw(cts, id);
  CTInfo info = ctype_info_acq(ct);
  CTSize size = ctype_size_acq(ct);
  lj_assertCTS((ctype_hassize(info) ? size : CTSIZE_PTR) == sz,
	       "inconsistent size of fixed-size cdata alloc");
#endif
  checked = ctype_check(cts, id);
  /* Cdata materialization is transparent to the OS error pair even when the
  ** allocator fails. Use the non-throwing primitive so the stack-local pair
  ** is restored at the actual throw edge instead of relying on lexical
  ** cleanup after an external unwind. */
  lj_oserr_save(&oserr);
  cd = (GCcdata *)lj_mem_newgco_unlinked_nothrow(L,
						  sizeof(GCcdata) + sz);
  if (LJ_UNLIKELY(cd == NULL)) {
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  cd->gct = ~LJ_TCDATA;
  cd->ctypeid = checked;
  cdata_flags_rel(cd, cdata_size_tail_flags(sizeof(GCcdata) + sz));
  newwhite(g, obj2gco(cd));
  if (LJ_UNLIKELY(!lj_mem_publish_cdata(
	L, cd, (GCSize)(sizeof(GCcdata) + sz), 0))) {
    lj_mem_freegco_unpublished(g, cd, sizeof(GCcdata) + sz);
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  lj_gc_linkobj_new_at(g, obj2gco(cd), cd);
  lj_oserr_restore(&oserr);
  return cd;
}

/* Variant which works without a valid CTState. */
static LJ_AINLINE GCcdata *lj_cdata_new_(lua_State *L, CTypeID id, CTSize sz)
{
  global_State *g = G(L);
  GCcdata *cd;
  LJOSerrState oserr;
  lj_oserr_save(&oserr);
  cd = (GCcdata *)lj_mem_newgco_unlinked_nothrow(L,
						  sizeof(GCcdata) + sz);
  if (LJ_UNLIKELY(cd == NULL)) {
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  cd->gct = ~LJ_TCDATA;
  cd->ctypeid = id;
  cdata_flags_rel(cd, cdata_size_tail_flags(sizeof(GCcdata) + sz));
  newwhite(g, obj2gco(cd));
  if (LJ_UNLIKELY(!lj_mem_publish_cdata(
	L, cd, (GCSize)(sizeof(GCcdata) + sz), 0))) {
    lj_mem_freegco_unpublished(g, cd, sizeof(GCcdata) + sz);
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  lj_gc_linkobj_new_at(g, obj2gco(cd), cd);
  lj_oserr_restore(&oserr);
  return cd;
}

LJ_FUNC GCcdata *lj_cdata_new_forjit(lua_State *L, CTypeID id, CTSize sz);
LJ_FUNC GCcdata *lj_cdata_newref_l(lua_State *L, CTState *cts,
				   const void *pp, CTypeID id);
LJ_FUNC GCcdata *lj_cdata_newv(lua_State *L, CTypeID id, CTSize sz,
			       CTSize align);
LJ_FUNC GCcdata *lj_cdata_newx_l(lua_State *L, CTState *cts, CTypeID id,
				 CTSize sz, CTInfo info);

LJ_FUNC int lj_cdata_validate(global_State *g, GCcdata *cd, void **basep,
			      GCSize *sizep);
LJ_FUNC void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd);
LJ_FUNC void lj_cdata_setfin(lua_State *L, GCcdata *cd, GCobj *obj,
			     uint32_t it);
/* One-shot keyed claims for an exact CTypeFinLease. RETRY means the caller must
** release/re-resolve before touching the slot again; FORWARD is never claimed. */
LJ_FUNC int lj_cdata_fin_claim_held(CTypeFinLease *lease, cTValue *key,
				    TValue *old, int nonnil);
LJ_FUNC int lj_cdata_fin_store_claim_held(CTypeFinLease *lease,
					  cTValue *key, cTValue *src);
LJ_FUNC int lj_cdata_fin_isclaim(cTValue *tv);

#if defined(LJ_CDATA_TEST_HELPERS)
enum {
  LJ_CDATA_FIN_PAUSE_ENABLE_ORDER = 1,
  LJ_CDATA_FIN_PAUSE_CLEAR_MISS,
  LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL,
  LJ_CDATA_FIN_PAUSE_ENABLE_RETRY,
  LJ_CDATA_FIN_PAUSE_CLEAR_RETRY,
  LJ_CDATA_FIN_PAUSE__MAX
};
LJ_FUNC void lj_cdata_test_fin_pause_arm(uint32_t point);
LJ_FUNC int lj_cdata_test_fin_pause_waiting(uint32_t point);
LJ_FUNC void lj_cdata_test_fin_pause_release(uint32_t point);
#endif

LJ_FUNC CType *lj_cdata_index_l(lua_State *L, CTState *cts, GCcdata *cd,
				cTValue *key, uint8_t **pp, CTInfo *qual,
				CType *snap, CTypeID *idp);
LJ_FUNC int lj_cdata_get_l(lua_State *L, CTState *cts, CType *s,
			   TValue *o, uint8_t *sp);
LJ_FUNC void lj_cdata_set_l(lua_State *L, CTState *cts, CType *d, CTypeID did,
			    uint8_t *dp, TValue *o, CTInfo qual);

#endif

#endif
