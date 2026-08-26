/*
** Metamethod handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_meta_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_state.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_lib.h"
#include "lj_tg.h"

#if defined(LJ_GC2_TEST_HELPERS)
static uintptr_t meta_test_mt_capture_target;
static uint32_t meta_test_mt_capture_armed;
static uint32_t meta_test_mt_capture_paused;
static uint32_t meta_test_mt_capture_release;
static uintptr_t meta_test_mt_lease_target;
static uint32_t meta_test_mt_lease_armed;
static uint32_t meta_test_mt_lease_paused;
static uint32_t meta_test_mt_lease_release;

void lj_meta_test_mt_capture_pause(GCobj *target)
{
  la_store32_rel(&meta_test_mt_capture_release, 0);
  la_store32_rel(&meta_test_mt_capture_paused, 0);
  la_storeuptr_rel(&meta_test_mt_capture_target,
		   (uintptr_t)(void *)target);
  la_store32_rel(&meta_test_mt_capture_armed, target != NULL);
}

uint32_t lj_meta_test_mt_capture_paused(void)
{
  return la_load32_acq(&meta_test_mt_capture_paused);
}

void lj_meta_test_mt_capture_release(void)
{
  la_store32_rel(&meta_test_mt_capture_release, 1);
}

void lj_meta_test_mt_lease_pause(GCobj *target)
{
  la_store32_rel(&meta_test_mt_lease_release, 0);
  la_store32_rel(&meta_test_mt_lease_paused, 0);
  la_storeuptr_rel(&meta_test_mt_lease_target, (uintptr_t)(void *)target);
  la_store32_rel(&meta_test_mt_lease_armed, target != NULL);
}

uint32_t lj_meta_test_mt_lease_paused(void)
{
  return la_load32_acq(&meta_test_mt_lease_paused);
}

void lj_meta_test_mt_lease_release(void)
{
  la_store32_rel(&meta_test_mt_lease_release, 1);
}

static void meta_test_pause_after_mt_capture(cTValue *o, GCtab *mt)
{
  uint32_t expect = 1;
  if (!mt || !tvisgcv(o) ||
      (GCobj *)(void *)la_loaduptr_acq(&meta_test_mt_capture_target) !=
	gcV(o) ||
      !la_cas32(&meta_test_mt_capture_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&meta_test_mt_capture_paused, 1);
  while (la_load32_acq(&meta_test_mt_capture_release) == 0)
    la_cpu_pause();
  la_store32_rel(&meta_test_mt_capture_paused, 0);
  la_storeuptr_rel(&meta_test_mt_capture_target, 0);
}

static void meta_test_pause_after_mt_lease(GCtab *mt)
{
  uint32_t expect = 1;
  if (!mt ||
      (GCobj *)(void *)la_loaduptr_acq(&meta_test_mt_lease_target) !=
	obj2gco(mt) ||
      !la_cas32(&meta_test_mt_lease_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&meta_test_mt_lease_paused, 1);
  while (la_load32_acq(&meta_test_mt_lease_release) == 0)
    la_cpu_pause();
  la_store32_rel(&meta_test_mt_lease_paused, 0);
  la_storeuptr_rel(&meta_test_mt_lease_target, 0);
}
#else
#define meta_test_pause_after_mt_capture(o, mt) \
  ((void)(o), (void)(mt))
#define meta_test_pause_after_mt_lease(mt) ((void)(mt))
#endif

/* -- Metamethod handling ------------------------------------------------- */

/* String interning of metamethod names for fast indexing. */
void lj_meta_init(lua_State *L)
{
#define MMNAME(name)	"__" #name
  const char *metanames = MMDEF(MMNAME);
#undef MMNAME
  global_State *g = G(L);
  const char *p, *q;
  uint32_t mm;
  for (mm = 0, p = metanames; *p; mm++, p = q) {
    GCstr *s;
    for (q = p+2; *q && *q != '_'; q++) ;
    s = lj_str_new(L, p, (size_t)(q-p));
    /* NOBARRIER: g->gcroot[] is a GC root. */
    lj_gcroot_rel(g, (GCRootID)(GCROOT_MMNAME+mm), obj2gco(s));
  }
}

/* Negative caching of a few fast metamethods. See the lj_meta_fast() macro. */
cTValue *lj_meta_cache(GCtab *mt, MMS mm, GCstr *name)
{
  cTValue *mo = lj_tab_getstr(mt, name);
  TValue snap;
  lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
  if (!mo)
    return NULL;
  lj_tv_load_acq(&snap, mo);
  /*
  ** Lock-free table snapshots can race with retired storage reuse. A tagged GC
  ** value whose header no longer matches its tag is not a callable/effective
  ** metamethod for this lookup; treating it as absent preserves safety without
  ** exposing internal forwarding or reuse artifacts to VM dispatch.
  */
  if (!lj_tv_gcref_type_match(&snap) || tvisnil(&snap)) {
    return NULL;
  }
  return mo;
}

cTValue *lj_meta_cachetv(GCtab *mt, MMS mm, GCstr *name, TValue *out)
{
  cTValue *mo = lj_tab_getstr(mt, name);
  lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
  if (!mo)
    return NULL;
  lj_tv_load_acq(out, mo);
  return (!lj_tv_gcref_type_match(out) || tvisnil(out)) ? NULL : out;
}

cTValue *lj_meta_cachetv_l(lua_State *L, GCtab *mt, MMS mm, GCstr *name,
			   TValue *out)
{
  TValue key;
  lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
  setstrV(L, &key, name);
  (void)lj_tab_gettv_forjit(L, mt, &key, out);
  /* The copied helper normalizes stale GC snapshots to nil and publishes every
  ** valid GC result under its exact lease. Do not re-read an unleased header. */
  return tvisnil(out) ? NULL : out;
}

/* Lookup metamethod for object. */
cTValue *lj_meta_lookup(lua_State *L, cTValue *o, MMS mm)
{
  GCtab *mt;
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt) {
    cTValue *mo = lj_tab_getstr(mt, mmname_str(G(L), mm));
    if (mo)
      return mo;
  }
  return niltv(L);
}

cTValue *lj_meta_lookuptv(lua_State *L, TValue *out, cTValue *o, MMS mm)
{
  global_State *g = G(L);
  TValue osnap, mtv, result;
  for (;;) {
    LJGC2Lease objlease, mtlease, resultlease;
    GCtab *mt;
    int ostatus, mtstatus, lookupstatus, resultstatus;

    /* Open source SMR before copying the authoritative receiver root. A lease
    ** acquired from a pre-SMR pointer could otherwise admit a same-address
    ** successor after a raced root replacement/reclaim. One acquired snapshot
    ** is the lookup's linearization candidate: never lease one incarnation and
    ** then branch through a second read of the slot. Callers which pre-copy a
    ** mutable registry/upvalue edge into a C local have not supplied an
    ** authoritative root and must be converted at that call site. */
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
      lj_tab_wait_l(L);
      continue;
    }
    lj_tv_load_acq(&osnap, o);
    ostatus = lj_gc2_tv_lease_acquire(g, &osnap, &objlease);
    if (LJ_UNLIKELY(ostatus != LJ_GC2_TV_EDGE_VALID)) {
      lj_gc2_smr_read_leave(g);
      if (ostatus == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      setnilV(&result);
      copyTVrel(L, out, &result);
      return out;
    }
    if (tvistab(&osnap))
      mt = lj_tab_metatable_acq(tabV(&osnap));
    else if (tvisudata(&osnap))
      mt = lj_udata_metatable_acq(udataV(&osnap));
    else
      mt = lj_basemt_obj_acq(g, &osnap);
    meta_test_pause_after_mt_capture(&osnap, mt);
    if (!mt) {
      lj_gc2_smr_read_leave(g);
      lj_gc2_lease_release(&objlease);
      setnilV(&result);
      copyTVrel(L, out, &result);
      return out;
    }

    /* The source-side SMR interval prevents reclamation/reuse between pointer
    ** capture and target admission. Replacement of the receiver's metatable
    ** therefore chooses the old or new table as a valid linearization, never a
    ** new incarnation at the same address. */
    setgcVraw(&mtv, obj2gco(mt), LJ_TTAB);
    mtstatus = lj_gc2_tv_lease_acquire(g, &mtv, &mtlease);
    if (mtstatus == LJ_GC2_TV_EDGE_VALID)
      meta_test_pause_after_mt_lease(mt);
    lj_gc2_smr_read_leave(g);
    if (LJ_UNLIKELY(mtstatus != LJ_GC2_TV_EDGE_VALID)) {
      lj_gc2_lease_release(&objlease);
      if (mtstatus == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      setnilV(&result);
      copyTVrel(L, out, &result);
      return out;
    }
    /* This lookup is deliberately one-shot. lj_tab_gettv_forjit() owns a retry
    ** loop which may service a safepoint and longjmp; calling it with objlease
    ** and mtlease held would strand both leases. The held nonwaiting lookup is
    ** bounded and never waits. Admit its copied result before closing SMR, then
    ** release every outer lease before the caller-visible retry wait. */
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
      lj_gc2_lease_release(&mtlease);
      lj_gc2_lease_release(&objlease);
      lj_tab_wait_l(L);
      continue;
    }
    lookupstatus = lj_tab_getstr_held_try(g, mt, mmname_str(g, mm),
					  &result);
    if (lookupstatus == LJ_TAB_GC_LOOKUP_FOUND &&
        LJ_UNLIKELY(tvistabinternal(&result)))
      lookupstatus = LJ_TAB_GC_LOOKUP_RETRY;
    resultstatus = lookupstatus == LJ_TAB_GC_LOOKUP_FOUND ?
      lj_gc2_tv_lease_acquire(g, &result, &resultlease) :
      LJ_GC2_TV_EDGE_VALID;
    if (lookupstatus == LJ_TAB_GC_LOOKUP_FOUND &&
        resultstatus == LJ_GC2_TV_EDGE_VALID) {
      /* Publish from the admitted local into the caller-provided result root
      ** before the source vector SMR interval closes. No raw vector value or
      ** unleased GC result crosses this helper boundary. */
      copyTVrel(L, out, &result);
      lj_gc2_smr_read_leave(g);
      lj_gc_pubroot(L, out);
      lj_gc2_lease_release(&resultlease);
      lj_gc2_lease_release(&mtlease);
      lj_gc2_lease_release(&objlease);
      return out;
    }
    lj_gc2_smr_read_leave(g);
    lj_gc2_lease_release(&mtlease);
    lj_gc2_lease_release(&objlease);
    if (lookupstatus == LJ_TAB_GC_LOOKUP_RETRY ||
        resultstatus == LJ_GC2_TV_EDGE_RETRY) {
      lj_tab_wait_l(L);
      continue;
    }
    /* A stable absent slot, or a stale result incarnation observed before its
    ** SMR scope closed, both have the semantic value nil for this lookup. */
    setnilV(&result);
    copyTVrel(L, out, &result);
    return out;
  }
}

#if LJ_HASFFI
/* Tailcall from C function. */
int lj_meta_tailcall(lua_State *L, cTValue *tv)
{
  TValue *base = L->base;
  TValue *top = L->top;
  TValue *funcslot = base-1-LJ_FR2;
  TValue *dummyslot;
  const BCIns *pc = frame_pc(base-1);  /* Preserve old PC from frame. */
  copyTV(L, funcslot, tv);  /* Replace frame with new object. */
  if (LJ_FR2)
    (top++)->u64 = LJ_CONT_TAILCALL;
  else
    top->u32.lo = LJ_CONT_TAILCALL;
  setframe_pc(top++, pc);
  dummyslot = top;
  setframe_gc(dummyslot, obj2gco(L), LJ_TTHREAD);  /* Dummy frame object. */
  if (LJ_FR2) top++;
  setframe_ftsz(top, ((char *)(top+1) - (char *)base) + FRAME_CONT);
  L->base = L->top = top+1;
  /* CType metamethod callers release their exact private anchor immediately
  ** after this handoff. Publish the complete replacement frame first, so a
  ** concurrent stack scan acquires both the new callable/object and the dummy
  ** continuation-frame owner instead of relying on the removed metatable edge. */
  lj_state_stack_pubtv(L, L, funcslot);
  lj_state_stack_pubtv(L, L, dummyslot);
  /*
  ** before:   [old_mo|PC]    [... ...]
  **                         ^base     ^top
  ** after:    [new_mo|itype] [... ...] [NULL|PC] [dummy|delta]
  **                                                           ^base/top
  ** tailcall: [new_mo|PC]    [... ...]
  **                         ^base     ^top
  */
  return 0;
}
#endif

/* Setup call to metamethod to be run by Assembler VM. */
static TValue *mmcall(lua_State *L, ASMFunction cont, cTValue *mo,
		    cTValue *a, cTValue *b)
{
  /*
  **           |-- framesize -> top       top+1       top+2 top+3
  ** before:   [func slots ...]
  ** mm setup: [func slots ...] [cont|?]  [mo|tmtype] [a]   [b]
  ** in asm:   [func slots ...] [cont|PC] [mo|delta]  [a]   [b]
  **           ^-- func base                          ^-- mm base
  ** after mm: [func slots ...]           [result]
  **                ^-- copy to base[PC_RA] --/     for lj_cont_ra
  **                          istruecond + branch   for lj_cont_cond*
  **                                       ignore   for lj_cont_nop
  ** next PC:  [func slots ...]
  */
  TValue *top = L->top;
  TValue *metafunc;
  if (curr_funcisL(L)) top = curr_topL(L);
  setcont(top++, cont);  /* Assembler VM stores PC in upper word or FR2. */
  if (LJ_FR2) setnilV(top++);
  metafunc = top;
  copyTV(L, top++, mo);  /* Store metamethod and two arguments. */
  if (LJ_FR2) setnilV(top++);
  copyTV(L, top, a);
  copyTV(L, top+1, b);
  /* The VM has not advanced L->top yet. Publish every collectable slot in the
  ** manual frame before returning its argument base to assembler dispatch. */
  lj_state_stack_pubtv(L, L, metafunc);
  lj_state_stack_pubtv(L, L, top);
  lj_state_stack_pubtv(L, L, top+1);
  return top;  /* Return new base. */
}

/* -- C helpers for some instructions, called from assembler VM ----------- */

enum {
  META_CHAIN_RECEIVER,
  META_CHAIN_KEY,
  META_CHAIN_RESULT,
  META_CHAIN_METHOD,
  META_CHAIN_NROOTS
};

typedef struct MetaChainRoots {
  TGState *tg;
  uint32_t idx[META_CHAIN_NROOTS];
} MetaChainRoots;

typedef struct MetaSourceRef {
  cTValue *tv;
  ptrdiff_t ofs;
  uint8_t stack;
  uint8_t funcenv;
} MetaSourceRef;

typedef struct MetaOutputRef {
  TValue *tv;
  ptrdiff_t ofs;
  uint8_t stack;
} MetaOutputRef;

enum {
  META_CAPTURE_VALID,
  META_CAPTURE_RECEIVER_STALE,
  META_CAPTURE_KEY_STALE
};

static LJ_AINLINE int meta_tv_on_stack(lua_State *L, cTValue *tv)
{
  uintptr_t p, lo, hi;
  if (!L || !tv)
    return 0;
  p = (uintptr_t)(const void *)tv;
  lo = (uintptr_t)(const void *)tvref(L->stack);
  hi = (uintptr_t)(const void *)tvref(L->maxstack);
  return p >= lo && p < hi;
}

static LJ_AINLINE MetaSourceRef meta_source_ref(lua_State *L, cTValue *tv)
{
  MetaSourceRef ref;
  ref.tv = tv;
  ref.stack = (uint8_t)meta_tv_on_stack(L, tv);
  ref.funcenv = 0;
  ref.ofs = ref.stack ? savestack(L, (TValue *)(void *)tv) : 0;
  return ref;
}

/* A global bytecode starts from the authoritative current-function stack
** root, not from a pre-copied fn->env pointer. Resolve that mutable child edge
** only after the chain anchors exist and source SMR has opened. */
static LJ_AINLINE MetaSourceRef meta_funcenv_source_ref(lua_State *L,
						 cTValue *tv)
{
  MetaSourceRef ref = meta_source_ref(L, tv);
  ref.funcenv = 1;
  return ref;
}

static LJ_AINLINE cTValue *meta_source_current(lua_State *L,
					       const MetaSourceRef *ref)
{
  return ref->stack ? restorestack(L, ref->ofs) : ref->tv;
}

static LJ_AINLINE MetaOutputRef meta_output_ref(lua_State *L, TValue *tv)
{
  MetaOutputRef ref;
  ref.tv = tv;
  ref.stack = (uint8_t)meta_tv_on_stack(L, tv);
  ref.ofs = ref.stack ? savestack(L, tv) : 0;
  return ref;
}

static LJ_AINLINE TValue *meta_output_current(lua_State *L,
					      const MetaOutputRef *ref)
{
  return ref->stack ? restorestack(L, ref->ofs) : ref->tv;
}

static LJ_AINLINE TValue *meta_chain_root(const MetaChainRoots *roots,
					   uint32_t which)
{
  return lj_tg_root_anchor_slot_acq(roots->tg, roots->idx[which]);
}

static void meta_chain_roots_init(lua_State *L, MetaChainRoots *roots)
{
  TValue nilv;
  uint32_t i;
  roots->tg = L2TG(L);
  setnilV(&nilv);
  for (i = 0; i < META_CHAIN_NROOTS; i++) {
    TValue *slot;
    if (LJ_UNLIKELY(!lj_tg_root_anchor_reserve_nothrow(L, roots->tg))) {
      /* Reserve before push so an allocation failure can explicitly unwind
      ** every already-published member of this root set. The protected-call
      ** checkpoint remains a second line of defence for unrelated throws. */
      while (i != 0) {
	i--;
	lj_tg_root_anchor_pop(roots->tg, roots->idx[i]);
      }
      lj_err_mem(L);
    }
    slot = lj_tg_root_anchor_push(L, roots->tg, &nilv, &roots->idx[i]);
    lj_assertL(slot != NULL, "metamethod chain root allocation failed");
    UNUSED(slot);
  }
}

static void meta_chain_roots_fini(MetaChainRoots *roots)
{
  uint32_t i = META_CHAIN_NROOTS;
  while (i-- != 0)
    lj_tg_root_anchor_pop(roots->tg, roots->idx[i]);
}

/* Capture the caller's authoritative receiver/key pair before any semantic
** lookup. Source SMR starts before both mutable-root loads, and both exact
** leases survive release-publication into owner-private enumerated anchors.
** VM stack roots and VM scratch/constant roots satisfy this contract. A C
** caller which first copied a replaceable registry/upvalue edge into a local
** has already lost source authority and must root that edge at its call site.
**
** The remaining lj_tab_wait_l() calls are deliberate b1.2.1 zero-wait debt:
** every lease and SMR interval is closed before servicing the retry. */
static int meta_chain_capture_inputs(lua_State *L, MetaChainRoots *roots,
				     const MetaSourceRef *oref,
				     const MetaSourceRef *kref)
{
  global_State *g = G(L);
  for (;;) {
    LJGC2Lease olease, envlease, klease;
    TValue osnap, ksnap;
    cTValue *o, *k;
    int ostatus, envstatus, kstatus;
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
      lj_tab_wait_l(L);
      continue;
    }
    o = meta_source_current(L, oref);
    k = meta_source_current(L, kref);
    lj_tv_load_acq(&osnap, o);
    ostatus = lj_gc2_tv_lease_acquire(g, &osnap, &olease);
    if (LJ_UNLIKELY(ostatus != LJ_GC2_TV_EDGE_VALID)) {
      lj_gc2_smr_read_leave(g);
      if (ostatus == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      return META_CAPTURE_RECEIVER_STALE;
    }
    if (oref->funcenv) {
      GCtab *env;
      /* The function lease authorizes the child-edge load. The surrounding
      ** source SMR interval then prevents replacement-to-reuse ABA until the
      ** exact environment incarnation has acquired its own lease. */
      if (LJ_UNLIKELY(!tvisfunc(&osnap))) {
	lj_gc2_smr_read_leave(g);
	lj_gc2_lease_release(&olease);
	return META_CAPTURE_RECEIVER_STALE;
      }
      env = lj_func_env_acq(funcV(&osnap));
      if (env)
	setgcVraw(&osnap, obj2gco(env), LJ_TTAB);
      else
	setnilV(&osnap);
      envstatus = lj_gc2_tv_lease_acquire(g, &osnap, &envlease);
      if (LJ_UNLIKELY(envstatus != LJ_GC2_TV_EDGE_VALID)) {
	lj_gc2_smr_read_leave(g);
	lj_gc2_lease_release(&olease);
	if (envstatus == LJ_GC2_TV_EDGE_RETRY) {
	  lj_tab_wait_l(L);
	  continue;
	}
	return META_CAPTURE_RECEIVER_STALE;
      }
    }
    lj_tv_load_acq(&ksnap, k);
    kstatus = lj_gc2_tv_lease_acquire(g, &ksnap, &klease);
    if (LJ_UNLIKELY(kstatus != LJ_GC2_TV_EDGE_VALID)) {
      lj_gc2_smr_read_leave(g);
      if (oref->funcenv)
	lj_gc2_lease_release(&envlease);
      lj_gc2_lease_release(&olease);
      if (kstatus == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      return META_CAPTURE_KEY_STALE;
    }
    copyTVrel(L, meta_chain_root(roots, META_CHAIN_RECEIVER), &osnap);
    copyTVrel(L, meta_chain_root(roots, META_CHAIN_KEY), &ksnap);
    lj_gc2_smr_read_leave(g);
    lj_gc_pubroot(L, meta_chain_root(roots, META_CHAIN_RECEIVER));
    lj_gc_pubroot(L, meta_chain_root(roots, META_CHAIN_KEY));
    lj_gc2_lease_release(&klease);
    if (oref->funcenv)
      lj_gc2_lease_release(&envlease);
    lj_gc2_lease_release(&olease);
    return META_CAPTURE_VALID;
  }
}

/* Transfer a rooted point-read result directly to the caller's enumerated
** result slot. Re-admit the exact anchored value before release-publication;
** returning this slot makes the caller's legacy immediate copy idempotent. */
static TValue *meta_chain_value_publish(lua_State *L,
					const MetaChainRoots *roots,
					uint32_t which,
					const MetaOutputRef *outref)
{
  global_State *g = G(L);
  for (;;) {
    LJGC2Lease lease;
    TValue *out;
    TValue snap;
    int status;
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
      lj_tab_wait_l(L);
      continue;
    }
    lj_tv_load_acq(&snap, meta_chain_root(roots, which));
    status = lj_gc2_tv_lease_acquire(g, &snap, &lease);
    out = meta_output_current(L, outref);
    if (status == LJ_GC2_TV_EDGE_VALID) {
      copyTVrel(L, out, &snap);
      lj_gc2_smr_read_leave(g);
      if (outref->stack)
	lj_state_stack_pubtv(L, L, out);
      else
	lj_gc_pubroot(L, out);
      lj_gc2_lease_release(&lease);
      return meta_output_current(L, outref);
    }
    lj_gc2_smr_read_leave(g);
    if (status == LJ_GC2_TV_EDGE_STALE) {
      setnilV(&snap);
      copyTVrel(L, out, &snap);
      if (outref->stack)
	lj_state_stack_pubtv(L, L, out);
      else
	lj_gc_pubroot(L, out);
      return meta_output_current(L, outref);
    }
    lj_tab_wait_l(L);
  }
}

static TValue *meta_error_stack_slot(lua_State *L,
				      const MetaSourceRef *oref,
				      const MetaSourceRef *kref)
{
  cTValue *src;
  if (!oref->funcenv) {
    src = meta_source_current(L, oref);
    if (meta_tv_on_stack(L, src))
      return (TValue *)(void *)src;
  }
  src = meta_source_current(L, kref);
  if (meta_tv_on_stack(L, src))
    return (TValue *)(void *)src;
  return L->base;
}

/* Preserve a valid chain-derived operand in an existing enumerated stack
** register before releasing the private anchor set. Semantic error
** construction may allocate and collect, and lj_err_optype() expects a real
** frame-relative operand rather than a C-local or beyond-top scratch slot. */
static TValue *meta_chain_error_operand(lua_State *L,
					const MetaChainRoots *roots,
					const MetaSourceRef *oref,
					const MetaSourceRef *kref)
{
  TValue *slot = meta_error_stack_slot(L, oref, kref);
  copyTVrel(L, slot, meta_chain_root(roots, META_CHAIN_RECEIVER));
  lj_state_stack_pubtv(L, L, slot);
  return slot;
}

/* A terminal stale source is safe only for its tag/type diagnostic. If it is
** not already a frame-relative slot, copy the raw word into one without a
** liveness assertion; lj_err_optype() reads the tag before it can allocate. */
static TValue *meta_stale_error_operand(lua_State *L,
					const MetaSourceRef *oref,
					const MetaSourceRef *kref)
{
  cTValue *src = meta_source_current(L, oref);
  TValue *slot = meta_error_stack_slot(L, oref, kref);
  if (slot != src)
    tv_rawstore_rel(slot, tv_rawload_acq(src));
  return slot;
}

/* Materialize a function-valued chain result in the VM continuation frame.
** mmcall() release-publishes the function and both arguments before returning;
** advance top only after that complete handoff and before dropping the anchors
** which fed it. */
static void meta_chain_mmcall(lua_State *L, ASMFunction cont, cTValue *method,
			      cTValue *receiver, cTValue *key)
{
  TValue *base = mmcall(L, cont, method, receiver, key);
  L->top = base;
}

/* Shared rooted implementation for TGET*. __index chain and metamethod. */
static cTValue *meta_tget_rooted_mode(lua_State *L, cTValue *o, cTValue *k,
				      TValue *out, int funcenv)
{
  MetaChainRoots roots;
  MetaSourceRef oref = funcenv ? meta_funcenv_source_ref(L, o) :
				 meta_source_ref(L, o);
  MetaSourceRef kref = meta_source_ref(L, k);
  MetaOutputRef outref = meta_output_ref(L, out);
  TValue *receiver, *key, *result, *method;
  cTValue *ret;
  int capture;
  int loop;

  meta_chain_roots_init(L, &roots);
  capture = meta_chain_capture_inputs(L, &roots, &oref, &kref);
  if (LJ_UNLIKELY(capture != META_CAPTURE_VALID)) {
    ret = meta_chain_value_publish(L, &roots, META_CHAIN_RESULT, &outref);
    meta_chain_roots_fini(&roots);
    return ret;
  }
  for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
    receiver = meta_chain_root(&roots, META_CHAIN_RECEIVER);
    key = meta_chain_root(&roots, META_CHAIN_KEY);
    result = meta_chain_root(&roots, META_CHAIN_RESULT);
    method = meta_chain_root(&roots, META_CHAIN_METHOD);
    if (LJ_LIKELY(tvistab(receiver))) {
      (void)lj_tab_gettv_rooted(L, receiver, key, result);
      result = meta_chain_root(&roots, META_CHAIN_RESULT);
      if (!tvisnil(result)) {
	ret = meta_chain_value_publish(L, &roots, META_CHAIN_RESULT, &outref);
	meta_chain_roots_fini(&roots);
	return ret;
      }
      receiver = meta_chain_root(&roots, META_CHAIN_RECEIVER);
      method = meta_chain_root(&roots, META_CHAIN_METHOD);
      (void)lj_meta_lookuptv(L, method, receiver, MM_index);
      method = meta_chain_root(&roots, META_CHAIN_METHOD);
      if (tvisnil(method)) {
	ret = meta_chain_value_publish(L, &roots, META_CHAIN_RESULT, &outref);
	meta_chain_roots_fini(&roots);
	return ret;
      }
    } else {
      (void)lj_meta_lookuptv(L, method, receiver, MM_index);
      method = meta_chain_root(&roots, META_CHAIN_METHOD);
      if (tvisnil(method)) {
        TValue *errslot = meta_chain_error_operand(L, &roots, &oref, &kref);
        meta_chain_roots_fini(&roots);
        lj_err_optype(L, errslot, LJ_ERR_OPINDEX);
        return NULL;  /* unreachable */
      }
    }
    receiver = meta_chain_root(&roots, META_CHAIN_RECEIVER);
    key = meta_chain_root(&roots, META_CHAIN_KEY);
    if (tvisfunc(method)) {
      meta_chain_mmcall(L, lj_cont_ra, method, receiver, key);
      meta_chain_roots_fini(&roots);
      return NULL;  /* Trigger metamethod call. */
    }
    copyTVrel(L, receiver, method);
    lj_gc_pubroot(L, receiver);
  }
  meta_chain_roots_fini(&roots);
  lj_err_msg(L, LJ_ERR_GETLOOP);
  return NULL;  /* unreachable */
}

/* Explicit-output C helper. `out` must be an enumerated root and may alias
** either input; stack offsets are captured before any allocation/retry. */
cTValue *lj_meta_tgettv_rooted(lua_State *L, cTValue *o, cTValue *k,
				TValue *out)
{
  return meta_tget_rooted_mode(L, o, k, out, 0);
}

/* Global-bytecode variant. `fnroot` is the authoritative frame function
** TValue; its mutable environment child is admitted inside source SMR instead
** of being copied into transient VM scratch before anchor initialization. */
cTValue *lj_meta_tgetenv_rooted(lua_State *L, cTValue *fnroot, cTValue *k,
				       TValue *out)
{
  return meta_tget_rooted_mode(L, fnroot, k, out, 1);
}

/* VM-only compatibility ABI: the assembler saves the next PC before calling,
** so pc[-1].A identifies the authoritative bytecode destination. */
cTValue *lj_meta_tget(lua_State *L, cTValue *o, cTValue *k)
{
  const BCIns *pc = cframe_Lpc(L) - 1;
  return meta_tget_rooted_mode(L, o, k, L->base + bc_a(*pc), 0);
}

static TValue *meta_tset_rooted_mode(lua_State *L, cTValue *o, cTValue *k,
				     GCtab **owner,
				     MetaChainRoots *kept_roots, int funcenv)
{
  MetaChainRoots local_roots;
  MetaChainRoots *roots = kept_roots ? kept_roots : &local_roots;
  MetaSourceRef oref = funcenv ? meta_funcenv_source_ref(L, o) :
				 meta_source_ref(L, o);
  MetaSourceRef kref = meta_source_ref(L, k);
  TValue *receiver, *key, *result, *method;
  int capture;
  int loop;
  if (owner)
    *owner = NULL;

  meta_chain_roots_init(L, roots);
  capture = meta_chain_capture_inputs(L, roots, &oref, &kref);
  if (LJ_UNLIKELY(capture == META_CAPTURE_RECEIVER_STALE)) {
    TValue *errslot = meta_stale_error_operand(L, &oref, &kref);
    meta_chain_roots_fini(roots);
    lj_err_optype(L, errslot, LJ_ERR_OPINDEX);
    return NULL;  /* unreachable */
  }
  if (LJ_UNLIKELY(capture == META_CAPTURE_KEY_STALE)) {
    meta_chain_roots_fini(roots);
    lj_err_msg(L, LJ_ERR_NILIDX);
    return NULL;  /* unreachable */
  }
  for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
    receiver = meta_chain_root(roots, META_CHAIN_RECEIVER);
    key = meta_chain_root(roots, META_CHAIN_KEY);
    result = meta_chain_root(roots, META_CHAIN_RESULT);
    method = meta_chain_root(roots, META_CHAIN_METHOD);
    if (LJ_LIKELY(tvistab(receiver))) {
      GCtab *t = tabV(receiver);
      /* The copied helper retains exact table/key leases through hashing and
      ** current-generation miss resolution. Use its value only for metamethod
      ** semantics; resolve a fresh current write slot afterwards, since a raw
      ** decision-time vector pointer cannot survive resize or reclamation. */
      (void)lj_tab_gettv_rooted(L, receiver, key, result);
      receiver = meta_chain_root(roots, META_CHAIN_RECEIVER);
      key = meta_chain_root(roots, META_CHAIN_KEY);
      result = meta_chain_root(roots, META_CHAIN_RESULT);
      t = tabV(receiver);
      if (LJ_LIKELY(!tvisnil(result))) {
	TValue *dst = lj_tab_set(L, t, key);
	lj_tab_nomm_rel(t, 0);  /* Invalidate negative metamethod cache. */
	lj_gc2_barrier_weak_key(L, t, key);
	lj_gc_pubtab(L, t);
	if (owner)
	  *owner = t;
	if (!kept_roots)
	  meta_chain_roots_fini(roots);
	return dst;
      }
      method = meta_chain_root(roots, META_CHAIN_METHOD);
      (void)lj_meta_lookuptv(L, method, receiver, MM_newindex);
      method = meta_chain_root(roots, META_CHAIN_METHOD);
      if (tvisnil(method)) {
	lj_tab_nomm_rel(t, 0);  /* Invalidate negative metamethod cache. */
	lj_gc_pubtab(L, t);
	if (tvisnil(key)) {
	  meta_chain_roots_fini(roots);
	  lj_err_msg(L, LJ_ERR_NILIDX);
	} else if (tvisnum(key) && tvisnan(key)) {
	  meta_chain_roots_fini(roots);
	  lj_err_msg(L, LJ_ERR_NANIDX);
	}
	if (owner)
	  *owner = t;
	result = lj_tab_set(L, t, key);
	if (!kept_roots)
	  meta_chain_roots_fini(roots);
	return result;
      }
    } else {
      (void)lj_meta_lookuptv(L, method, receiver, MM_newindex);
      method = meta_chain_root(roots, META_CHAIN_METHOD);
      if (tvisnil(method)) {
        TValue *errslot = meta_chain_error_operand(L, roots, &oref, &kref);
        meta_chain_roots_fini(roots);
        lj_err_optype(L, errslot, LJ_ERR_OPINDEX);
        return NULL;  /* unreachable */
      }
    }
    receiver = meta_chain_root(roots, META_CHAIN_RECEIVER);
    key = meta_chain_root(roots, META_CHAIN_KEY);
    if (tvisfunc(method)) {
      meta_chain_mmcall(L, lj_cont_nop, method, receiver, key);
      /* L->top+2 = v filled in by caller. */
      meta_chain_roots_fini(roots);
      return NULL;  /* Trigger metamethod call. */
    }
    copyTVrel(L, receiver, method);
    lj_gc_pubroot(L, receiver);
  }
  meta_chain_roots_fini(roots);
  lj_err_msg(L, LJ_ERR_SETLOOP);
  return NULL;  /* unreachable */
}

static TValue *meta_tset(lua_State *L, cTValue *o, cTValue *k, GCtab **owner)
{
  return meta_tset_rooted_mode(L, o, k, owner, NULL, 0);
}

/* Helper for TSET*. __newindex chain and metamethod. */
TValue *lj_meta_tset(lua_State *L, cTValue *o, cTValue *k)
{
  return meta_tset(L, o, k, NULL);
}

/* C API variant that reports the resolved target table for post-store barriers. */
TValue *lj_meta_tset_owner(lua_State *L, cTValue *o, cTValue *k, GCtab **owner)
{
  return meta_tset(L, o, k, owner);
}

/* VM helper that resolves the target table, stores the value and barriers it. */
static TValue *meta_tsettv_pair_mode(lua_State *L, cTValue *o, cTValue *k,
				     cTValue *v, int funcenv)
{
  int stack_o = meta_tv_on_stack(L, o);
  int stack_k = meta_tv_on_stack(L, k);
  int stack_v = meta_tv_on_stack(L, v);
  ptrdiff_t oofs = stack_o ? savestack(L, (TValue *)(void *)o) : 0;
  ptrdiff_t kofs = stack_k ? savestack(L, (TValue *)(void *)k) : 0;
  ptrdiff_t vofs = stack_v ? savestack(L, (TValue *)(void *)v) : 0;
  for (;;) {
    MetaChainRoots roots;
    GCtab *owner = NULL;
    TValue *keyroot;
    TValue *dst;
    int rc;
    if (stack_o) o = restorestack(L, oofs);
    if (stack_k) k = restorestack(L, kofs);
    if (stack_v) v = restorestack(L, vofs);
    /*
    ** Missing-key resolution may allocate, wait for a table generation, or
    ** move the Lua stack before the value edge reaches the table. Keys are
    ** anchored by lj_tab_newkey(); the source value needs an explicit root
    ** until the CAS publishes the real edge.
    */
    lj_gc_pubroot(L, v);
    /* Keep the resolved table-valued __newindex carrier rooted through the
    ** keyed CAS and every post-store barrier. Popping it inside meta_tset()
    ** would let a peer remove the metamethod edge and reclaim owner/dst in the
    ** return-to-caller window. */
    dst = meta_tset_rooted_mode(L, o, k, &owner, &roots, funcenv);
    if (stack_o) o = restorestack(L, oofs);
    if (stack_k) k = restorestack(L, kofs);
    if (stack_v) v = restorestack(L, vofs);
    if (!dst)
      return NULL;
    keyroot = meta_chain_root(&roots, META_CHAIN_KEY);
    rc = lj_tab_trystoretv_cas_keyed(L, owner, dst, keyroot, v);
    if (rc == LJ_TAB_STORE_CAS_OK) {
      /* The keyed transaction owns weak-write exclusion through its CAS and
      ** handoff. Do not retain an outer token across its L-aware CHANGED retry. */
      lj_gc2_barrier_weak_key(L, owner, keyroot);
      lj_gc2_barrier_weak_value(L, owner, v);
      lj_gc2_barrier_tv_pair(L, owner ? obj2gco(owner) : NULL, v);
      meta_chain_roots_fini(&roots);
      return dst;
    }
    meta_chain_roots_fini(&roots);
    lj_tab_store_wait_l(L);  /* Slot became stale/FORWARD; re-resolve. */
  }
}

TValue *lj_meta_tsettv_pair(lua_State *L, cTValue *o, cTValue *k, cTValue *v)
{
  return meta_tsettv_pair_mode(L, o, k, v, 0);
}

/* Store counterpart to lj_meta_tgetenv_rooted(). */
TValue *lj_meta_tsetenvtv_pair(lua_State *L, cTValue *fnroot, cTValue *k,
				      cTValue *v)
{
  return meta_tsettv_pair_mode(L, fnroot, k, v, 1);
}

static cTValue *str2num(cTValue *o, TValue *n)
{
  if (tvisnum(o))
    return o;
  else if (tvisint(o))
    return (setnumV(n, (lua_Number)intV(o)), n);
  else if (tvisstr(o) && lj_strscan_num(strV(o), n))
    return n;
  else
    return NULL;
}

#if LJ_HASBUFFER
static int meta_buf_owner_valid(global_State *g, lua_State *owner)
{
  return owner != NULL &&
	 (owner == mainthread_acq(g) ||
	  lj_state_thread_registry_valid(g, owner));
}

static SBufExt *meta_buf_sbx(lua_State *L, cTValue *o, LJGC2Lease *lease)
{
  GCobj *gco;
  GCudata *ud;
  SBufExt *sbx;
  GCSize flags;
  lua_State *owner;
  if (!lease)
    return NULL;
  memset(lease, 0, sizeof(*lease));
  if (!tvisudata(o))
    return NULL;
  gco = gcval(o);
  /*
  ** A racy snapshot can retain a userdata tag after the cell was retired and
  ** reused. Validate the cell and the fixed buffer payload shape before reading
  ** SBufExt fields; plain tvisbuf() only checks the published udtype byte.
  */
  if (lj_gc2_obj_lease_acquire(G(L), gco, (uint32_t)~LJ_TUDATA,
	NULL, lease) < 0)
    return NULL;
  ud = &gco->ud;
  if ((MSize)la_load32_acq(&ud->len) != sizeof(SBufExt) ||
      lj_udata_udtype_acq(ud) != UDTYPE_BUFFER)
    goto invalid;
  sbx = (SBufExt *)uddata(ud);
  flags = (GCSize)la_load64_acq(&sbx->L.ptr64);
  owner = (lua_State *)(void *)(uintptr_t)(flags & SBUF_MASK_L);
  if (!(flags & SBUF_FLAG_EXT) || !meta_buf_owner_valid(G(L), owner))
    goto invalid;
  return sbx;
invalid:
  lj_gc2_lease_release(lease);
  return NULL;
}

typedef struct MetaBufSnap {
  char *b;
  char *e;
  char *r;
  char *w;
  GCobj *ref;
  GCSize flags;
} MetaBufSnap;

static void meta_buf_snapshot(const SBufExt *sbx, MetaBufSnap *s)
{
  s->flags = (GCSize)la_load64_acq(&sbx->L.ptr64);
  s->b = lj_buf_bptr_acq((const SBuf *)sbx);
  s->e = lj_buf_eptr_acq((const SBuf *)sbx);
  s->r = lj_buf_rptr_acq(sbx);
  s->w = lj_buf_wptr_acq((const SBuf *)sbx);
  s->ref = lj_bufx_cowref_acq(sbx);
}

static int meta_buf_snapshot_same(const MetaBufSnap *a,
				  const MetaBufSnap *b)
{
  return a->flags == b->flags && a->b == b->b && a->e == b->e &&
    a->r == b->r && a->w == b->w && a->ref == b->ref;
}

static int meta_buf_snapshot_valid(global_State *g, const MetaBufSnap *s,
				   MSize *lenp)
{
  uintptr_t ur = (uintptr_t)(void *)s->r;
  uintptr_t uw = (uintptr_t)(void *)s->w;
  lua_State *owner = (lua_State *)(void *)(uintptr_t)
    (s->flags & SBUF_MASK_L);
  if (!(s->flags & SBUF_FLAG_EXT) || !meta_buf_owner_valid(g, owner) ||
      !lj_buf_ptr_range(s->r, s->b, s->e) ||
      !lj_buf_ptr_range(s->w, s->b, s->e) || ur > uw)
    return 0;
  *lenp = (MSize)(uw - ur);
  return 1;
}

static int meta_buf_data(lua_State *L, cTValue *o, const char **pp,
			 MSize *lenp, LJGC2Lease *holdp)
{
  LJGC2Lease bodylease;
  SBufExt *sbx;
  uint32_t attempt;
  if (holdp)
    memset(holdp, 0, sizeof(*holdp));
  sbx = meta_buf_sbx(L, o, &bodylease);
  if (!sbx)
    return 0;

  /* Individual SBuf fields are atomic, but they are not one transaction.
  ** Admit the storage selected by a first snapshot, then require the complete
  ** descriptor (including COW identity) to match a second snapshot. If a
  ** writer moved away and back, the retained allocation at the repeated
  ** address is still the exact storage named by the accepted descriptor. */
  for (attempt = 0; attempt < 8u; attempt++) {
    LJGC2Lease storage;
    MetaBufSnap first, second;
    MSize len;
    uint32_t gct = 0;
    memset(&storage, 0, sizeof(storage));
    meta_buf_snapshot(sbx, &first);
    if (!meta_buf_snapshot_valid(G(L), &first, &len))
      continue;
    if (len != 0) {
      if (first.flags & SBUF_FLAG_COW) {
	/* Stock string.buffer COW storage is owned by a string or cdata edge.
	** A cdata pointer may intentionally name external FFI memory; retaining
	** the cdata preserves runtime identity but cannot validate user memory. */
	if (first.ref == NULL ||
	    lj_gc2_obj_lease_acquire(G(L), first.ref, 0, &gct, &storage) < 0)
	  continue;
	if (gct != (uint32_t)~LJ_TSTR
#if LJ_HASFFI
	    && gct != (uint32_t)~LJ_TCDATA
#endif
	   ) {
	  lj_gc2_lease_release(&storage);
	  continue;
	}
      } else if (first.b == NULL ||
		 lj_gc2_mem_lease_acquire(G(L), first.b, &storage) < 0) {
	continue;
      }
    }
    meta_buf_snapshot(sbx, &second);
    if (!meta_buf_snapshot_same(&first, &second) ||
	!meta_buf_snapshot_valid(G(L), &second, &len)) {
      lj_gc2_lease_release(&storage);
      continue;
    }
    if ((second.flags & SBUF_FLAG_COW) &&
	gct == (uint32_t)~LJ_TSTR) {
      GCstr *s = gco2str(second.ref);
      if (second.b != (char *)strdata(s) ||
	  (uintptr_t)(void *)second.e - (uintptr_t)(void *)second.b !=
	    (uintptr_t)s->len) {
	lj_gc2_lease_release(&storage);
	continue;
      }
    }
    if (pp) *pp = second.r ? second.r : "";
    if (lenp) *lenp = len;
    lj_gc2_lease_release(&bodylease);
    if (holdp) {
      *holdp = storage;
      memset(&storage, 0, sizeof(storage));
    }
    lj_gc2_lease_release(&storage);
    return 1;
  }

  lj_gc2_lease_release(&bodylease);
  return 0;
}

/* Copy one shared buffer without a throwing/growing operation while its body
** lease is live. Capacity growth happens after release, then the source is
** resnapshotted and re-admitted. The loop is bounded so a racing writer cannot
** turn concatenation into a wait; an unstable buffer contributes an empty
** racy snapshot, which is memory-safe and within the allowed racy semantics. */
static int meta_buf_putmem(lua_State *L, SBuf *sb, cTValue *o)
{
  uint32_t attempt;
  for (attempt = 0; attempt < 8u; attempt++) {
    LJGC2Lease lease;
    const char *p;
    char *w;
    MSize len;
    memset(&lease, 0, sizeof(lease));
    if (!meta_buf_data(L, o, &p, &len, &lease))
      return 1;  /* Safe empty result for an unstable racy descriptor. */
    if (len > sbufleft(sb)) {
      lj_gc2_lease_release(&lease);
      (void)lj_buf_more(sb, len);  /* May throw only with no source lease. */
      continue;
    }
    w = lj_buf_wptr_acq(sb);
    w = lj_buf_wmem(w, p, len);
    lj_buf_wptr_rel(sb, w);
    lj_gc2_lease_release(&lease);
    return 1;
  }
  return 1;
}
#else
#define meta_buf_data(L, o, pp, lenp, holdp)	0
#define meta_buf_putmem(L, sb, o)		0
#endif

static LJ_AINLINE int meta_cat_compat(lua_State *L, cTValue *o)
{
  return tvisstr(o) || tvisnumber(o) ||
	 meta_buf_data(L, o, NULL, NULL, NULL);
}

/* Helper for arithmetic instructions. Coercion, metamethod. */
TValue *lj_meta_arith(lua_State *L, TValue *ra, cTValue *rb, cTValue *rc,
		      BCReg op)
{
  MMS mm = bcmode_mm(op);
  TValue tempb, tempc, motv;
  cTValue *b, *c;
  if ((b = str2num(rb, &tempb)) != NULL &&
      (c = str2num(rc, &tempc)) != NULL) {  /* Try coercion first. */
    setnumV(ra, lj_vm_foldarith(numV(b), numV(c), (int)mm-MM_add));
    return NULL;
  } else {
    cTValue *mo = lj_meta_lookuptv(L, &motv, rb, mm);
    if (tvisnil(mo)) {
      mo = lj_meta_lookuptv(L, &motv, rc, mm);
      if (tvisnil(mo)) {
	if (str2num(rb, &tempb) == NULL) rc = rb;
	lj_err_optype(L, rc, LJ_ERR_OPARITH);
	return NULL;  /* unreachable */
      }
    }
    return mmcall(L, lj_cont_ra, mo, rb, rc);
  }
}

/* Helper for CAT. Coercion, iterative concat, __concat metamethod. */
TValue *lj_meta_cat(lua_State *L, TValue *top, int left)
{
  TValue motv;
  int fromc = 0;
  if (left < 0) { left = -left; fromc = 1; }
  do {
    /*
    ** CAT keeps its operands in a VM stack window above the result slot. The
    ** buffer growth/string allocation below can run the collector in this fork,
    ** so publish that operand window as stack roots before inspecting/copying
    ** string payloads.
    */
    if (!fromc && L->top < top+1)
      L->top = top+1;
    if (!meta_cat_compat(L, top) || !meta_cat_compat(L, top-1)) {
      TValue *metafunc, *metaarg1, *metaarg2;
      cTValue *mo = lj_meta_lookuptv(L, &motv, top-1, MM_concat);
      if (tvisnil(mo)) {
	mo = lj_meta_lookuptv(L, &motv, top, MM_concat);
	if (tvisnil(mo)) {
	  if (tvisstr(top-1) || tvisnumber(top-1)) top++;
	  lj_err_optype(L, top-1, LJ_ERR_OPCAT);
	  return NULL;  /* unreachable */
	}
      }
      /* One of the top two elements is not a string, call __cat metamethod:
      **
      ** before:    [...][CAT stack .........................]
      **                                 top-1     top         top+1 top+2
      ** pick two:  [...][CAT stack ...] [o1]      [o2]
      ** setup mm:  [...][CAT stack ...] [cont|?]  [mo|tmtype] [o1]  [o2]
      ** in asm:    [...][CAT stack ...] [cont|PC] [mo|delta]  [o1]  [o2]
      **            ^-- func base                              ^-- mm base
      ** after mm:  [...][CAT stack ...] <--push-- [result]
      ** next step: [...][CAT stack .............]
      */
      metafunc = top+LJ_FR2;
      metaarg1 = top+2*LJ_FR2+1;
      metaarg2 = top+2*LJ_FR2+2;
      copyTV(L, metaarg2, top);  /* Carefully ordered stack copies! */
      copyTV(L, metaarg1, top-1);
      copyTV(L, metafunc, mo);
      setcont(top-1, lj_cont_cat);
      if (LJ_FR2) { setnilV(top); setnilV(top+2); top += 2; }
      lj_state_stack_pubtv(L, L, metafunc);
      lj_state_stack_pubtv(L, L, metaarg1);
      lj_state_stack_pubtv(L, L, metaarg2);
      return top+1;  /* Trigger metamethod call. */
    } else {
      /* Pick as many strings as possible from the top and concatenate them:
      **
      ** before:    [...][CAT stack ...........................]
      ** pick str:  [...][CAT stack ...] [...... strings ......]
      ** concat:    [...][CAT stack ...] [result]
      ** next step: [...][CAT stack ............]
      */
      TValue *e = top, *o = top, *r;
      ptrdiff_t topofs;
      MSize blen;
      uint64_t tlen = 0;
      SBuf *sb;
      GCstr *s;
      do {
	o--;
      } while (--left > 0 && meta_cat_compat(L, o-1));
      for (r = o; r <= e; r++)
	lj_gc_pubroot(L, r);
      for (r = o; r <= e; r++) {
	if (tvisstr(r)) {
	  tlen += strV(r)->len;
	} else if (tvisnumber(r)) {
	  tlen += STRFMT_MAXBUF_NUM;
	} else {
	  /* Compatibility classified this operand as a buffer. An unstable racy
	  ** descriptor contributes an empty snapshot, not a numeric type-pun. */
	  blen = 0;
	  (void)meta_buf_data(L, r, NULL, &blen, NULL);
	  tlen += blen;
	}
      }
      if (tlen >= LJ_MAX_STR) lj_err_msg(L, LJ_ERR_STROV);
      sb = lj_buf_tmp_(L);
      lj_buf_more(sb, (MSize)tlen);
      for (e = top, top = o; o <= e; o++) {
	MSize len;
	if (tvisstr(o)) {
	  GCstr *s = strV(o);
	  len = s->len;
	  lj_buf_putmem(sb, strdata(s), len);
	} else if (tvisint(o)) {
	  lj_strfmt_putint(sb, intV(o));
	} else if (tvisnum(o)) {
	  lj_strfmt_putfnum(sb, STRFMT_G14, numV(o));
	} else {
	  (void)meta_buf_putmem(L, sb, o);
	}
      }
      topofs = savestack(L, top);
      s = lj_buf_str(L, sb);
      top = restorestack(L, topofs);
      setstrV(L, top, s);
      lj_state_stack_pubtv(L, L, top);
    }
  } while (left >= 1);
  if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) {
    if (!fromc) L->top = curr_topL(L);
    lj_gc_step_top(L);
  }
  return NULL;
}

/* Helper for LEN. __len metamethod. */
TValue * LJ_FASTCALL lj_meta_len(lua_State *L, cTValue *o)
{
  TValue motv;
  cTValue *mo = lj_meta_lookuptv(L, &motv, o, MM_len);
  if (tvisnil(mo)) {
    if (!(LJ_52 && tvistab(o)))
      lj_err_optype(L, o, LJ_ERR_OPLEN);
    return NULL;
  }
  return mmcall(L, lj_cont_ra, mo, o, LJ_52 ? o : niltv(L));
}

/* Helper for equality comparisons. __eq metamethod. */
TValue *lj_meta_equal(lua_State *L, GCobj *o1, GCobj *o2, int ne)
{
  /* Field metatable must be at same offset for GCtab and GCudata! */
  TValue motv, motv2;
  GCtab *mt1 = lj_obj_metatable_acq(o1);
  GCtab *mt2 = lj_obj_metatable_acq(o2);
  cTValue *mo = lj_meta_fasttv(G(L), mt1, MM_eq, &motv);
  if (mo) {
    TValue *top;
    uint32_t it;
    if (mt1 != mt2) {
      cTValue *mo2 = lj_meta_fasttv(G(L), mt2, MM_eq, &motv2);
      if (mo2 == NULL || !lj_obj_equal(mo, mo2))
	return (TValue *)(intptr_t)ne;
    }
    top = curr_top(L);
    setcont(top++, ne ? lj_cont_condf : lj_cont_condt);
    if (LJ_FR2) setnilV(top++);
    copyTV(L, top++, mo);
    if (LJ_FR2) setnilV(top++);
    it = ~(uint32_t)o1->gch.gct;
    setgcV(L, top, o1, it);
    setgcV(L, top+1, o2, it);
    return top;  /* Trigger metamethod call. */
  }
  return (TValue *)(intptr_t)ne;
}

#if LJ_HASFFI
TValue * LJ_FASTCALL lj_meta_equal_cd(lua_State *L, BCIns ins)
{
  ASMFunction cont = (bc_op(ins) & 1) ? lj_cont_condf : lj_cont_condt;
  int op = (int)bc_op(ins) & ~1;
  TValue tv, motv;
  cTValue *mo, *o2, *o1 = &L->base[bc_a(ins)];
  cTValue *o1mm = o1;
  if (op == BC_ISEQV) {
    o2 = &L->base[bc_d(ins)];
    if (!tviscdata(o1mm)) o1mm = o2;
  } else if (op == BC_ISEQS) {
    setstrV(L, &tv,
	    gco2str(proto_kgc_acq(curr_proto(L), ~(ptrdiff_t)bc_d(ins))));
    o2 = &tv;
  } else if (op == BC_ISEQN) {
    proto_knumtv_load_acq(&tv, curr_proto(L), bc_d(ins));
    o2 = &tv;
  } else {
    lj_assertL(op == BC_ISEQP, "bad bytecode op %d", op);
    setpriV(&tv, ~bc_d(ins));
    o2 = &tv;
  }
  mo = lj_meta_lookuptv(L, &motv, o1mm, MM_eq);
  if (LJ_LIKELY(!tvisnil(mo)))
    return mmcall(L, cont, mo, o1, o2);
  else
    return (TValue *)(intptr_t)(bc_op(ins) & 1);
}
#endif

/* Helper for ordered comparisons. String compare, __lt/__le metamethods. */
TValue *lj_meta_comp(lua_State *L, cTValue *o1, cTValue *o2, int op)
{
  lj_gc_pubroot(L, o1);
  lj_gc_pubroot(L, o2);
  if (LJ_HASFFI && (tviscdata(o1) || tviscdata(o2))) {
    ASMFunction cont = (op & 1) ? lj_cont_condf : lj_cont_condt;
    MMS mm = (op & 2) ? MM_le : MM_lt;
    TValue motv;
    cTValue *mo = lj_meta_lookuptv(L, &motv, tviscdata(o1) ? o1 : o2, mm);
    if (LJ_UNLIKELY(tvisnil(mo))) goto err;
    return mmcall(L, cont, mo, o1, o2);
  } else if (LJ_52 || itype(o1) == itype(o2)) {
    /* Never called with two numbers. */
    if (tvisstr(o1) && tvisstr(o2)) {
      int32_t res = lj_str_cmp(strV(o1), strV(o2));
      return (TValue *)(intptr_t)(((op&2) ? res <= 0 : res < 0) ^ (op&1));
    } else {
    trymt:
      while (1) {
	ASMFunction cont = (op & 1) ? lj_cont_condf : lj_cont_condt;
	MMS mm = (op & 2) ? MM_le : MM_lt;
	TValue motv;
	cTValue *mo = lj_meta_lookuptv(L, &motv, o1, mm);
#if LJ_52
	if (tvisnil(mo) &&
	    tvisnil((mo = lj_meta_lookuptv(L, &motv, o2, mm))))
#else
	TValue motv2;
	cTValue *mo2 = lj_meta_lookuptv(L, &motv2, o2, mm);
	if (tvisnil(mo) || !lj_obj_equal(mo, mo2))
#endif
	{
	  if (op & 2) {  /* MM_le not found: retry with MM_lt. */
	    cTValue *ot = o1; o1 = o2; o2 = ot;  /* Swap operands. */
	    op ^= 3;  /* Use LT and flip condition. */
	    continue;
	  }
	  goto err;
	}
	return mmcall(L, cont, mo, o1, o2);
      }
    }
  } else if (tvisbool(o1) && tvisbool(o2)) {
    goto trymt;
  } else {
  err:
    lj_err_comp(L, o1, o2);
    return NULL;
  }
}

/* Helper for ISTYPE and ISNUM. Implicit coercion or error. */
void lj_meta_istype(lua_State *L, BCReg ra, BCReg tp)
{
  L->top = curr_topL(L);
  ra++; tp--;
  lj_assertL(LJ_DUALNUM || tp != ~LJ_TNUMX, "bad type for ISTYPE");
  if (LJ_DUALNUM && tp == ~LJ_TNUMX) lj_lib_checkint(L, ra);
  else if (tp == ~LJ_TNUMX+1) lj_lib_checknum(L, ra);
  else if (tp == ~LJ_TSTR) lj_lib_checkstr(L, ra);
  else lj_err_argtype(L, ra, lj_obj_itypename[tp]);
}

/* Helper for calls. __call metamethod. */
void lj_meta_call(lua_State *L, TValue *func, TValue *top)
{
  TValue motv;
  cTValue *mo = lj_meta_lookuptv(L, &motv, func, MM_call);
  TValue *p;
  lj_gc_pubroot(L, func);
  lj_gc_pubroot(L, mo);
  if (!tvisfunc(mo))
    lj_err_optype_call(L, func);
  for (p = top; p > func+2*LJ_FR2; p--) {
    copyTV(L, p, p-1);
    lj_state_stack_pubtv(L, L, p);
  }
  if (LJ_FR2) {
    copyTV(L, func+2, func);
    lj_state_stack_pubtv(L, L, func+2);
  }
  copyTV(L, func, mo);
  lj_state_stack_pubtv(L, L, func);
}

/* Helper for FORI. Coercion. */
void LJ_FASTCALL lj_meta_for(lua_State *L, TValue *o)
{
  if (!lj_strscan_numberobj(o)) lj_err_msg(L, LJ_ERR_FORINIT);
  if (!lj_strscan_numberobj(o+1)) lj_err_msg(L, LJ_ERR_FORLIM);
  if (!lj_strscan_numberobj(o+2)) lj_err_msg(L, LJ_ERR_FORSTEP);
  if (LJ_DUALNUM) {
    /* Ensure all slots are integers or all slots are numbers. */
    int32_t k[3];
    int nint = 0;
    ptrdiff_t i;
    for (i = 0; i <= 2; i++) {
      if (tvisint(o+i)) {
	k[i] = intV(o+i); nint++;
      } else {
	int64_t i64;
	if (lj_num2int_check(numV(o+i), i64, k[i])) nint++;
      }
    }
    if (nint == 3) {  /* Narrow to integers. */
      setintV(o, k[0]);
      setintV(o+1, k[1]);
      setintV(o+2, k[2]);
    } else if (nint != 0) {  /* Widen to numbers. */
      if (tvisint(o)) setnumV(o, (lua_Number)intV(o));
      if (tvisint(o+1)) setnumV(o+1, (lua_Number)intV(o+1));
      if (tvisint(o+2)) setnumV(o+2, (lua_Number)intV(o+2));
    }
  }
}
