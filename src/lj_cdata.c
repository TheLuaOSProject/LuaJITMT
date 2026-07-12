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
  cdata_flags_rel(cd, cdata_size_tail_flags(allocsz));
  newwhite(g, obj2gco(cd));
  lj_obj_addgcflags(obj2gco(cd), 0x80);
  if (LJ_UNLIKELY(!lj_mem_publish_interior_cdata(L, p, allocsz))) {
    lj_mem_freegco_unpublished(g, p, allocsz);
    lj_oserr_restore(&oserr);
    lj_err_mem(L);
  }
  lj_gc_linkobj_new(g, obj2gco(cd));
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
  tabh = ctype_tabh_acq(cts);
  if (!tabh)
    return 0;
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
    if (!cdata_size_tail_matches(cd, (size_t)size))
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
  if (!cdata_size_tail_matches(cd, (size_t)size))
    return 0;
  if (basep) *basep = base;
  if (sizep) *sizep = size;
  return 1;
}

/* Free a C data object. */
void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd)
{
  if (LJ_UNLIKELY(lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN)) {
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

#if defined(LJ_CDATA_TEST_HELPERS)
static uint32_t cdata_fin_test_pause_armed[LJ_CDATA_FIN_PAUSE__MAX];
static uint32_t cdata_fin_test_pause_waiting[LJ_CDATA_FIN_PAUSE__MAX];
static uint32_t cdata_fin_test_pause_release[LJ_CDATA_FIN_PAUSE__MAX];

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

static void cdata_fin_test_pause(uint32_t point)
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
#define cdata_fin_test_pause(point) ((void)0)
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

static LJ_NORET void cdata_fin_store_invariant(global_State *g,
					       const char *why)
{
  lj_assertG_(g, 0, "%s", why);
  UNUSED(g); UNUSED(why);
  abort();
}

static void cdata_fin_store_exact_or_abort(global_State *g,
					    CTypeFinLease *lease,
					    cTValue *key, cTValue *src,
					    const char *why)
{
  if (!lj_cdata_fin_store_claim_held(lease, key, src))
    cdata_fin_store_invariant(g, why);
}

int lj_cdata_fin_claim_held(CTypeFinLease *lease, cTValue *key, TValue *old,
			    int nonnil)
{
  TValue claim, expect;
  int rc;
  if (!lease || !lease->tab || !lease->slot || !key || !old)
    return LJ_CTYPE_FIN_RETRY;
  rc = lj_tab_read_current_keyed(lease->tab, lease->slot, key, old);
  if (rc != LJ_TAB_STORE_CAS_OK || tvisforward(old) ||
      lj_cdata_fin_isclaim(old))
    return LJ_CTYPE_FIN_RETRY;
  if (nonnil && tvisnil(old))
    return LJ_CTYPE_FIN_MISS;
  cdata_fin_setclaim(&claim);
  expect = *old;
  if (lj_tv_cas(lease->slot, &expect, &claim))
    return LJ_CTYPE_FIN_FOUND;
  *old = expect;
  /* A resize winner publishes FORWARD. Never use it as a CAS expectation on a
  ** retry; release the exact generation certificate and resolve by key again. */
  return LJ_CTYPE_FIN_RETRY;
}

typedef struct CDataFinBarrierCtx {
  GCtab *t;
  cTValue *key;
} CDataFinBarrierCtx;

int lj_cdata_fin_store_claim_held(CTypeFinLease *lease, cTValue *key,
				  cTValue *src)
{
  uint32_t retry;
  if (!lease || !lease->tab || !lease->slot || !key || !src)
    return 0;
  for (retry = 0; retry < 4; retry++) {
    TValue old, expect;
    if (lj_tab_read_current_keyed(lease->tab, lease->slot, key, &old) !=
	LJ_TAB_STORE_CAS_OK || tvisforward(&old) ||
	!lj_cdata_fin_isclaim(&old))
      return 0;
    expect = old;
    if (lj_tv_cas(lease->slot, &expect, src))
      return 1;
    if (tvisforward(&expect) || !lj_cdata_fin_isclaim(&expect))
      return 0;
  }
  return 0;
}

/* Install a key only in its empty anchor node. Collision chaining and resize
** can wait or allocate, so a contended/full anchor falls back to a larger
** generation instead. KEYLOCK plus FINCLAIM make this short path restartable
** without holding a structural-owner lock. */
static int cdata_fin_try_newkey_anchor_held(lua_State *L,
					   CTypeFinLease *lease,
					   cTValue *key, cTValue *claim)
{
  Node *node, *n;
  MSize hmask;
  TValue nk, nv, nilv, keylock, expect;
  int snap, reserved, value_claimed = 0;
  if (!lease || !lease->g || !lease->tab || !lease->smr_held ||
      !key || !tviscdata(key) || !claim)
    return LJ_CTYPE_FIN_RETRY;
  snap = lj_tab_node_snapshot_gc_held(lease->g, lease->tab, &node, &hmask);
  if (snap != LJ_TAB_GC_SNAPSHOT_OK)
    return LJ_CTYPE_FIN_RETRY;
  if (hmask == 0)
    return LJ_CTYPE_FIN_MISS;
  n = hashgcref_node(node, hmask, key->gcr);
  lj_tv_load_acq(&nk, &n->key);
  if (lj_obj_equal(&nk, key) || tviskeylock(&nk))
    return LJ_CTYPE_FIN_RETRY;
  if (!tvisnil(&nk))
    return LJ_CTYPE_FIN_MISS;
  lj_tv_load_acq(&nv, &n->val);
  if (!tvisnil(&nv))
    return LJ_CTYPE_FIN_RETRY;
  reserved = lj_tab_node_free_reserve(node);
  if (reserved <= 0)
    return reserved < 0 ? LJ_CTYPE_FIN_RETRY : LJ_CTYPE_FIN_MISS;
  if (lj_tab_node_acq(lease->tab) != node ||
      lj_tab_node_is_retiring(node)) {
    lj_tab_node_free_release(node);
    return LJ_CTYPE_FIN_RETRY;
  }
  setnilV(&nilv);
  setkeylockV(&keylock);
  expect = nilv;
  if (!lj_tv_cas(&n->key, &expect, &keylock)) {
    lj_tab_node_free_release(node);
    return LJ_CTYPE_FIN_RETRY;
  }
  if (lj_tab_node_acq(lease->tab) != node ||
      lj_tab_node_is_retiring(node))
    goto rollback_key;
  expect = nilv;
  if (!lj_tv_cas(&n->val, &expect, claim))
    goto rollback_key;
  value_claimed = 1;
  if (lj_tab_node_acq(lease->tab) != node ||
      lj_tab_node_is_retiring(node))
    goto rollback_value;
  /* The cdata key is already validated and rooted by lj_cdata_setfin(). */
  copyTVrel(L, &n->key, key);
  lj_gc_pubtabkey(L, lease->tab, key);
  lease->slot = &n->val;
  return LJ_CTYPE_FIN_FOUND;

rollback_value:
  value_claimed = 1;
rollback_key:
  {
    int clean = 1;
    if (value_claimed) {
      expect = *claim;
      if (!lj_tv_cas(&n->val, &expect, &nilv))
	clean = 0;
    }
    expect = keylock;
    if (!lj_tv_cas(&n->key, &expect, &nilv))
      clean = 0;
    if (clean) {
      lj_tab_node_free_release(node);
      return LJ_CTYPE_FIN_RETRY;
    }
  }
  /* KEYLOCK and FINCLAIM are owned by this exact reservation. Resize and table
  ** clear wait for both, so losing either rollback CAS is not recoverable
  ** contention. Returning would expose an internal sentinel to ordinary table
  ** traversal and silently brick the registry. */
  cdata_fin_store_invariant(lease->g,
			    "FINREG anchor rollback ownership lost");
}

static TValue *cdata_fin_weak_key_barrier_cp(lua_State *L, lua_CFunction dummy,
					     void *ud)
{
  CDataFinBarrierCtx *ctx = (CDataFinBarrierCtx *)ud;
  UNUSED(dummy);
  lj_gc2_barrier_weak_key(L, ctx->t, ctx->key);
  return NULL;
}

static void cdata_fin_weak_key_barrier_claimed(lua_State *L,
					       global_State *g, CTState *cts,
					       CTypeFinLease *lease,
					       cTValue *key, cTValue *restore,
					       FinRegOrderNode **ordp)
{
  CDataFinBarrierCtx ctx;
  int errcode;
  ctx.t = lease->tab;
  ctx.key = key;
  errcode = lj_vm_cpcall(L, NULL, &ctx, cdata_fin_weak_key_barrier_cp);
  if (LJ_UNLIKELY(errcode)) {
    TValue nilv;
    cTValue *src = restore;
    setnilV(&nilv);
    if (!src || lj_cdata_fin_isclaim(src))
      src = &nilv;
    cdata_fin_store_exact_or_abort(g, lease, key, src,
				   "FINREG barrier unwind lost slot claim");
    if (ordp && *ordp) {
      lj_ctype_fin_order_free(g, *ordp);
      *ordp = NULL;
    }
    UNUSED(cts);
    /* cpcall caught the internal unwind. Drop both lifetime certificates
    ** before rethrowing across this C frame. */
    lj_ctype_fin_lease_release(lease);
    lj_err_throw(L, errcode);
  }
}

static int cdata_fin_store_enabled(lua_State *L, global_State *g,
				   CTState *cts, CTypeFinLease *lease,
				   GCcdata *cd, cTValue *key, TValue *val,
				   cTValue *restore,
				   FinRegOrderNode **ordp)
{
  GCtab *t = lease->tab;
  TValue *tv = lease->slot;
  int fresh = tvisnil(restore);

  /* Re-registering the exact same callback is an identity-preserving slot
  ** transaction. Keep its one ordered membership and active-set accounting;
  ** only close a scanner which may have observed FINCLAIM. */
  if (!fresh && lj_obj_equal(restore, val)) {
    cdata_fin_store_exact_or_abort(g, lease, key, restore,
				   "FINREG identical enable lost slot claim");
    lj_gc2_barrier_weak_value(L, t, restore);
    lj_gc2_barrier_tv_pair(L, obj2gco(t), restore);
    lj_gc_pubtab(L, t);
    lj_ctype_fin_lease_release(lease);
    return 1;
  }

  cdata_fin_weak_key_barrier_claimed(L, g, cts, lease, key, restore,
				    ordp);
  if (!fresh) {
    size_t retired = 0;
    if (!lj_ctype_fin_order_retire_obj_complete(
	  cts, obj2gco(cd), &retired)) {
      /* Once even one membership is retired, restoring the callback would
      ** expose a live registration without a complete discovery certificate.
      ** Published native nodes must remain admissible under the outer SMR
      ** lease; a partial commit is therefore an invariant failure, not an API
      ** retry. A zero-mutation transient remains exactly rollbackable. */
      if (retired != 0)
	cdata_fin_store_invariant(g,
	  "FINREG replacement retirement partially committed");
      cdata_fin_store_exact_or_abort(
	g, lease, key, restore,
	"FINREG replacement rollback lost exact slot claim");
      lj_ctype_fin_lease_release(lease);
      return 0;
    }
  }
  if (ordp && *ordp) {
    /* Publish the ordered node while the slot still contains FINCLAIM.
    ** Ordered discovery treats that sentinel as an in-flight transaction. */
    lj_ctype_fin_order_publish(cts, *ordp, obj2gco(cd), t, tv);
    *ordp = NULL;
  }
  cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_ENABLE_ORDER);
  lj_obj_addgcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
  if (fresh)
    lj_gc2_finreg_cdata_set(g, obj2gco(cd), 1);
  cdata_fin_store_exact_or_abort(g, lease, key, val,
				 "FINREG enable lost exact slot claim");
  lj_gc2_barrier_weak_value(L, t, val);
  lj_gc2_barrier_tv_pair(L, obj2gco(t), val);
  lj_gc_pubtab(L, t);  /* 11.4 FINREG publish after claim resolution. */
  lj_ctype_fin_lease_release(lease);
  return 1;
}

/* FINCLAIM is the clear transaction LP. No later enable can observe nil until
** every old ordered membership is logically retired and the old FIN bit is
** gone. Conversely, a transient spine scan restores the exact old value before
** releasing either lifetime certificate, leaving the registration unchanged. */
static int cdata_fin_clear_claimed(lua_State *L, global_State *g,
				    CTState *cts, CTypeFinLease *lease,
				    GCcdata *cd, cTValue *key, cTValue *old)
{
  TValue nilv;
  size_t retired = 0;
  /* A previously committed clear leaves a stable nil slot with no FIN bit.
  ** Repeating ffi.gc(cd, nil) is an identity no-op, not another clear event.
  ** Nil plus FIN is distinct: GC2 may own a queued preclaim which an explicit
  ** clear still has to suppress under this exact slot transaction. */
  if (tvisnil(old) &&
      !(lj_obj_gcflags(obj2gco(cd)) & LJ_GC_CDATA_FIN)) {
    cdata_fin_store_exact_or_abort(g, lease, key, old,
				   "FINREG nil clear lost slot claim");
    lj_ctype_fin_lease_release(lease);
    return 1;
  }
  if (!lj_ctype_fin_order_retire_obj_complete(
	cts, obj2gco(cd), &retired)) {
    if (retired != 0)
      cdata_fin_store_invariant(g,
	"FINREG clear retirement partially committed");
    cdata_fin_store_exact_or_abort(
      g, lease, key, old, "FINREG clear rollback lost exact slot claim");
    lj_ctype_fin_lease_release(lease);
    return 0;
  }
  UNUSED(retired);
  lj_obj_cleargcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
  lj_gc2_finreg_cdata_set(g, obj2gco(cd), 0);
  cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_CLEAR_BEFORE_NIL);
  setnilV(&nilv);
  cdata_fin_store_exact_or_abort(g, lease, key, &nilv,
				 "FINREG clear lost exact slot claim");
  lj_gc_pubtab(L, lease->tab);
  lj_ctype_fin_lease_release(lease);
  return 1;
}

void lj_cdata_setfin(lua_State *L, GCcdata *cd, GCobj *obj, uint32_t it)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  ptrdiff_t anchor;
  TValue key, val, old, claim;
  FinRegOrderNode *ord = NULL;
  int enabled = (it != LJ_TNIL);
  if (LJ_UNLIKELY(cts == NULL))
    return;
  lj_state_checkstack(L, enabled ? 2u : 1u);
  anchor = savestack(L, L->top);
  setcdataV(L, &key, cd);
  if (enabled) {
    TValue *top = restorestack(L, anchor);
    setgcV(L, &val, obj, it);
    if (LJ_LIKELY(top + 2 < tvref(L->maxstack))) {
      copyTVrel(L, top, &key);
      copyTVrel(L, top + 1, &val);
      L->top = top + 2;
    }
    lj_gc_pubroot(L, &val);
    ord = lj_ctype_fin_order_new(L);
  } else {
    TValue *top = restorestack(L, anchor);
    if (LJ_LIKELY(top + 1 < tvref(L->maxstack))) {
      copyTVrel(L, top, &key);
      L->top = top + 1;
    }
  }
  if (!enabled)
    lj_gc_pubroot(L, &key);
  for (;;) {
    CTypeFinLease held = CTYPE_FIN_LEASE_INIT;
    int rc = lj_ctype_fin_get(L, cts, &key, &held);
    if (rc == LJ_CTYPE_FIN_RETRY) {
      lj_ctype_fin_lease_release(&held);
      if (enabled)
	cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_ENABLE_RETRY);
      else
	cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
      cdata_fin_claim_wait(L);
      continue;
    }
    if (rc == LJ_CTYPE_FIN_MISS) {
      if (!enabled) {  /* Missing clear is a no-op; avoid structural insert. */
	cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_CLEAR_MISS);
	goto done;
      }
      rc = lj_ctype_fin_head(cts, &held);
      if (rc == LJ_CTYPE_FIN_RETRY) {
	lj_ctype_fin_lease_release(&held);
	cdata_fin_claim_wait(L);
	continue;
      }
      if (rc == LJ_CTYPE_FIN_FOUND) {
	if (!fin_gen_tab_enabled_acq(held.tab)) {
	  lj_ctype_fin_lease_release(&held);
	} else {
	  cdata_fin_setclaim(&claim);
	  setnilV(&old);
	  rc = cdata_fin_try_newkey_anchor_held(L, &held, &key, &claim);
	  if (rc == LJ_CTYPE_FIN_FOUND) {
	    if (!fin_gen_tab_enabled_acq(held.tab)) {
	      cdata_fin_store_exact_or_abort(
		g, &held, &key, &old,
		"disabled FINREG anchor lost exact slot claim");
	      lj_ctype_fin_lease_release(&held);
	      continue;
	    }
	    (void)cdata_fin_store_enabled(L, g, cts, &held, cd, &key,
					   &val, &old, &ord);
	    goto done;
	  }
	  lj_ctype_fin_lease_release(&held);
	  if (rc == LJ_CTYPE_FIN_ERROR) {
	    cdata_fin_claim_wait(L);
	    continue;
	  }
	  if (rc == LJ_CTYPE_FIN_RETRY) {
	    cdata_fin_claim_wait(L);
	    continue;  /* Key publication or vector generation raced us. */
	  }
	}
      }
      cdata_fin_setclaim(&claim);
      setnilV(&old);
      switch (lj_ctype_fin_newgen(L, cts, &key, &claim, &held)) {
      case 1:
	if (!fin_gen_tab_enabled_acq(held.tab)) {
	  cdata_fin_store_exact_or_abort(
	    g, &held, &key, &old,
	    "disabled fresh FINREG generation lost exact slot claim");
	  lj_ctype_fin_lease_release(&held);
	  continue;
	}
	(void)cdata_fin_store_enabled(L, g, cts, &held, cd, &key, &val,
				      &old, &ord);
	goto done;
      case -1:
	continue;  /* Racing generation already has this cdata key. */
      case LJ_CTYPE_FIN_ERROR:
	cdata_fin_claim_wait(L);
	continue;
      default:
	cdata_fin_claim_wait(L);
	continue;
      }
    }
    rc = lj_cdata_fin_claim_held(&held, &key, &old, 0);
    if (rc != LJ_CTYPE_FIN_FOUND) {
      lj_ctype_fin_lease_release(&held);
      if (enabled)
	cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_ENABLE_RETRY);
      else
	cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
      cdata_fin_claim_wait(L);
      continue;
    }
    if (!fin_gen_tab_enabled_acq(held.tab)) {
      cdata_fin_store_exact_or_abort(
	g, &held, &key, &old,
	"disabled FINREG slot lost exact claim rollback");
      lj_ctype_fin_lease_release(&held);
      continue;
    }
    if (enabled) {
      if (!cdata_fin_store_enabled(L, g, cts, &held, cd, &key, &val,
				   &old, &ord)) {
	cdata_fin_claim_wait(L);
	continue;
      }
    }
    else if (!cdata_fin_clear_claimed(L, g, cts, &held, cd, &key, &old)) {
      cdata_fin_test_pause(LJ_CDATA_FIN_PAUSE_CLEAR_RETRY);
      cdata_fin_claim_wait(L);
      continue;
    }
    goto done;
  }
done:
  if (ord)
    lj_ctype_fin_order_free(g, ord);
  L->top = restorestack(L, anchor);
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
  tabh = ctype_tabh_acq(cts);
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
