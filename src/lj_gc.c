/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_chan.h"
#include "lj_err.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_debug.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_mcode.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#endif
#include "lj_trace.h"
#include "lj_arena.h"

#define GCACTIVEAUTOSTEPS	64u

static GCSize gc_step_debt_quantum(global_State *g)
{
  /*
  ** Idle threshold publication stays close to the GC2 allocation trigger so a
  ** cycle starts promptly. Once GC2 is active, automatic allocation checks are
  ** only a bounded progress hook; use a larger quantum so mark fixpoint root
  ** snapshots are not retried at the idle trigger cadence.
  */
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return (GCSize)LJ_GC2_ACTIVE_AUTO_STEP;
  return (GCSize)LJ_GC2_HELPER_IDLE_STEP;
}

static int gc_hard_assist_due_interp(global_State *g, TGState *tg)
{
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  return !tg || lj_gc2_hard_load(g) < LJ_GC2_ACCT_FLUSH ||
	 lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH;
}

static int gc_logical_running(global_State *g)
{
  GCSize threshold;
  if (mt_live_acq(g) != 0) {
    threshold = lj_gc_mt_threshold_load(g);
    if (mt_live_acq(g) == 0)
      threshold = lj_gc_threshold_load(g);
  } else {
    threshold = lj_gc_threshold_load(g);
  }
  return threshold != LJ_MAX_MEM;
}

#if LJ_HASJIT
static int gc_hard_assist_due_jit(global_State *g)
{
  uint64_t hard, since;
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  hard = lj_gc2_hard_load(g);
  if (hard < LJ_GC2_ACCT_FLUSH)
    return 1;
  since = lj_gc2_alloc_since_load(g);
  return since >= lj_gc2_hard_check_load(g);
}
#endif

void lj_gc_fixstring(global_State *g, GCstr *s)
{
  GCobj *o;
  if (!s)
    return;
  o = obj2gco(s);
  lj_obj_addgcflags_atomic(o, LJ_GC_FIXED);
  /*
  ** Fixed strings can be referenced outside the string table by lexer token
  ** state, preallocated error paths and FFI CType names. Mark at the creation
  ** edge so GC2 does not need to rescan the whole string hash table after every
  ** fixpoint handshake to protect late fixed strings.
  */
  if (g) {
    lj_gc_arena_markobj(g, o);
    (void)lj_gc2_markobj_nogrey(g, o);
  }
}

#define isfinalized(u)		(lj_obj_gcflags(obj2gco(u)) & LJ_GC_FINALIZED)

static void gc_root_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

static int gc_root_link_valid(global_State *g, GCobj *o);

/* -- GC2 mark bridge ----------------------------------------------------- */

void lj_gc_arena_markobj(global_State *g, GCobj *o)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markobj_direct(g, o);
}

void lj_gc_arena_markmem(global_State *g, void *p)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markmem_registered(g, p);
}

void lj_gc_arena_markmem_registered(global_State *g, void *p)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markmem_registered(g, p);
}

static void gc2_preserve_root_spine_body(global_State *g, GCobj *o)
{
  if (o && o->gch.gct == (uint32_t)~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    TValue *array = NULL;
    MSize asize = 0, acap = 0, hmask = 0;
    Node *node = NULL;
    if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) ==
	LJ_TAB_GC_SNAPSHOT_OK && array)
      (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				 (void *)array);
    UNUSED(asize);
    if (lj_tab_node_snapshot_gc(g, t, &node, &hmask) ==
	LJ_TAB_GC_SNAPSHOT_OK && hmask > 0)
      (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  }
}

void lj_gc_preserve_root_chain_for_gc2_sweep(global_State *g)
{
  GCobj *o;
  uint32_t n = 0;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  /*
  ** The root list is still the GC root ownership spine while GC2 owns arena
  ** storage, but it is not a semantic root set. Semantic roots are closed by
  ** lj_gc2_trace_sweep_roots(); the arena owner sweep dispatches or preserves
  ** unmarked valid root-spine bodies at the bridge boundary. Do not mark child
  ** values here, but keep side-vector storage for root-spine table bodies alive
  ** for the same bridge interval as the table bodies themselves.
  */
  for (o = lj_gc_root_acq(g); o != NULL;) {
    GCobj *next;
    if (!gc_root_link_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    gc2_preserve_root_spine_body(g, o);
    if (next == o || ++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
    o = next;
  }
}

int lj_gc_tv_gcref_valid(global_State *g, cTValue *tv)
{
  return lj_gc2_tv_gcref_valid_edge(g, tv);
}

static int gc_objroot_gct_valid(global_State *g, GCobj *o, uint32_t *gctp)
{
  uint32_t gct;
  if (!g || !o || !checkptrGC(o))
    return 0;
  if (!lj_gc2_obj_valid_queued(g, o)) {
    /*
    ** Strings may be plain allocations rather than traversable object cells.
    ** For other object roots, queued-object validation must prove the cell
    ** before the header drives the synthetic TValue tag. The empty string is
    ** embedded in global_State and is a valid string root.
    */
    if (o != obj2gco(&g->strempty) && !lj_gc2_mem_registered_known(g, o))
      return 0;
    gct = o->gch.gct;
    if (gct != (uint32_t)~LJ_TSTR)
      return 0;
  } else {
    gct = o->gch.gct;
  }
  if (gct == 0 || gct < (uint32_t)~LJ_TSTR ||
      gct > (uint32_t)~LJ_TUDATA)
    return 0;
  if (gctp)
    *gctp = gct;
  return 1;
}

static MSize gc_udata_io_file_size(void)
{
  /*
  ** UDTYPE_IO_FILE has a private payload in the I/O library:
  ** { FILE *fp; uint32_t type; }.  The collector only needs the allocation
  ** size, and FILE is stored as an opaque pointer there.
  */
  return (MSize)((sizeof(void *) + sizeof(uint32_t) + sizeof(void *) - 1u) &
		 ~(sizeof(void *) - 1u));
}

int lj_gc_udata_payload_valid(GCudata *ud, GCSize *sizep)
{
  MSize len;
  GCSize size;
  uint8_t udtype;
  if (!ud || ud->gct != ~LJ_TUDATA)
    return 0;
  len = ud->len;
  if (len > LJ_MAX_UDATA || len > LJ_MAX_MEM32 - sizeof(GCudata))
    return 0;
  udtype = lj_udata_udtype_acq(ud);
  if (udtype >= UDTYPE__MAX)
    return 0;
  switch (udtype) {
  case UDTYPE_USERDATA:
    break;
  case UDTYPE_IO_FILE:
    if (len != gc_udata_io_file_size())
      return 0;
    break;
  case UDTYPE_FFI_CLIB:
#if LJ_HASFFI
    if (len != sizeof(CLibrary))
      return 0;
    break;
#else
    return 0;
#endif
  case UDTYPE_BUFFER:
#if LJ_HASBUFFER
    if (len != sizeof(SBufExt))
      return 0;
    break;
#else
    return 0;
#endif
  case UDTYPE_CHANNEL:
    {
      LJChan *ch = (LJChan *)uddata(ud);
      uint32_t cap, mask, rendezvous;
      uint64_t bytes;
      if (len < sizeof(LJChan))
	return 0;
      cap = la_load32_acq(&ch->cap);
      mask = la_load32_acq(&ch->mask);
      rendezvous = la_load32_acq(&ch->rendezvous);
      if (cap == 0 || (cap & (cap - 1u)) != 0 || mask != cap - 1u ||
	  rendezvous > 1u)
	return 0;
      bytes = sizeof(LJChan) + ((uint64_t)cap - 1u) * sizeof(LJChanSlot);
      if (bytes > LJ_MAX_UDATA || len != (MSize)bytes)
	return 0;
    }
    break;
  case UDTYPE_THREAD:
    {
      LJThread *th = (LJThread *)uddata(ud);
      if (len != sizeof(LJThread) ||
	  lj_thread_udata_acq(th) != ud)
	return 0;
    }
    break;
  case UDTYPE_MUTEX:
    if (len != sizeof(LJMutex))
      return 0;
    break;
  default:
    return 0;
  }
  size = sizeof(GCudata) + (GCSize)len;
  if (sizep)
    *sizep = size;
  return 1;
}

static int gc_chain_splice(GCRef *p, GCobj *o)
{
  GCobj *next = lj_obj_gcw_acq(o);
  GCobj *expect = o;
  return gcref_cas(p, &expect, next);
}

static int gc_ref_cas_obj(GCRef *p, GCobj *old, GCobj *next)
{
  return gcref_cas(p, &old, next);
}

static int gc_root_link_valid(global_State *g, GCobj *o)
{
  GCobj *th;
  if (o == NULL)
    return 0;
  th = gcref_acq(*mainthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  th = gcref_acq(*vmthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  return lj_gc2_obj_valid(g, o);
}

static int gc_root_chain_break_cycle(global_State *g, GCobj *head)
{
  GCobj *slow = head, *fast = head, *entry, *tail, *next;
  while (fast != NULL) {
    if (!gc_root_link_valid(g, slow) || !gc_root_link_valid(g, fast))
      return 0;
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
    if (fast == NULL || slow == NULL)
      return 0;
    if (!gc_root_link_valid(g, fast))
      return 0;
    fast = lj_obj_gcw_acq(fast);
    if (slow == fast)
      break;
  }
  if (fast == NULL)
    return 0;
  slow = head;
  while (slow != fast) {
    if (!gc_root_link_valid(g, slow) || !gc_root_link_valid(g, fast))
      return 0;
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
  }
  entry = slow;
  tail = entry;
  /*
  ** Arena cells can be reused while an old root-spine entry for the same
  ** address is still visible to lock-free publishers. Splicing a pending chain
  ** that contains the reused address back to the old spine forms
  ** head..tail -> oldhead..entry. Sever the cycle predecessor with the same
  ** GCRef CAS discipline used by root unlinking, preserving each unique object
  ** reachable from head exactly once.
  */
  while ((next = lj_obj_gcw_acq(tail)) != entry) {
    if (next == NULL || !gc_root_link_valid(g, next))
      return 0;
    tail = next;
  }
  while (lj_obj_gcw_acq(tail) == entry) {
    if (gc_ref_cas_obj(lj_obj_gcwref(tail), entry, NULL))
      return 1;
    gc_root_wait_no_l();
  }
  return 0;
}

uint32_t lj_gc_repair_root_spine(global_State *g)
{
  uint64_t epoch, repaired;
  int fixed;
  if (!g)
    return 0;
  epoch = lj_gcroot_repair_epoch_acq(g);
  repaired = lj_gcroot_repaired_epoch_acq(g);
  if (repaired == epoch)
    return 0;
  fixed = gc_root_chain_break_cycle(g, lj_gc_root_acq(g));
  /*
  ** Root-spine cycles can only be introduced by root-spine publication. The
  ** publish CAS/release store orders the links themselves; this epoch is only a
  ** conservative cache of whether a scan has already covered that publication.
  ** If another publisher bumps the epoch while this scan runs, storing the old
  ** epoch leaves the newer publication visible to the next repair.
  */
  lj_gcroot_repaired_epoch_rel(g, epoch);
  return (uint32_t)fixed;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

static int gc2_free_unmarked_obj(global_State *g, GCobj *o)
{
  uint32_t gct = o->gch.gct;
  /*
  ** Strings are owned by the intern table and swept by GC2's string-table
  ** owner (or the terminal GC2 shutdown drain).
  ** Traversable arena scans can encounter stale type bytes from reclaimed or
  ** reused bodies; dispatching those through lj_str_free() would double-count
  ** string-table ownership and corrupt g->str.num.
  */
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;
#if LJ_HASJIT
  if (gct == (uint32_t)~LJ_TTRACE)
    return lj_trace_free_gc(g, gco2trace(o));
#endif
  if (gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA) {
    GCFreeFunc fn = gc_freefunc[gct - (uint32_t)~LJ_TSTR];
    if (fn) {
      fn(g, o);
      return 1;
    }
  }
  return 0;
}

static int gc2_size_fits_mem(global_State *g, const void *p, GCSize size)
{
  GCArena *a;
  uint32_t cell, end, ncells;
  if (!g || g->allocf != lj_arena_allocf)
    return 1;
  if (!p || size > LJ_MAX_MEM32)
    return 0;
  /* Prove mapping ownership and an exact live allocation start before reading
  ** its arena header. Destructor validation can receive stale side pointers. */
  if (!lj_gc2_mem_registered_known(g, p))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    TGState *tg = lj_tg_find_owner(g, lj_arena_owner_acq(a));
    LJHugeInfo hi;
    return tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	   lj_arena_hugetab_lookup(&tg->huge, p, &hi) == 1 &&
	   !(hi.flags & LJ_HUGEF_FREEING) && size <= hi.size;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  ncells = lj_arena_ncells(size);
  /* block|mark is the allocation/free boundary map. RESET_ALLOC publishes the
  ** old bump tail before root pruning, so the next boundary is authoritative
  ** for every object whose destructor GC2 may dispatch. */
  for (end = cell + 1u; end < LJ_ARENA_CELLS; end++)
    if (lj_arena_bm_get(a->block, end) || lj_arena_bm_get(a->mark, end))
      break;
  return ncells <= end - cell;
}

static int gc2_size_fits_arena(global_State *g, GCobj *o, GCSize size)
{
  return gc2_size_fits_mem(g, o, size);
}

static int gc2_valid_func_free_obj(global_State *g, GCfunc *fn)
{
  GCSize size;
  if (!fn || !checkptrGC(fn) ||
      (((uintptr_t)fn & (uintptr_t)(sizeof(void *) - 1u)) != 0) ||
      !lj_gc2_obj_valid(g, obj2gco(fn)) || fn->c.gct != ~LJ_TFUNC)
    return 0;
  if (isluafunc(fn)) {
    uint32_t nup = lj_funcL_nupvalues(&fn->l);
    if (nup > LJ_MAX_UPVAL)
      return 0;
    size = (GCSize)sizeLfunc((MSize)nup);
  } else {
    size = (GCSize)sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  }
  return gc2_size_fits_arena(g, obj2gco(fn), size);
}

static int gc2_valid_proto_obj(global_State *g, GCproto *pt)
{
  MSize minpt;
  if (pt->sizept < sizeof(GCproto) || pt->sizept > LJ_MAX_MEM32)
    return 0;
  if (!gc2_size_fits_arena(g, obj2gco(pt), pt->sizept))
    return 0;
  if (pt->sizebc == 0 || pt->sizebc > LJ_MAX_BCINS)
    return 0;
  if (pt->framesize > LJ_MAX_SLOTS || pt->sizeuv > LJ_MAX_UPVAL)
    return 0;
  minpt = (MSize)sizeof(GCproto) + pt->sizebc*(MSize)sizeof(BCIns);
  minpt = (minpt + (MSize)sizeof(TValue)-1) & ~((MSize)sizeof(TValue)-1);
  return minpt <= pt->sizept;
}

static int gc2_valid_pow2_mask(MSize hmask)
{
  MSize hsize;
  if (hmask == 0)
    return 1;
  if (hmask > (((MSize)1u << LJ_MAX_HBITS) - 1u))
    return 0;
  hsize = hmask + 1u;
  return (hsize & hmask) == 0;
}

static int gc2_valid_tab_obj(global_State *g, GCtab *t)
{
  int8_t colo = lj_tab_colo_acq(t);
  MSize colosz = lj_tab_colo_size(t);
  MSize asize, acap;
  TValue *array;
  TValue *coloarray = (TValue *)(void *)((char *)(void *)t + sizeof(GCtab));
  Node *node;
  MSize hmask;
  GCSize bodysize;

  if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) !=
      LJ_TAB_GC_SNAPSHOT_OK ||
      lj_tab_node_snapshot_gc(g, t, &node, &hmask) !=
      LJ_TAB_GC_SNAPSHOT_OK)
    return 0;
  if (LJ_MAX_COLOSIZE != 0 && colosz > LJ_MAX_COLOSIZE)
    return 0;
  if (asize > LJ_MAX_ASIZE || acap > LJ_MAX_ASIZE)
    return 0;
  if (colo > 0) {
    if (array != coloarray || asize > colosz || lj_tab_acap_acq(t) > colosz)
      return 0;
  } else if (array == coloarray) {
    /*
    ** A negative colocated marker means a resize has split the old inline array
    ** from table indexing. A dead table still pointing at the inline storage is
    ** in a transient resize state; ordinary sweep must not free it as separated.
    */
    return 0;
  } else if (array != NULL) {
    if (!gc2_size_fits_mem(g, lj_tab_array_hdrw(array),
			   lj_tab_array_bytes(acap)))
      return 0;
  }

  if (node == NULL)
    return 0;
  if (!gc2_valid_pow2_mask(hmask))
    return 0;
  if (hmask > 0) {
    if (!gc2_size_fits_mem(g, lj_tab_node_hdrw(node),
			   lj_tab_node_bytes(hmask)))
      return 0;
  }

  bodysize = (LJ_MAX_COLOSIZE != 0 && colosz) ?
	     (GCSize)sizetabcolo(colosz) : (GCSize)sizeof(GCtab);
  return gc2_size_fits_arena(g, obj2gco(t), bodysize);
}

static int gc2_cdata_finalizer_pending(GCobj *o)
{
#if LJ_HASFFI
  return o->gch.gct == (uint32_t)~LJ_TCDATA &&
	 (lj_obj_gcflags(o) & LJ_GC_CDATA_FIN);
#else
  UNUSED(o);
  return 0;
#endif
}

static int gc2_valid_freeable_obj(global_State *g, GCobj *o)
{
  uint32_t gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;  /* String table sweep owns GCstr lifetime. */
  if (gc2_cdata_finalizer_pending(o))
    return 0;  /* FINREG dispatch must clear LJ_GC_CDATA_FIN before free. */
#if LJ_HASFFI
  if (gct == (uint32_t)~LJ_TCDATA &&
      !lj_cdata_validate(g, gco2cd(o), NULL, NULL))
    return 0;  /* Stale cdata header: ctype/size is not safe to dispatch. */
#endif
  if (gct == (uint32_t)~LJ_TPROTO && !gc2_valid_proto_obj(g, gco2pt(o)))
    return 0;  /* Stale proto header: sizept is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TFUNC && !gc2_valid_func_free_obj(g, gco2func(o)))
    return 0;  /* Free only needs a sane closure body; traversal stays strict. */
  if (gct == (uint32_t)~LJ_TTHREAD &&
      mref(gco2th(o)->glref, global_State) != g)
    return 0;  /* Stale thread header: glref is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TTAB && !gc2_valid_tab_obj(g, gco2tab(o)))
    return 0;  /* Stale table header: side-vector sizes are not trustworthy. */
  if (gct == (uint32_t)~LJ_TUDATA) {
    GCSize size;
    if (!lj_gc_udata_payload_valid(gco2ud(o), &size) ||
	!gc2_size_fits_arena(g, o, size))
      return 0;  /* Stale userdata header: payload size is not trustworthy. */
    if (lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD &&
	lj_thread_live_node_acq((LJThread *)uddata(gco2ud(o))) != NULL)
      return 0;  /* Published native root still owns this thread userdata. */
  }
  return gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA &&
	 gc_freefunc[gct - (uint32_t)~LJ_TSTR] != NULL;
}

static int gc2_deferred_body_pending(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!o || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 0;
  a = lj_arena_of(o);
  if (lj_arena_ishuge(a) || !(a->hdr.flags & LJ_AF_TRAVERSABLE))
    return 0;
  cell = lj_arena_cellof(o);
  return cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	 lj_arena_bm_get(a->block, cell);
}

void lj_gc_unlink_root_obj(global_State *g, GCobj *dead)
{
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  if (!g || !dead)
    return;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  while ((o = gcref_acq(*p)) != NULL) {
    if (o == dead) {
      if (gc_chain_splice(p, o))
	return;
      gc_root_wait_no_l();
      continue;
    }
    p = lj_obj_gcwref(o);
  }
}

static int gc_root_chain_contains_obj(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  if (!g || !needle)
    return 0;
  for (o = lj_gc_root_acq(g); o != NULL && n++ < LJ_GC2_ROOT_SCAN_LIMIT;) {
    GCobj *next;
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o)))
      break;
    if (o == needle)
      return 1;
    next = lj_obj_gcw_acq(o);
    if (next == o)
      break;
    o = next;
  }
  return 0;
}

#define GC2_ROOT_PRUNE_BATCH 256u

static void *gc2_sweep_obj_base(global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (o && o->gch.gct == (uint32_t)~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    void *base;
    if (cdataisv(cd) && lj_cdata_validate(g, cd, &base, NULL))
      return base;
  }
#else
  UNUSED(g);
#endif
  return o;
}

static int gc2_sweep_obj_old_generation(global_State *g, GCobj *o)
{
  void *base = gc2_sweep_obj_base(g, o);
  GCArena *a;
  if (!base || !checkptrGC(base))
    return 0;
  a = lj_arena_of(base);
  if (!lj_arena_ishuge(a))
    return (lj_arena_flags_acq(a) &
	    (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)) != 0;
  {
    uint32_t owner_tid = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    LJHugeInfo hi;
    return tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	   lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 1 &&
	   (hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE)) ==
	     (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE);
  }
}

/* Classify one exact small old-generation allocation after its ownership-spine
** link has been detached. LIVE means it must be reanchored after the grace;
** RETIRED means its destructor is pending. WHITE remains reserved for raw or
** fixed allocations which were never detached. */
static void gc2_sweep_detached_small(global_State *g, GCArena *a,
				     uint32_t cell, int marked)
{
  uint32_t state = lj_arena_sweep_state_acq(a, cell);
  if (marked) {
    for (;;) {
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_FREEING)
	break;
      if (lj_arena_sweep_state_cas(a, cell, state,
					   LJ_ARENA_SWEEP_LIVE)) {
	if (state == LJ_ARENA_SWEEP_RETIRED) {
	  uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	  lj_assertG(old != 0, "detached-root rescue underflow");
	  UNUSED(old);
	}
	break;
      }
      state = lj_arena_sweep_state_acq(a, cell);
    }
  } else if (state == LJ_ARENA_SWEEP_WHITE) {
    uint32_t old;
    /* Reserve the counter before publishing RETIRED. A sweep-time publisher
    ** which wins RETIRED->LIVE can then decrement without racing an increment. */
    (void)lj_arena_reclaim_deferred_add(a, 1);
    if (!lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
					  LJ_ARENA_SWEEP_RETIRED)) {
      old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "detached-root retire rollback underflow");
    } else if (lj_arena_bm_get(a->mark, cell) &&
	       lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					  LJ_ARENA_SWEEP_LIVE)) {
      old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "detached-root overlap rescue underflow");
    }
    UNUSED(old);
  }
  gc2_sweep_grace_needed_rel(g, 1);
}

static void gc2_sweep_detached_obj(global_State *g, GCobj *o, int marked)
{
  void *base = gc2_sweep_obj_base(g, o);
  GCArena *a = lj_arena_of(base);
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS)
      gc2_sweep_detached_small(g, a, cell, marked);
    return;
  }
  {
    uint32_t owner_tid = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    int ticketed;
    if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      return;
    /* One huge mapping contains one allocation. Publish an explicit metadata
    ** ticket for both dead and already-marked detached roots: RETIRED is the
    ** destructor claim, while MARK|TICKET is the post-grace reanchor claim.
    ** A concurrent marker may win either side of this call without making the
    ** exact variable-offset header undiscoverable. */
    ticketed = lj_arena_hugetab_retire(&tg->huge, base, o,
					      lj_gc2_retire_epoch(g), NULL);
    lj_assertG(ticketed, "detached huge root lost ownership ticket");
    gc2_sweep_grace_needed_rel(g, 1);
    if (!ticketed)
      return;  /* Retain the mapped body; never invent a destructor owner. */
    UNUSED(marked);
  }
}

uint32_t lj_gc_sweep_gc2_unmarked(global_State *g)
{
  GCRef *p;
  GCobj *o;
  uint32_t seen = 0, unlinked = 0;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_sweep_root_done_acq(g))
    return 0;
  p = gc2_sweep_root_cursor_acq(g);
  if (!p)
    p = lj_gc_root_ref(g);
  while (seen < GC2_ROOT_PRUNE_BATCH && (o = gcref_acq(*p)) != NULL) {
    int marked;
    seen++;
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o))) {
      /* Quarantined bodies remain mapped, but an unregistered pointer is not a
      ** cursor-safe ownership entry. Restart after spine repair in a later
      ** bounded batch instead of dereferencing an unknown successor. */
      (void)lj_gc_repair_root_spine(g);
      p = lj_gc_root_ref(g);
      break;
    }
    gc2_preserve_root_spine_body(g, o);
    if (!gc2_sweep_obj_old_generation(g, o)) {
      p = lj_obj_gcwref(o);  /* Post-reset allocation: never this sweep. */
      continue;
    }
    marked = lj_gc2_ismarked(g, o);
    if (LJ_UNLIKELY(gc2_cdata_finalizer_pending(o)) ||
	(lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))) {
      (void)lj_gc2_markmem(g, gc2_sweep_obj_base(g, o));
      p = lj_obj_gcwref(o);
      continue;
    }
    if (o->gch.gct == 0 || gc2_valid_freeable_obj(g, o)) {
#if LJ_HASJIT
      if (marked == 0 && o->gch.gct == (uint32_t)~LJ_TTRACE &&
	  !lj_trace_retire_gc_claim(g, gco2trace(o))) {
	/* The nonwaiting recorder-token attempt left trace/root/state unchanged. */
	break;
      }
#endif
      if (!gc_chain_splice(p, o))
	continue;  /* Concurrent insertion at this link; revisit boundedly. */
      gc2_sweep_detached_obj(g, o, marked > 0);
      unlinked++;
      continue;
    }
    p = lj_obj_gcwref(o);
  }
  gc2_sweep_root_cursor_rel(g, p);
  if (gcref_acq(*p) == NULL) {
    uint32_t flushed = lj_gc_flush_root_pending(g);
    (void)lj_gc_repair_root_spine(g);
    if (flushed != 0 || lj_gcroot_pending_hint_acq(g) != 0) {
      gc2_sweep_root_cursor_rel(g, lj_gc_root_ref(g));
    } else {
      gc2_sweep_root_done_rel(g, 1);
      gc2_sweep_root_cursor_rel(g, NULL);
    }
  }
  return unlinked;
}

static uint32_t gc2_sweep_alloc_end(const GCArena *a, uint32_t start)
{
  uint32_t i = start + 1u;
  while (i < LJ_ARENA_CELLS &&
	 !lj_arena_bm_get(a->block, i) && !lj_arena_bm_get(a->mark, i))
    i++;
  return i;
}

static GCobj *gc2_sweep_cell_obj(global_State *g, GCArena *a,
				  uint32_t cell, uint32_t end)
{
  GCobj *o = (GCobj *)lj_arena_cellptr(a, cell);
#if LJ_HASFFI
  {
    char *base = (char *)o;
    size_t bytes = (size_t)(end - cell) << LJ_CELL_SHIFT;
    uint16_t offset = la_load16_acq((uint16_t *)(void *)base);
    if (offset >= sizeof(GCcdataVar) &&
	(size_t)offset + sizeof(GCcdata) <= bytes) {
      GCcdata *cd = (GCcdata *)(void *)(base + offset);
      void *realbase = NULL;
      GCSize size = 0;
      if (cd->gct == ~LJ_TCDATA && cdataisv(cd) &&
	  memcdatav(cd) == (void *)base &&
	  lj_cdata_validate(g, cd, &realbase, &size) &&
	  realbase == (void *)base && size <= bytes)
	return obj2gco(cd);
    }
  }
#else
  UNUSED(end);
#endif
  if (o->gch.gct == 0 || gc2_valid_freeable_obj(g, o))
    return o;
  return NULL;
}

static uint32_t gc2_sweep_arena_bodies(global_State *g, GCArena *a,
				       int unmarked_only)
{
  uint32_t i, n = 0;
  GCobj *o;
  if (!g || !a)
    return 0;
  if (unmarked_only) {
    (void)lj_gc_flush_root_pending(g);
    (void)lj_gc_repair_root_spine(g);
  }
  for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
    uint32_t state;
    if (!lj_arena_bm_get(a->block, i))
      continue;
    if (!unmarked_only) {
      o = (GCobj *)lj_arena_cellptr(a, i);
      if (isdead(g, o) && gc2_valid_freeable_obj(g, o)) {
	lj_gc_unlink_root_obj(g, o);
	if (gc2_free_unmarked_obj(g, o))
	  n++;
      }
      continue;
    }
    state = lj_arena_sweep_state_acq(a, i);
    if (state != LJ_ARENA_SWEEP_WHITE || lj_arena_bm_get(a->mark, i))
      continue;
    /* Every GC object is ownership-spine linked before publication. The
    ** bounded bridge classified all old nonfixed headers before arena scan, so
    ** a remaining WHITE allocation is raw/opaque storage. Retain it unless its
    ** owning subsystem physically frees it (which transitions FREEING). Never
    ** infer a destructor from attacker-controlled payload bytes. */
    (void)la_bit_test_and_set64(&a->mark[i >> 6], i & 63);
  }
  return n;
}

uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a)
{
  return gc2_sweep_arena_bodies(g, a, 1);
}

uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,
				  uint32_t limit, int *donep)
{
  uint32_t cell, scanned = 0, changed = 0;
  int pending = 0;
  if (donep)
    *donep = 0;
  if (!g || !a || limit == 0 ||
      !(lj_arena_flags_acq(a) & LJ_AF_QUARANTINE))
    return 0;
  /* The activation mirror can only veto this legacy-authorized reclaim. */
  if (lj_gc2_activation_reclaim_veto(g))
    return 0;
  cell = a->hdr.reclaim_cell;
  if (cell < LJ_AFIRST_CELL || cell > LJ_ARENA_CELLS)
    cell = LJ_AFIRST_CELL;
  while (cell < LJ_ARENA_CELLS && scanned < limit) {
    uint32_t state;
    if (!lj_arena_bm_get(a->block, cell)) {
      cell++;
      scanned++;
      continue;
    }
    state = lj_arena_sweep_state_acq(a, cell);
    if (lj_arena_late_get(a, cell)) {
      /* A bit-only terminal publisher has already completed the physical
      ** destructor. Never reconstruct this header. Retain the allocation for
      ** the next generation and settle detached accounting exactly once. */
      while (state != LJ_ARENA_SWEEP_WHITE) {
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_WHITE)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "late-pin deferred underflow");
	    UNUSED(old);
	  }
	  changed++;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
      cell++;
      scanned++;
      continue;
    }
    if (state == LJ_ARENA_SWEEP_RETIRED &&
	((la_load64_acq(&a->mark[cell >> 6]) >> (cell & 63)) & 1u) &&
	lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					 LJ_ARENA_SWEEP_LIVE)) {
      uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "sealed marked rescue underflow");
      UNUSED(old);
      state = LJ_ARENA_SWEEP_LIVE;
      changed++;
    }
    if (state == LJ_ARENA_SWEEP_LIVE ||
	state == LJ_ARENA_SWEEP_RETIRED) {
      uint32_t end = gc2_sweep_alloc_end(a, cell);
      GCobj *o = gc2_sweep_cell_obj(g, a, cell, end);
      if (!o) {
	/* A valid detach publishes only an exact ownership-spine header, but a
	** hostile/racy producer may leave a header snapshot temporarily or
	** permanently undecodable. Fail closed without parking the whole GC: pin
	** the allocation as graphless raw storage and settle detached accounting.
	** A later semantic edge can validate/mark the same block again; reclamation
	** never reuses it based on attacker-controlled header bytes. */
	(void)la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_WHITE)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "invalid-header deferred underflow");
	    UNUSED(old);
	  }
	  changed++;
	} else {
	  pending = 1;
	}
	cell++;
	scanned++;
	continue;
      }
      if (state == LJ_ARENA_SWEEP_LIVE) {
	if (o->gch.gct == 0) {
	  if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					      LJ_ARENA_SWEEP_FREEING))
	    changed++;
	  else
	    pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
#if LJ_HASJIT
	if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	    la_load64_acq(&gco2trace(o)->retire_epoch) != 0) {
	  /* A retire-list root deliberately marked this trace during the grace.
	  ** It is not a rescued Lua root: leave it detached until the token-owned
	  ** physical destructor publishes gct=0/FREEING. */
	  (void)lj_trace_free_gc(g, gco2trace(o));
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
#endif
	/* The detach left gcw untouched for pre-grace root readers. Now no such
	** reader remains, publish one fresh ownership link and retire LIVE state. */
	lj_gc_linkobj(g, o);
	if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					    LJ_ARENA_SWEEP_WHITE))
	  changed++;
	else
	  pending = 1;  /* A concurrent physical free won; revisit conservatively. */
      } else {
        if (o->gch.gct == 0) {
	  if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					      LJ_ARENA_SWEEP_FREEING)) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "destroyed-body deferred underflow");
	    UNUSED(old);
	    changed++;
	  } else {
	    pending = 1;
	  }
	  cell++;
	  scanned++;
	  continue;
	}
#if LJ_HASJIT
	if (o->gch.gct == (uint32_t)~LJ_TTRACE) {
	  /* Listing/slot teardown is not physical completion. Keep RETIRED until
	  ** trace_freebody tears down exittab and release-publishes gct=0. */
	  if (!lj_trace_body_destroyed_acq(gco2trace(o)))
	    (void)lj_trace_free_gc(g, gco2trace(o));
	  pending = 1;
	} else
#endif
	if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					    LJ_ARENA_SWEEP_FREEING)) {
	  uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	  int freed;
	  lj_assertG(old != 0, "arena destructor deferred underflow");
	  UNUSED(old);
	  freed = gc2_free_unmarked_obj(g, o);
	  if (LJ_UNLIKELY(!freed)) {
	    (void)lj_arena_reclaim_deferred_add(a, 1);
	    (void)lj_arena_sweep_state_cas(a, cell,
	      LJ_ARENA_SWEEP_FREEING, LJ_ARENA_SWEEP_RETIRED);
	    pending = 1;
	  } else {
	    changed++;
	  }
	} else {
	  pending = 1;  /* RETIRED->LIVE rescue or physical-free completion. */
	}
      }
    }
    cell++;
    scanned++;
  }
  a->hdr.reclaim_cell = cell;
  if (cell == LJ_ARENA_CELLS) {
    uint32_t deferred = lj_arena_reclaim_deferred_acq(a);
    if (deferred == 0 && !pending) {
      if (donep)
	*donep = 1;
    } else {
      a->hdr.reclaim_cell = LJ_AFIRST_CELL;
    }
  }
  /* Cursor advancement is bounded real work and must keep the owner scheduled
  ** until it reaches EOF. At EOF, a token-busy trace returns zero so the worker
  ** can park rather than spin; JIT token release wakes it for another pass. */
  if (changed)
    return changed;
  return cell < LJ_ARENA_CELLS && scanned != 0 ? 1u : 0u;
}

uint32_t lj_gc_reclaim_gc2_huge(global_State *g, TGState *tg, void *p,
				 const LJHugeInfo *hi, int *pendingp)
{
  GCArena *a;
  GCobj *o;
  uint32_t flags;
  if (pendingp)
    *pendingp = 0;
  if (!g || !tg || !p || !hi)
    return 0;
  /* Keep the mapping and its ticket intact on any typed/legacy disagreement. */
  if (lj_gc2_activation_reclaim_veto(g)) {
    if (pendingp)
      *pendingp = 1;
    return 0;
  }
  a = lj_arena_of(p);
  flags = hi->flags;
  if (flags & LJ_HUGEF_BUSY) {
    if (pendingp) *pendingp = 1;
    return 0;  /* Publisher/reanchor owns all mapping-header access. */
  }
  o = (flags & LJ_HUGEF_TICKET) ?
      (GCobj *)la_loadptr_acq((void *const *)&a->hdr.retire_obj) : NULL;

  if (flags & LJ_HUGEF_FREEING) {
    uint64_t retire_epoch = la_load64_acq(&a->hdr.retire_epoch);
    uint64_t now = lj_gc2_retire_epoch(g);
    if (retire_epoch == ~(uint64_t)0) {
      la_store64_rel(&a->hdr.retire_epoch, now);
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      return 1;
    }
    if (retire_epoch >= now) {
      if (pendingp) *pendingp = 1;
      return 0;
    }
  }

  if (flags & LJ_HUGEF_MARK) {
    if (o && (flags & LJ_HUGEF_TICKET)) {
#if LJ_HASJIT
      if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	  la_load64_acq(&gco2trace(o)->retire_epoch) != 0) {
	(void)lj_trace_free_gc(g, gco2trace(o));
	if (pendingp) *pendingp = 1;
	return 0;
      }
#endif
      if (!gc2_valid_freeable_obj(g, o)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      if (!lj_arena_hugetab_claim_live_ticket(&tg->huge, p, NULL)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      /* Every old nonfixed root was detached before the grace. One mapping has
      ** one allocation, so retire_obj is an exact single reanchor ticket. */
      lj_gc_linkobj(g, o);
      la_storeptr_rel(&a->hdr.retire_obj, NULL);
      {
	int finished = lj_arena_hugetab_finish_live_ticket(&tg->huge, p, NULL);
	lj_assertG(finished, "huge live ticket lost after root reanchor");
	if (!finished) {
	  if (pendingp) *pendingp = 1;
	  return 0;
	}
      }
      return 1;
    }
    return 0;  /* Marked raw/fixed huge storage was never detached. */
  }

  if (!(flags & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING))) {
    if (o && lj_arena_hugetab_retire(&tg->huge, p, o,
					     lj_gc2_retire_epoch(g), NULL)) {
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      return 1;
    }
    /* Opaque huge storage has no GC destructor proof. Preserve it rather than
    ** interpreting payload bytes as a header. */
    (void)lj_arena_hugetab_mark(&tg->huge, p, NULL);
    return 1;
  }

  if (!o) {
    /* An external subsystem owns the logical free. FREEING is enough to let
    ** this sole huge-table owner perform the final delete after the grace. */
    if (!(flags & LJ_HUGEF_FREEING) &&
	!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
      if (pendingp) *pendingp = 1;
      return 0;
    }
  } else {
#if LJ_HASJIT
    if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	!lj_trace_body_destroyed_acq(gco2trace(o))) {
      (void)lj_trace_free_gc(g, gco2trace(o));
      if (pendingp) *pendingp = 1;
      return 0;
    }
#endif
    if (!(flags & LJ_HUGEF_FREEING)) {
      if (!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      if (o->gch.gct != 0 && !gc2_free_unmarked_obj(g, o)) {
	(void)lj_arena_hugetab_revert_retired(&tg->huge, p);
	if (pendingp) *pendingp = 1;
	return 0;
      }
    }
  }
  {
    LJHugeInfo snap;
    if (lj_arena_hugetab_delete(&tg->huge, p, &snap) == 1) {
      /* No access to a/a->hdr is legal after this unmap. */
      lj_arena_huge_unmap(p, snap.size);
      return 1;
    }
  }
  return 0;
}

uint32_t lj_gc_sweep_gc2_all_arena_bodies(global_State *g)
{
  TGState *tg;
  uint32_t total = 0;
  if (!g)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    GCArena *a;
    if (!lj_gc2_sweep_tg_ready(tg))
      continue;
    for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a;
	 a = lj_arena_next_acq(a))
      total += gc2_sweep_arena_bodies(g, a, 0);
    for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]; a;
	 a = lj_arena_next_acq(a))
      total += gc2_sweep_arena_bodies(g, a, 0);
  }
  return total;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  /*
  ** Weak clearing reads slots without owning their table generation. A stale
  ** tagged GC pointer whose header has been reused is not a live weak edge.
  ** Clear it instead of dereferencing reclaimed header state.
  */
  if (LJ_UNLIKELY(!lj_gc2_tv_gcref_valid_edge(g, o)))
    return 1;
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      (void)lj_gc2_markobj(g, gcV(o));  /* Interned strings remain strong. */
      return 0;
    }
    if (lj_gc2_ismarked(g, gcV(o)) <= 0)
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
void lj_gc_clearweak_bridge(global_State *g, GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK),
	       "clear of non-weak table");
    if ((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAKVAL)) {
      TValue *array;
      MSize i, asize, acap;
      if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) ==
	  LJ_TAB_GC_SNAPSHOT_OK) {
	UNUSED(acap);
	for (i = 0; i < asize; i++) {
	  /* Clear array slot when value is about to be collected. */
	  TValue val;
	  TValue *slot = &array[i];
	  lj_tv_load_acq(&val, &array[i]);
	  if (tvisforward(&val)) {
	    slot = lj_tab_forwarded_array_slot(t, array, asize, i, &val);
	    if (!slot)
	      continue;
	  }
	  if (gc_mayclear(g, &val, 1)) {
	    TValue key;
	    setintV(&key, (int32_t)i);
	    (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	  }
	}
      }
    }
    {
      MSize i, hmask;
      Node *node;
      if (lj_tab_node_snapshot_gc(g, t, &node, &hmask) ==
	  LJ_TAB_GC_SNAPSHOT_OK) {
	for (i = 0; i <= hmask; i++) {
	  Node *n = &node[i];
	  TValue key, val;
	  TValue *slot = &n->val;
	  int key_loaded = 0;
	  /* Clear hash slot when key or value is about to be collected. */
	  lj_tv_load_acq(&val, &n->val);
	  if (tvisforward(&val)) {
	    lj_tv_load_acq(&key, &n->key);
	    key_loaded = 1;
	    while (tviskeylock(&key)) {
	      gc_root_wait_no_l();
	      lj_tv_load_acq(&key, &n->key);
	    }
	    if (tvisnil(&key))
	      continue;
	    slot = lj_tab_forwarded_hash_slot(t, node, hmask, &key, &val);
	    if (!slot)
	      continue;
	  }
	  if (!tvisnil(&val)) {
	    if (!key_loaded)
	      lj_tv_load_acq(&key, &n->key);
	    while (tviskeylock(&key)) {
	      gc_root_wait_no_l();
	      lj_tv_load_acq(&key, &n->key);
	    }
	    if (gc_mayclear(g, &key, 0) || gc_mayclear(g, &val, 1))
	      (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	  }
	}
      }
    }
    o = lj_tab_gclist_acq(t);
  }
}

typedef struct GCShutdownSeen {
  const void **slot;
  size_t mask;
  size_t count;
} GCShutdownSeen;

static size_t gc_shutdown_ptrhash(const void *p)
{
  uintptr_t x = (uintptr_t)p >> 3;
#if LJ_64
  x ^= x >> 33;
  x *= (uintptr_t)0xff51afd7ed558ccdULL;
  x ^= x >> 33;
#else
  x ^= x >> 16;
  x *= (uintptr_t)0x7feb352dUL;
  x ^= x >> 15;
#endif
  return (size_t)x;
}

static int gc_shutdown_seen_grow(GCShutdownSeen *seen, size_t minslots)
{
  const void **old = seen->slot;
  size_t oldcap = seen->mask + 1u;
  size_t cap = old ? oldcap << 1 : 1024u;
  const void **slot;
  size_t i;
  while (cap < minslots && cap <= (SIZE_MAX >> 1))
    cap <<= 1;
  if (cap < minslots || cap > SIZE_MAX / sizeof(*slot))
    return 0;
  slot = (const void **)calloc(cap, sizeof(*slot));
  if (!slot)
    return 0;
  if (old) {
    for (i = 0; i < oldcap; i++) {
      const void *p = old[i];
      if (p) {
	size_t j = gc_shutdown_ptrhash(p) & (cap - 1u);
	while (slot[j])
	  j = (j + 1u) & (cap - 1u);
	slot[j] = p;
      }
    }
    free((void *)old);
  }
  seen->slot = slot;
  seen->mask = cap - 1u;
  return 1;
}

/* Return 1 for a first observation, 0 for a duplicate/cycle, -1 on OOM. */
static int gc_shutdown_seen_add(GCShutdownSeen *seen, const void *p)
{
  size_t i;
  if (!seen->slot && !gc_shutdown_seen_grow(seen, 1024u))
    return -1;
  if (seen->count >= (seen->mask + 1u) / 2u &&
      !gc_shutdown_seen_grow(seen, (seen->mask + 1u) << 1))
    return -1;
  i = gc_shutdown_ptrhash(p) & seen->mask;
  while (seen->slot[i]) {
    if (seen->slot[i] == p)
      return 0;
    i = (i + 1u) & seen->mask;
  }
  seen->slot[i] = p;
  seen->count++;
  return 1;
}

static void gc_shutdown_seen_clear(GCShutdownSeen *seen)
{
  if (seen->slot)
    memset((void *)seen->slot, 0, (seen->mask + 1u) * sizeof(*seen->slot));
  seen->count = 0;
}

static void gc_shutdown_seen_fini(GCShutdownSeen *seen)
{
  free((void *)seen->slot);
  seen->slot = NULL;
  seen->mask = seen->count = 0;
}

static void gc2_shutdown_free_obj(global_State *g, GCobj *o)
{
  uint32_t gct;
  int deferred;
  if (!gc2_valid_freeable_obj(g, o)) {
    /* A stale ownership-spine entry has either already had its destructor run
    ** or belongs to terminal arena teardown. Never dispatch from an untrusted
    ** type byte merely because the process is closing. */
    return;
  }
  gct = o->gch.gct;
  deferred = gc2_deferred_body_pending(g, o);
#if LJ_HASJIT
  if (gct == (uint32_t)~LJ_TTRACE) {
    (void)lj_trace_free_gc(g, gco2trace(o));
    return;  /* Trace retirement owns its intact header until the SMR drain. */
  }
#endif
  gc_freefunc[gct - (uint32_t)~LJ_TSTR](g, o);
  if (deferred)
    la_store8_rel(&o->gch.gct, 0);  /* Arena body awaits terminal unmap. */
}

static uint32_t gc2_shutdown_free_roots(global_State *g,
					 GCShutdownSeen *seen)
{
  GCobj *maino = obj2gco(mainthread_acq(g));
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  uint32_t freed = 0;
  gc_shutdown_seen_clear(seen);
  while ((o = gcref_acq(*p)) != NULL) {
    GCobj *next;
    int seenrc = gc_shutdown_seen_add(seen, o);
    if (seenrc <= 0) {
      /* A duplicate intrusive node necessarily closes a cycle: one object has
      ** only one gcw link. Allocation failure is also fail-safe: stop before
      ** dereferencing an untracked body rather than risking terminal UAF. */
      setgcrefnullrel(*p);
      break;
    }
    if (!gc_root_link_valid(g, o)) {
      setgcrefnullrel(*p);
      break;
    }
    next = lj_obj_gcw_acq(o);
    if (o == maino) {
      p = lj_obj_gcwref(o);
      continue;
    }
    if (next)
      setgcrefrel(*p, next);
    else
      setgcrefnullrel(*p);
    gc2_shutdown_free_obj(g, o);
    freed++;
  }
  return freed;
}

static void gc2_shutdown_free_strings(global_State *g,
				      GCShutdownSeen *seen)
{
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  MSize i;
  if (hdr) {
    gc_shutdown_seen_clear(seen);
    for (i = hdr->mask; i != ~(MSize)0; i--) {
      GCRef *bucket = &hdr->bucket[i];
      uintptr_t u = lj_str_ref_load_acq(bucket);
      GCobj *o = (GCobj *)(u & ~(uintptr_t)LJ_STRHASH_LINKMASK);
      lj_str_ref_store_rel(bucket, u & LJ_STRHASH_SECONDARY);
      while (o) {
	GCobj *next;
	int seenrc = gc_shutdown_seen_add(seen, o);
	if (seenrc <= 0)
	  break;
	next = lj_str_next_acq(o);
	if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TSTR)
	  lj_str_free(g, gco2str(o));
	o = next;
      }
    }
  }
  /* Unlinked bodies are no longer discoverable from the active table. */
  lj_str_free_retired_bodies(g);
}

/* Free all remaining GC objects with GC2 ownership metadata only. This is a
** terminal, single-threaded drain after threading shutdown and finalizers; it
** deliberately never starts the retired color marker or sweeper. */
void lj_gc2_freeall(global_State *g)
{
  GCShutdownSeen seen;
  GCobj *maino;
  uint32_t rounds = 0;
  memset(&seen, 0, sizeof(seen));
  (void)lj_gc_repair_root_spine(g);
  do {
    (void)lj_gc_flush_root_pending(g);
    if (gc2_shutdown_free_roots(g, &seen) == 0)
      break;
  } while (++rounds < 16u);  /* Closing a thread can publish closed upvalues. */
  (void)lj_gc_flush_root_pending(g);
  (void)gc2_shutdown_free_roots(g, &seen);
  maino = obj2gco(mainthread_acq(g));
  lj_obj_setgcwrel(maino, NULL);
  lj_gc_root_rel(g, maino);
  gc2_shutdown_free_strings(g, &seen);
  gc_shutdown_seen_fini(&seen);
}

#if LJ_HASJIT
int lj_gc2_jit_needs_exit(global_State *g)
{
  uint32_t phase = gc2_phase_acq(g);
  return lj_tg_any_jit_active(g) &&
	 (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
	  phase == LJ_GC2_SWEEP ||
	  lj_gc_total_load(g) >= lj_gc_threshold_load(g) ||
	  gc_hard_assist_due_jit(g));
}
#else
#define lj_gc2_jit_needs_exit(g)	0
#endif

static int gc2_step_auto(lua_State *L, int threshold_step, uint32_t step_limit)
{
  global_State *g = G(L);
  GCSize quantum = gc_step_debt_quantum(g);
  int32_t ostate = vmstate_load_acq(g);
  int running = gc_logical_running(g);
  int drove = 0;
  int done = 0;
  if (step_limit == 0)
    step_limit = 1;
  setvmstate(g, GC);
  if (running)
    lj_gc2_check_trigger(g, L2TG(L));
  while (running && step_limit-- != 0) {
    uint32_t phase = gc2_phase_acq(g);
    if (phase == LJ_GC2_IDLE && gc2_cycle_leader_acq(g) == 0 &&
	!threshold_step)
      break;
    drove = 1;
    done = lj_gc2_step_explicit(L, 1);
    if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      break;
  }
  g->gc.debt = 0;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE) {
    if (running)
      lj_gc2_publish_idle_threshold(g);
    vmstate_store_rel(g, ostate);
    return done ? 1 : (drove ? 0 : -1);
  }
  lj_gc_threshold_store(g, lj_gc_total_load(g) + quantum);
  vmstate_store_rel(g, ostate);
  return -1;
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  return gc2_step_auto(L, lj_gc_total_load(g) >= lj_gc_threshold_load(g),
		       GCACTIVEAUTOSTEPS);
}

#ifdef LJ_GC2_TEST_HELPERS
static uint32_t gc_test_step_fixtop_calls;

static LJ_AINLINE void gc_test_step_fixtop_call(void)
{
  (void)la_add32_acqrel(&gc_test_step_fixtop_calls, 1);
}

uint32_t lj_gc_test_step_fixtop_calls(void)
{
  return la_load32_acq(&gc_test_step_fixtop_calls);
}

void lj_gc_test_reset_step_fixtop_calls(void)
{
  la_store32_rel(&gc_test_step_fixtop_calls, 0);
}
#else
#define gc_test_step_fixtop_call()	((void)0)
#endif

static void gc_step_assist_top(lua_State *L, global_State *g, int threshold_step)
{
  TGState *tg = L2TG(L);
  lj_gc2_check_trigger(g, L2TG(L));
  if (!threshold_step)
    threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  if (gc_hard_assist_due_interp(g, tg)) {
    gc2_interp_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 interpreter assist bridge. */
  }
  if (threshold_step)
    gc2_step_auto(L, threshold_step, GCACTIVEAUTOSTEPS);
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  global_State *g = G(L);
  gc_test_step_fixtop_call();
  if (curr_funcisL(L)) L->top = curr_topL(L);
  gc_step_assist_top(L, g, lj_gc_total_load(g) >= lj_gc_threshold_load(g));
}

/* Ditto, but use an already fixed stack top. */
void LJ_FASTCALL lj_gc_step_top(lua_State *L)
{
  global_State *g = G(L);
  gc_step_assist_top(L, g, lj_gc_total_load(g) >= lj_gc_threshold_load(g));
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = lj_tg_cur_L(g);
  TValue *jbase = lj_tg_jit_base(g);
  int threshold_step, hard_step, needs_exit;
  TGState *tg;
  if (!L || !jbase)
    return 1;
  L->base = jbase;
  L->top = curr_topL(L);
  tg = L2TG(L);
  if (lj_gc2_jit_needs_exit(g))
    return 1;
  lj_gc2_check_trigger(g, tg);
  threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  hard_step = gc_hard_assist_due_jit(g);
  if (hard_step) {
    gc2_jit_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 trace-side assist bridge. */
    lj_gc2_hard_check_advance(g, lj_gc2_alloc_since_load(g));
  }
  if (threshold_step) {
    while (steps-- > 0 && gc2_step_auto(L, threshold_step, 1) == 0)
      threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  }
  needs_exit = lj_gc2_jit_needs_exit(g);
  /*
  ** Return 1 to force a trace exit when GC2 needs interpreter-owned progress:
  ** mark root snapshots, weak/sweep transitions, or finalizer dispatch. GC2
  ** active-black allocation can make a trace-local container live before its
  ** payload has been closed by a root scan; leaving at the poll gives the
  ** interpreter-side scanner an authoritative frame before the cycle advances.
  */
  return needs_exit;
}
#endif

/* -- Write barriers ------------------------------------------------------ */

/* Barrier for a store to a global root slot. */
void lj_gc_pubroot(lua_State *L, cTValue *tv)
{
  global_State *g;
  TValue snap;
  if (!L || !tv)
    return;
  g = G(L);
  if (LJ_UNLIKELY(g == NULL && L->tg_hint != NULL && L->tg_hint->gl != NULL)) {
    g = L->tg_hint->gl;
    setmref(L->glref, g);
  }
  if (LJ_UNLIKELY(g == NULL))
    return;
  lj_tv_load_acq(&snap, tv);
  if (tvisgcv(&snap)) {
    if (!lj_gc2_tv_gcref_valid_edge(g, &snap))
      return;
    /* GC2 is the sole runtime collector. Its publication barrier covers
    ** MARK/WEAK, SWEEP resurrection and the generational IDLE remembered set;
    ** never revive or traverse the retired color collector here. */
    lj_gc2_barrier_tv_g(g, &snap);
  }
}

/* Publish a GC object that is becoming reachable from a native root list. */
void lj_gc_pubobjroot(lua_State *L, GCobj *o)
{
  global_State *g;
  uint32_t gct;
  TValue tv;
  if (!L)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, o, &gct)))
    return;
  setgcV(L, &tv, o, ~gct);
  lj_gc_pubroot(L, &tv);
}

/* Compatibility entry for a published object edge. GC2 owns the runtime
** frontier; the legacy color frontier is never entered. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  if (!g || !v)
    return;
  lj_gc2_barrier_obj_pair_g(g, o, v);
}

/* VM-callable table black-to-gray repair. */
void lj_gc_barrierback_tab_g(global_State *g, GCtab *t)
{
  if (g && t)
    lj_gc2_barrier_tab_g(g, t);
}

void lj_gc_tbar_trace_g(global_State *g, GCtab *t, cTValue *key)
{
  if (!g || !t)
    return;
  if (key)
    lj_gc2_barrier_key_g(g, t, key);
  else
    lj_gc2_barrier_tab_g(g, t);
}

/* Publication wrapper for x64 VM table -> object stores. */
void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o)
{
  global_State *g;
  uint32_t gct;
  if (!L || !t || !o)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, obj2gco(t), &gct) ||
		  gct != (uint32_t)~LJ_TTAB ||
		  !gc_objroot_gct_valid(g, o, NULL)))
    return;
  lj_gc2_barrier_obj_pair(L, obj2gco(t), o);
}

/* Publication wrapper for x64 VM table -> TValue stores. */
void lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv)
{
  if (!L || !t || !tv)
    return;
  lj_gc_barriertv_(L, t, tv);
}

/* Publication wrapper for x64 VM table range stores. */
void lj_gc_pubtabtvn_vm(lua_State *L, GCtab *t, cTValue *tv, uint32_t n)
{
  global_State *g;
  uint32_t gct;
  if (!L || !t || !tv || n == 0)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, obj2gco(t), &gct) ||
		  gct != (uint32_t)~LJ_TTAB))
    return;
  lj_gc2_barrier_tvn_pair_g(g, obj2gco(t), tv, n);
  lj_gc2_barrier_tab(L, t);  /* Preserve the previous TSETM table barrier. */
}

/* Publication wrapper for x64 VM table -> stack loads. */
void lj_gc_pubtvroot_vm(lua_State *L, cTValue *tv)
{
  if (!L || !tv)
    return;
  /* DynASM stores and dirties the slot before entering this cold wrapper. */
  lj_gc_pubroot(L, tv);
}

/* Publication wrapper for closed-upvalue TValue stores. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv)
{
  GCupval *uv = (GCupval *)((char *)tv - offsetof(GCupval, tv));
  TValue snap;
  lj_tv_load_acq(&snap, tv);
  if (LJ_UNLIKELY(!lj_gc2_tv_gcref_valid_edge(g, &snap)) ||
      !tvisgcv(&snap))
    return;
  lj_gc2_barrier_tv_pair_g(g, obj2gco(uv), &snap);
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTVrel(mainthread_acq(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  /*
  ** lj_func_closeuv() has already removed the upvalue from the thread-open
  ** chain and g->uvhead, so its nextgc link is available for object-list
  ** publication. Liveness is still discovered through closures that reference
  ** the GCupval; the object list is the sweep/free spine and every consumer of
  ** that spine flushes pending roots first. Queueing here avoids a global
  ** root-list CAS on the close-upvalue path without changing collector reach.
  */
  lj_gc_linkobj_pending(g, o);
  lj_gc2_barrier_tv_pair_g(g, o, &uv->tv);
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_pubtrace(global_State *g, uint32_t traceno)
{
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    lj_gc2_mark_trace_slot(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

static LJArenaAllocD *gc_arena_allocd_for_tg(global_State *g, TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return &tg->allocd;
  return (LJArenaAllocD *)g->allocd;
}

static LJArenaAllocD *gc_arena_allocd_for_new(lua_State *L)
{
  return gc_arena_allocd_for_tg(G(L), L2TG(L));
}

static LJArenaAllocD *gc_arena_allocd_for_ptr(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_owner_acq(lj_arena_of(p));
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    if (tg)
      return gc_arena_allocd_for_tg(g, tg);
  }
  return (LJArenaAllocD *)g->allocd;
}

/* Allocate a new fragment without raising an error on allocation failure. */
void *lj_mem_new_nothrow(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  void *p;
  lj_assertG(size != 0, "zero-sized non-throwing allocation");
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = gc_arena_allocd_for_new(L);
    p = lj_arena_allocf(ad, NULL, 0, size);
  } else {
    p = g->allocf(g->allocd, NULL, 0, size);
  }
  if (p == NULL)
    return NULL;
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  lj_gc_total_add(g, size);
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return p;
}

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = p ? gc_arena_allocd_for_ptr(g, p) :
			    gc_arena_allocd_for_new(L);
    p = lj_arena_allocf(ad, p, osz, nsz);
  } else {
    p = g->allocf(g->allocd, p, osz, nsz);
  }
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lj_assertG((nsz == 0) == (p == NULL), "allocf API violation");
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  lj_gc_total_adjust(g, osz, nsz);
  if (nsz > osz)
    lj_gc2_account_alloc(g, L2TG(L), nsz - osz);  /* 04 section 4.8. */
  return p;
}

/* Allocate raw storage for a GC object without linking or throwing. */
void *lj_mem_newgco_raw_nothrow(lua_State *L, GCSize size, uint32_t flags)
{
  global_State *g = G(L);
  GCobj *o;
  if (g->allocf == lj_arena_allocf)
    o = (GCobj *)lj_arena_allocd_alloc(gc_arena_allocd_for_new(L), size,
				       flags);
  else
    o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    return NULL;
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  lj_gc_total_add(g, size);
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return o;
}

/* Allocate raw storage for a GC object without linking it. */
void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags)
{
  void *o = lj_mem_newgco_raw_nothrow(L, size, flags);
  if (o == NULL)
    lj_err_mem(L);
  return o;
}

void *lj_mem_newgco_unlinked_nothrow(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw_nothrow(L, size, LJ_AF_TRAVERSABLE);
}

void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
}

void lj_gc_linkobj(global_State *g, GCobj *o)
{
  GCRef *root = lj_gc_root_ref(g);
  GCobj *head;
  do {
    head = gcref_acq(*root);
    if (head)
      lj_obj_setgcw(o, head);
    else
      lj_obj_setgcwnull(o);
  } while (!gcref_cas(root, &head, o));  /* M7 publish. */
  lj_gcroot_repair_epoch_add(g);
}

static LJ_AINLINE void gc_root_set_next(GCobj *o, GCobj *next)
{
  if (next)
    lj_obj_setgcw(o, next);
  else
    lj_obj_setgcwnull(o);
}

static LJ_AINLINE void gc_root_set_next_rel(GCobj *o, const GCobj *next)
{
  if (next)
    lj_obj_setgcwrel(o, next);
  else
    lj_obj_setgcwnullrel(o);
}

static uint32_t gc_root_chain_tail(global_State *g, GCobj *head, GCobj **tailp)
{
  GCobj *tail, *next;
  uint32_t n = 0;
  if (!head) {
    *tailp = NULL;
    return 0;
  }
  (void)gc_root_chain_break_cycle(g, head);
  tail = head;
  do {
    if (!gc_root_link_valid(g, tail))
      break;
    if (n != ~(uint32_t)0)
      n++;
    next = lj_obj_gcw_acq(tail);
    if (!next)
      break;
    if (!gc_root_link_valid(g, next))
      break;
    tail = next;
  } while (1);
  *tailp = tail;
  return n;
}

static int gc_root_chain_contains_to_tail(global_State *g, GCobj *head, GCobj *tail,
					  GCobj *needle)
{
  GCobj *o;
  if (!needle)
    return 0;
  for (o = head; o != NULL; o = lj_obj_gcw_acq(o)) {
    if (!gc_root_link_valid(g, o))
      return 0;
    if (o == needle)
      return 1;
    if (o == tail)
      return 0;
  }
  return 0;
}

static void gc_root_prepend_chain_at(global_State *g, GCRef *p, GCobj *head,
				     GCobj *tail)
{
  GCobj *oldhead;
  do {
    oldhead = gcref_acq(*p);
    /*
    ** Pending-root publication owns head..tail, but address reuse can make a
    ** freshly published pending chain overlap the existing GC root spine at
    ** the insertion point. Linking tail back to that old head would create a
    ** cycle; null-terminating instead preserves every unique object already
    ** reachable from head.
    */
    gc_root_set_next_rel(tail,
			 gc_root_chain_contains_to_tail(g, head, tail, oldhead) ?
			 NULL : oldhead);
  } while (!gcref_cas(p, &oldhead, head));
  lj_gcroot_repair_epoch_add(g);
}

static void gc_root_prepend_known_chain(global_State *g, GCobj *head,
					GCobj *tail)
{
  gc_root_prepend_chain_at(g, lj_gc_root_ref(g), head, tail);
}

static uint32_t gc_root_prepend_chain(global_State *g, GCobj *head)
{
  GCobj *tail;
  uint32_t n = gc_root_chain_tail(g, head, &tail);
  if (!n)
    return 0;
  gc_root_prepend_known_chain(g, head, tail);
  return n;
}

static uint32_t gc_root_prepend_chain_after(global_State *g, GCobj *anchor,
					    GCobj *head)
{
  GCobj *tail;
  uint32_t n = gc_root_chain_tail(g, head, &tail);
  if (!anchor || !n)
    return 0;
  gc_root_prepend_chain_at(g, lj_obj_gcwref(anchor), head, tail);
  if (LJ_UNLIKELY(!gc_root_chain_contains_obj(g, anchor)))
    n += gc_root_prepend_chain(g, anchor);
  return n;
}

static uint32_t gc_flush_root_pending_tg(global_State *g, TGState *tg)
{
  GCobj *head;
  uint32_t n;
  if (!g || !tg || tg->gl != g)
    return 0;
  head = lj_tg_gcroot_pending_xchg_acqrel(tg, NULL);
  n = gc_root_prepend_chain(g, head);
  head = lj_tg_gcroot_pending_after_main_xchg_acqrel(tg, NULL);
  n += gc_root_prepend_chain_after(g, obj2gco(mainthread_acq(g)), head);
  return n;
}

static void gc_pending_root_stats(global_State *g, uint32_t n)
{
  uint64_t old;
  if (n == 0)
    return;
  gc2_pending_root_flushes_add(g, 1);
  gc2_pending_root_flushed_add(g, n);
  old = gc2_pending_root_flush_max_acq(g);
  while (old < (uint64_t)n) {
    uint64_t expect = old;
    if (gc2_pending_root_flush_max_cas(g, &expect, (uint64_t)n))
      break;
    old = expect;
  }
}

static int gc_root_pending_tg_nonempty(TGState *tg)
{
  return tg &&
    (lj_tg_gcroot_pending_acq(tg) != NULL ||
     lj_tg_gcroot_pending_after_main_acq(tg) != NULL);
}

uint32_t lj_gc_flush_root_pending(global_State *g)
{
  TGState *tg, *main_tg, *self;
  uint32_t n = 0, saw_main = 0, saw_self = 0;
  if (!g)
    return 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
  /* Raw TLS may legitimately belong to another independent Lua universe on
  ** the same OS thread. It is neither a pending-root producer nor an attach
  ** edge for this registry; exclude it before the hint fast path inspects its
  ** owner-private chains. gc_flush_root_pending_tg() also rejects mismatched
  ** universes, but that is deliberately after the non-empty probe. */
  if (self && self->gl != g)
    self = NULL;
  /*
  ** This is only a non-empty hint. Publishers set it before and after pending
  ** stack publication; false positives are harmless and false negatives are
  ** covered for the two TGs that can publish without already being visible in
  ** gc2.tg_list: the main TG and the TLS-current TG during attach/detach edges.
  */
  if (lj_gcroot_pending_hint_acq(g) == 0 &&
      !gc_root_pending_tg_nonempty(main_tg) &&
      (self == NULL || self == main_tg ||
       !gc_root_pending_tg_nonempty(self)))
    return 0;
  /*
  ** Once the hint is non-zero, clear it before scanning so a concurrent
  ** publisher can republish a new non-empty state. The zero-hint fast return
  ** above intentionally avoids a global RMW on the overwhelmingly common empty
  ** flush path; concurrent remote publishers that set the hint after the load
  ** are covered by the next flush, matching the old xchg-after-zero race.
  */
  (void)lj_gcroot_pending_hint_xchg(g, 0);
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg)
      saw_main = 1;
    if (tg == self)
      saw_self = 1;
    n += gc_flush_root_pending_tg(g, tg);
  }
  if (main_tg && !saw_main)
    n += gc_flush_root_pending_tg(g, main_tg);
  if (self && self != main_tg && !saw_self)
    n += gc_flush_root_pending_tg(g, self);
  if (n != 0)
    (void)lj_gc_repair_root_spine(g);
  gc_pending_root_stats(g, n);
  return n;
}

static void gc_linkobj_pending(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *head;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    lj_gc_linkobj(g, o);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    /*
    ** Before secondary Lua threads, entering secondary attachers, or GC
    ** workers exist, the main TG is the only pending-root producer and
    ** flusher. Avoid the CAS/RMW allocation tax, but keep release publication
    ** so a later activation sees a complete pending chain.
    */
    head = lj_tg_gcroot_pending_acq(tg);
    gc_root_set_next_rel(o, head);
    lj_tg_gcroot_pending_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_acq(tg);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_cas(tg, &head, o));
}

void lj_gc_linkobj_pending(global_State *g, GCobj *o)
{
  gc_linkobj_pending(g, o);
}

void lj_gc_linkobj_new(global_State *g, GCobj *o)
{
  gc_linkobj_pending(g, o);
}

void lj_gc_linkobj_new_chain(global_State *g, GCobj *head, GCobj *tail)
{
  TGState *tg;
  GCobj *oldhead;
  if (!head || !tail)
    return;
  tg = lj_thr_get_tg();
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    gc_root_prepend_known_chain(g, head, tail);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    /*
    ** Publish a freshly initialized object run with one release store. The
    ** caller owns head..tail until this point; after the store, a later MT
    ** activation or root flush observes every object and edge in the run.
    */
    oldhead = lj_tg_gcroot_pending_acq(tg);
    gc_root_set_next_rel(tail, oldhead);
    lj_tg_gcroot_pending_store_transition_rel(tg, oldhead, head);
    return;
  }
  oldhead = lj_tg_gcroot_pending_acq(tg);
  do {
    gc_root_set_next_rel(tail, oldhead);
  } while (!lj_tg_gcroot_pending_cas(tg, &oldhead, head));
}

void lj_gc_linkobj_new_after_main(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *head;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    lj_gc_linkobj_after(g, obj2gco(mainthread_acq(g)), o);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    head = lj_tg_gcroot_pending_after_main_acq(tg);
    gc_root_set_next_rel(o, head);
    lj_tg_gcroot_pending_after_main_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_after_main_acq(tg);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_after_main_cas(tg, &head, o));
}

void lj_gc_linkobj_after(global_State *g, GCobj *anchor, GCobj *o)
{
  GCRef *p;
  GCobj *head;
  if (!anchor || !o)
    return;
  p = lj_obj_gcwref(anchor);
  do {
    head = gcref_acq(*p);
    gc_root_set_next(o, head);
  } while (!gcref_cas(p, &head, o));
  if (g)
    lj_gcroot_repair_epoch_add(g);
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
  newwhite(g, o);
  lj_gc_linkobj_new(g, o);
  return o;
}

void lj_mem_free(global_State *g, void *p, size_t osize)
{
  lj_gc_total_sub(g, (GCSize)osize);
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = gc_arena_allocd_for_ptr(g, p);
    (void)lj_arena_allocf(ad, p, osize, 0);
  } else {
    g->allocf(g->allocd, p, osize, 0);
  }
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

/*
** Account a dead traversable GC object body for later arena bitmap reclaim.
** A successful deferral means the type-specific destructor has finished and
** the body is released from the live bitmap without being inserted into an
** allocator free list for immediate reuse. Keep the object header intact until
** a later free-list rebuild owns reuse: VM/JIT readers can still hold
** SMR-protected stale pointers during the deferral window.
*/
int lj_mem_freegco_defer(global_State *g, void *p, GCSize osize)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!p || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a) || !(a->hdr.flags & LJ_AF_TRAVERSABLE))
    return 0;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  if (!lj_arena_free_deferred(&tg->alloc, p, osize))
    return 0;
  lj_gc_total_sub(g, osize);
  return 1;
}
