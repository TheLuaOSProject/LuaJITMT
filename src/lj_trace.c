/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_trace_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_gdbjit.h"
#include "lj_record.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_target.h"
#include "lj_prng.h"

#define LJ_TRACE_SCOPE_FLUSHING		(~(uint64_t)0)

/* -- Error handling ------------------------------------------------------ */

int lj_jit_token_try(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t expect = 0;
  if (!tg || tg->tid == 0)
    return 0;
  return la_cas32(&g->jit_token, &expect, tg->tid, LA_ACQ_REL, LA_ACQ);
}

int lj_jit_token_held(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  return tg && tg->tid != 0 && la_load32_acq(&g->jit_token) == tg->tid;
}

void lj_jit_token_release(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  if (tg && tg->tid != 0 && la_load32_acq(&g->jit_token) == tg->tid)
    la_store32_rel(&g->jit_token, 0);
}

void lj_trace_abort(global_State *g)
{
  jit_State *J = G2J(g);
  uint32_t old = la_load32_acq((uint32_t *)&J->state);
  while ((old & (uint32_t)LJ_TRACE_ACTIVE) != 0) {
    uint32_t next = old & ~(uint32_t)LJ_TRACE_ACTIVE;
    if (la_cas32((uint32_t *)&J->state, &old, next,
		 LA_ACQ_REL, LA_ACQ))
      break;  /* 08 section 8.7: publish async recorder abort. */
  }
}

/* Synchronous abort with error message. */
void lj_trace_err(jit_State *J, TraceError e)
{
  setnilV(&J->errinfo);  /* No error info. */
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* Synchronous abort with error message and error info. */
void lj_trace_err_info(jit_State *J, TraceError e)
{
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* -- Trace management ---------------------------------------------------- */

/* The current trace is first assembled in J->cur. The variable length
** arrays point to shared, growable buffers (J->irbuf etc.). When trace
** recording ends successfully, the current trace and its data structures
** are copied to a new (compact) GCtrace object.
*/

static TraceVec *tracevec_new(lua_State *L, MSize sizetrace)
{
  TraceVec *tv = (TraceVec *)lj_mem_new(L, tracevec_size(sizetrace));
  tv->sizetrace = sizetrace;
  tv->retire_epoch = 0;
  tv->retired_next = NULL;
  memset(tv->slot, 0, sizetrace*sizeof(GCRef));
  return tv;
}

static void tracevec_free(global_State *g, TraceVec *tv)
{
  lj_mem_free(g, tv, tracevec_size(tv->sizetrace));
}

static void tracevec_publish(jit_State *J, TraceVec *tv)
{
  J->trace = tv->slot;
  J->sizetrace = tv->sizetrace;
  la_storeptr_rel((void **)&J->tracev, tv);
}

static void tracevec_retired_push(jit_State *J, TraceVec *tv)
{
  void *head = la_loadptr_acq((void *const *)&J->retiredtracev);
  do {
    tv->retired_next = (TraceVec *)head;
  } while (!la_casptr((void **)&J->retiredtracev, &head, tv,
		      LA_ACQ_REL, LA_ACQ));  /* 08 section 8.3 RCU retire. */
}

static void tracevec_retire(jit_State *J, TraceVec *tv)
{
  if (tv) {
    la_store64_rel(&tv->retire_epoch, la_load64_acq(&J2G(J)->gc2.hs_epoch));
    tracevec_retired_push(J, tv);
  }
}

static void trace_exittab_free(global_State *g, GCtrace *T);

static GCSize trace_size(GCtrace *T)
{
  return (GCSize)(((sizeof(GCtrace)+7)&~7) +
    (T->nins-T->nk)*sizeof(IRIns) +
    T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry));
}

static void trace_retired_push(jit_State *J, GCtrace *T)
{
  void *head = la_loadptr_acq((void *const *)&J->retiredtraces);
  do {
    T->retired_next = (GCtrace *)head;
  } while (!la_casptr((void **)&J->retiredtraces, &head, T,
		      LA_ACQ_REL, LA_ACQ));  /* 08 section 8.7 trace SMR. */
}

static void trace_retire(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  uint64_t epoch = la_load64_acq(&T->retire_epoch);
  if (epoch == 0 || epoch == LJ_TRACE_SCOPE_FLUSHING)
    epoch = la_load64_acq(&g->gc2.hs_epoch);
  la_store64_rel(&T->retire_epoch, epoch);
  T->retired_next = NULL;
  lj_gc_arena_markmem(g, T);
  if (T->exittab)
    lj_gc_arena_markmem(g, T->exittab);
  trace_retired_push(J, T);
}

static void trace_freebody(global_State *g, GCtrace *T)
{
  trace_exittab_free(g, T);
  lj_mem_free(g, T, trace_size(T));
}

static void trace_free_immediate(global_State *g, GCtrace *T)
{
  trace_exittab_free(g, T);
  lj_mem_free(g, T, trace_size(T));
}

void LJ_FASTCALL lj_trace_free_unpublished(global_State *g, GCtrace *T)
{
  trace_free_immediate(g, T);
}

static LJ_AINLINE int trace_body_retire_ready(GCtrace *T,
					       uint64_t completed_epoch)
{
  uint64_t retire_epoch = la_load64_acq(&T->retire_epoch);
  return completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_FLUSH_EPOCHS;
}

static void trace_markbody(global_State *g, GCtrace *T, int gc2)
{
  IRRef ref;
  GCobj *startpt;
  if (gc2) lj_gc2_markmem(g, T); else lj_gc_arena_markmem(g, T);
  if (T->exittab) {
    if (gc2) lj_gc2_markmem(g, T->exittab);
    else lj_gc_arena_markmem(g, T->exittab);
  }
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir_iskgc_acq(ir)) {
      GCobj *o = ir_kgc_load_acq(ir);
      if (gc2) lj_gc2_markobj(g, o);
      else lj_gc_arena_markobj(g, o);
    }
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  startpt = trace_startptgco_acq(T);
  if (startpt) {
    if (gc2) lj_gc2_markobj(g, startpt);
    else lj_gc_arena_markobj(g, startpt);
  }
}

uint32_t lj_trace_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  jit_State *J;
  TraceVec *tv;
  GCtrace *rt;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  J = G2J(g);
  tv = (TraceVec *)la_xchgptr_acqrel((void **)&J->retiredtracev, NULL);
  while (tv) {
    TraceVec *next = tv->retired_next;
    tv->retired_next = NULL;
    if (la_load64_acq(&tv->retire_epoch) < completed_epoch) {
      tracevec_free(g, tv);
      reclaimed++;
    } else {
      tracevec_retired_push(J, tv);
    }
    tv = next;
  }
  rt = (GCtrace *)la_xchgptr_acqrel((void **)&J->retiredtraces, NULL);
  while (rt) {
    GCtrace *next = rt->retired_next;
    rt->retired_next = NULL;
    if (trace_body_retire_ready(rt, completed_epoch)) {
      trace_freebody(g, rt);
      reclaimed++;
    } else {
      trace_retired_push(J, rt);
    }
    rt = next;
  }
  return reclaimed;
}

void lj_trace_freeretired(global_State *g)
{
  jit_State *J = G2J(g);
  TraceVec *tv = (TraceVec *)la_xchgptr_acqrel((void **)&J->retiredtracev,
					       NULL);
  GCtrace *rt;
  while (tv) {
    TraceVec *next = tv->retired_next;
    tracevec_free(g, tv);
    tv = next;
  }
  rt = (GCtrace *)la_xchgptr_acqrel((void **)&J->retiredtraces, NULL);
  while (rt) {
    GCtrace *next = rt->retired_next;
    trace_freebody(g, rt);
    rt = next;
  }
}

void lj_trace_markvecs(global_State *g, int gc2)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  GCtrace *rt;
  if (tv) {
    if (gc2) lj_gc2_markmem(g, tv); else lj_gc_arena_markmem(g, tv);
  }
  for (tv = (TraceVec *)la_loadptr_acq((void *const *)&J->retiredtracev);
       tv != NULL;
       tv = (TraceVec *)la_loadptr_acq((void *const *)&tv->retired_next)) {
    if (gc2) lj_gc2_markmem(g, tv); else lj_gc_arena_markmem(g, tv);
  }
  for (rt = (GCtrace *)la_loadptr_acq((void *const *)&J->retiredtraces);
       rt != NULL;
       rt = (GCtrace *)la_loadptr_acq((void *const *)&rt->retired_next))
    trace_markbody(g, rt, gc2);
}

/* Find a free trace number. */
static TraceNo trace_findfree(jit_State *J)
{
  MSize osz, lim;
  TraceVec *oldtv, *newtv;
  if (J->freetrace == 0)
    J->freetrace = 1;
  for (; J->freetrace < J->sizetrace; J->freetrace++)
    if (traceref(J, J->freetrace) == NULL)
      return J->freetrace++;
  /* Need to grow trace array. */
  lim = (MSize)J->param[JIT_P_maxtrace] + 1;
  if (lim < 2) lim = 2; else if (lim > 65535) lim = 65535;
  osz = J->sizetrace;
  if (osz >= lim)
    return 0;  /* Too many traces. */
  oldtv = J->tracev;
  newtv = tracevec_new(J->L, lim);
  if (oldtv)
    memcpy(newtv->slot, oldtv->slot, osz*sizeof(GCRef));
  tracevec_publish(J, newtv);
  tracevec_retire(J, oldtv);
  return J->freetrace;
}

#define TRACE_APPENDVEC(field, szfield, tp) \
  T->field = (tp *)p; \
  memcpy(p, J->cur.field, J->cur.szfield*sizeof(tp)); \
  p += J->cur.szfield*sizeof(tp);

#ifdef LUAJIT_USE_PERFTOOLS
/*
** Create symbol table of JIT-compiled code. For use with Linux perf tools.
** Example usage:
**   perf record -f -e cycles luajit test.lua
**   perf report -s symbol
**   rm perf.data /tmp/perf-*.map
*/
#include <stdio.h>
#include <unistd.h>

static void perftools_addtrace(GCtrace *T)
{
  static FILE *fp;
  GCproto *pt = trace_startpt_acq(T);
  const BCIns *startpc = mref(T->startpc, const BCIns);
  const char *name = proto_chunknamestr(pt);
  BCLine lineno;
  if (name[0] == '@' || name[0] == '=')
    name++;
  else
    name = "(string)";
  lj_assertX(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc,
	     "trace PC out of range");
  lineno = lj_debug_line(pt, proto_bcpos(pt, startpc));
  if (!fp) {
    char fname[40];
    sprintf(fname, "/tmp/perf-%d.map", getpid());
    if (!(fp = fopen(fname, "w"))) return;
    setlinebuf(fp);
  }
  fprintf(fp, "%lx %x TRACE_%d::%s:%u\n",
	  (long)T->mcode, T->szmcode, T->traceno, name, lineno);
}
#endif

/* Allocate space for copy of T. */
GCtrace * LJ_FASTCALL lj_trace_alloc(lua_State *L, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (T->nins-T->nk)*sizeof(IRIns);
  size_t sz = sztr + szins +
	      T->nsnap*sizeof(SnapShot) +
	      T->nsnapmap*sizeof(SnapEntry);
  GCtrace *T2 = (GCtrace *)lj_mem_newgco_raw(L, (MSize)sz,
					     LJ_AF_TRAVERSABLE);
  char *p = (char *)T2 + sztr;
  T2->gct = ~LJ_TTRACE;
  lj_obj_setgcflags(obj2gco(T2), 0);
  T2->traceno = 0;
  T2->ir = (IRIns *)p - T->nk;
  T2->nins = T->nins;
  T2->nk = T->nk;
  T2->nsnap = T->nsnap;
  T2->nsnapmap = T->nsnapmap;
  trace_startpt_clear(T2);
  setmref(T2->startpc, NULL);
  T2->startins = 0;
  T2->szmcode = 0;
  T2->mcode = NULL;
  T2->exittab = NULL;
  T2->exitstub = NULL;
  T2->mcloop = 0;
  T2->nchild = 0;
  T2->spadjust = 0;
  trace_link_rel(T2, 0);
  T2->root = 0;
  trace_nextroot_rel(T2, 0);
  trace_nextside_rel(T2, 0);
  T2->sinktags = 0;
  T2->topslot = 0;
  T2->linktype = 0;
  T2->unused1 = 0;
  T2->retire_epoch = 0;
  T2->retired_next = NULL;
  memcpy(p, T->ir + T->nk, szins);
  return T2;
}

static void trace_exittab_free(global_State *g, GCtrace *T)
{
  if (T->exittab) {
    lj_mem_freevec(g, T->exittab, T->nsnap, MCode *);
    T->exittab = NULL;
  }
  T->exitstub = NULL;
}

static void trace_exittab_reset(jit_State *J, GCtrace *T)
{
#if LJ_64 && defined(EXITSTUBS_PER_GROUP)
  ExitNo i;
  if (T->exittab == NULL)
    return;
  for (i = 0; i < T->nsnap; i++)
    trace_exittarget_rel(T, i, exitstub_addr(J, i));
#else
  UNUSED(J); UNUSED(T);
#endif
}

static void trace_exittab_resetroot(jit_State *J, TraceNo rootno)
{
  TraceNo i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T && (T->traceno == rootno || T->root == rootno))
      trace_exittab_reset(J, T);
  }
}

/* Save current trace by copying and compacting it. */
static void trace_save(jit_State *J, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  char *p = (char *)T + sztr;
  global_State *g = J2G(J);
  memcpy(T, &J->cur, sizeof(GCtrace));
  newwhite(g, T);
  T->gct = ~LJ_TTRACE;
  T->ir = (IRIns *)p - J->cur.nk;  /* The IR has already been copied above. */
#if LJ_ABI_PAUTH
  T->mcauth = lj_ptr_sign((ASMFunction)T->mcode, T);
#endif
  p += szins;
  TRACE_APPENDVEC(snap, nsnap, SnapShot)
  TRACE_APPENDVEC(snapmap, nsnapmap, SnapEntry)
  J->cur.traceno = 0;
  J->cur.exittab = NULL;
  J->cur.exitstub = NULL;
  J->curfinal = NULL;
  lj_gc_linkobj(g, obj2gco(T));  /* CAS-publish root after body init. */
  traceslot_publish(J, T->traceno, T);
  lj_gc_barriertrace(g, T->traceno);
  lj_gdbjit_addtrace(J, T);
#ifdef LUAJIT_USE_PERFTOOLS
  perftools_addtrace(T);
#endif
}

void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  lj_assertG(T->traceno != 0 || trace_startptgco_acq(T) != NULL ||
	     la_load64_acq(&T->retire_epoch) != 0,
	     "unpublished trace body retired");
  if (T->traceno) {
    lj_gdbjit_deltrace(J, T);
    if (T->traceno < J->freetrace)
      J->freetrace = T->traceno;
    traceslot_clear(J, T->traceno);
  }
  if (g->gc.currentwhite & LJ_GC_SFIXED) {
    trace_free_immediate(g, T);
    return;
  }
  trace_retire(g, T);
}

/* Re-enable compiling a prototype by unpatching any modified bytecode. */
void lj_trace_reenableproto(GCproto *pt)
{
  if ((pt->flags & PROTO_ILOOP)) {
    BCIns *bc = proto_bc(pt);
    BCPos i, sizebc = pt->sizebc;
    pt->flags &= ~PROTO_ILOOP;
    if (bc_op(bc[0]) == BC_IFUNCF)
      bc_publish_op(&bc[0], BC_FUNCF);
    for (i = 1; i < sizebc; i++) {
      BCOp op = bc_op(bc[i]);
      if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP)
	bc_publish_op(&bc[i], (int)op+(int)BC_LOOP-(int)BC_ILOOP);
    }
  }
}

/* Unpatch the bytecode modified by a root trace. */
static void trace_unpatch(jit_State *J, GCtrace *T)
{
  BCOp op = bc_op(T->startins);
  BCIns *pc = mref(T->startpc, BCIns);
  UNUSED(J);
  if (op == BC_JMP)
    return;  /* No need to unpatch branches in parent traces (yet). */
  switch (bc_op(*pc)) {
  case BC_JFORL:
    lj_assertJ(traceref(J, bc_d(*pc)) == T, "JFORL references other trace");
    bc_publish(pc, T->startins);
    pc += bc_j(T->startins);
    lj_assertJ(bc_op(*pc) == BC_JFORI, "FORL does not point to JFORI");
    bc_publish_op(pc, BC_FORI);
    break;
  case BC_JITERL:
  case BC_JLOOP:
    lj_assertJ(op == BC_ITERL || op == BC_ITERN || op == BC_LOOP ||
	       bc_isret(op), "bad original bytecode %d", op);
    bc_publish(pc, T->startins);
    break;
  case BC_JFUNCF:
    lj_assertJ(op == BC_FUNCF, "bad original bytecode %d", op);
    bc_publish(pc, T->startins);
    break;
  default:  /* Already unpatched. */
    break;
  }
}

/* Flush a root trace. Returns 1 iff trace-exit publication changed. */
static uint32_t trace_flushroot(jit_State *J, GCtrace *T, int scoped)
{
  GCproto *pt = trace_startpt_acq(T);
  TraceNo head;
  TraceNo nextroot = trace_nextroot_acq(T);
  uint32_t retargeted = 1;
  lj_assertJ(T->root == 0, "not a root trace");
  lj_assertJ(pt != NULL, "trace has no prototype");
  if (LJ_UNLIKELY(pt == NULL))
    return 0;
  head = proto_trace_acq(pt);
  trace_exittab_resetroot(J, T->traceno);
  if (scoped)
    la_store64_rel(&T->retire_epoch, LJ_TRACE_SCOPE_FLUSHING);
  /* Unlink root trace from chain anchored in prototype. */
  if (head == T->traceno) {  /* Trace is first in chain. Easy. */
    proto_trace_rel(pt, nextroot);
unpatch:
    /* Unpatch modified bytecode only if the trace has not been flushed. */
    trace_unpatch(J, T);
    return 1;
  } else if (head) {  /* Otherwise search in chain of root traces. */
    GCtrace *T2 = traceref(J, head);
    if (T2) {
      TraceNo next;
      for (next = trace_nextroot_acq(T2); next;
	   next = T2 ? trace_nextroot_acq(T2) : 0) {
	if (next == T->traceno) {
	  trace_nextroot_rel(T2, nextroot);  /* Unlink from chain. */
	  goto unpatch;
	}
	T2 = traceref(J, next);
      }
    }
  }
  return retargeted;
}

static int trace_scope_flushing(jit_State *J, TraceNo traceno)
{
  if (traceno > 0 && traceno < J->sizetrace) {
    GCtrace *T = traceref(J, traceno);
    return T && T->traceno == traceno &&
	   la_load64_acq(&T->retire_epoch) == LJ_TRACE_SCOPE_FLUSHING;
  }
  return 0;
}

static uint32_t trace_flushside(jit_State *J, GCtrace *T, int scoped)
{
  IRIns *base = &T->ir[REF_BASE];
  TraceNo parentno = (TraceNo)base->op1;
  GCtrace *parent = traceref(J, parentno);
  ExitNo exitno = (ExitNo)base->op2;
  lj_assertJ(T->root != 0, "not a side trace");
  trace_exittab_reset(J, T);
  if (parent && parent->traceno == parentno &&
      parent->exittab && exitno < parent->nsnap)
    trace_exittarget_rel(parent, exitno, exitstub_addr(J, exitno));
  if (scoped)
    la_store64_rel(&T->retire_epoch, LJ_TRACE_SCOPE_FLUSHING);
  return 1;
}

static int trace_scope_flush_dependency(jit_State *J, GCtrace *T)
{
  TraceNo link = trace_link_acq(T);
  if (trace_scope_flushing(J, link))
    return 1;
  if (T->root != 0) {
    TraceNo parent = (TraceNo)T->ir[REF_BASE].op1;
    if (trace_scope_flushing(J, T->root) ||
	trace_scope_flushing(J, parent))
      return 1;
  }
  return 0;
}

static uint32_t trace_flushscope_mark_deps(jit_State *J)
{
  uint32_t marked = 0, changed;
  do {
    TraceNo i;
    changed = 0;
    for (i = 1; i < J->sizetrace; i++) {
      GCtrace *T = traceref(J, i);
      if (T && T->traceno == i &&
	  la_load64_acq(&T->retire_epoch) != LJ_TRACE_SCOPE_FLUSHING &&
	  trace_scope_flush_dependency(J, T)) {
	if (T->root == 0) {
	  if (!trace_flushroot(J, T, 1))
	    continue;
	} else {
	  (void)trace_flushside(J, T, 1);
	}
	marked++;
	changed = 1;
      }
    }
  } while (changed);
  return marked;
}

/* Flush a root or side trace. Returns non-zero iff scoped work was marked. */
uint32_t lj_trace_flush(jit_State *J, TraceNo traceno)
{
  if (traceno > 0 && traceno < J->sizetrace) {
    GCtrace *T = traceref(J, traceno);
    if (T && T->traceno == traceno) {
      if (T->root == 0)
	return trace_flushroot(J, T, 1);
      return trace_flushside(J, T, 1);
    }
  }
  return 0;
}

/* Flush all traces associated with a prototype. */
uint32_t lj_trace_flushproto(global_State *g, GCproto *pt)
{
  TraceNo trace;
  uint32_t flushed = 0;
  while ((trace = proto_trace_acq(pt)) != 0) {
    GCtrace *T = traceref(G2J(g), trace);
    if (!T)
      break;
    if (!trace_flushroot(G2J(g), T, 1))
      break;
    flushed++;
  }
  return flushed;
}

static void trace_scope_clear_slot(jit_State *J, TraceNo traceno, GCtrace *T,
				   uint64_t epoch)
{
  if (T->root != 0) {
    GCtrace *root = traceref(J, T->root);
    if (root && root->traceno == T->root) {
      TraceNo next = trace_nextside_acq(T);
      TraceNo head = trace_nextside_acq(root);
      if (head == traceno) {
	trace_nextside_rel(root, next);
	if (root->nchild > 0)
	  root->nchild--;
      } else if (head != 0) {
	GCtrace *prev = traceref(J, head);
	while (prev) {
	  TraceNo prevnext = trace_nextside_acq(prev);
	  if (prevnext == traceno) {
	    trace_nextside_rel(prev, next);
	    if (root->nchild > 0)
	      root->nchild--;
	    break;
	  }
	  prev = prevnext ? traceref(J, prevnext) : NULL;
	}
      }
    }
  }
  lj_gdbjit_deltrace(J, T);
  T->traceno = 0;  /* Scoped slot retired after HS_EXIT_TRACES grace. */
  trace_link_rel(T, 0);
  trace_nextroot_rel(T, 0);
  trace_nextside_rel(T, 0);
  la_store64_rel(&T->retire_epoch, epoch);
  traceslot_clear(J, traceno);
  if (J->freetrace == 0 || traceno < J->freetrace)
    J->freetrace = traceno;
}

static uint32_t lj_trace_flushscope_retire(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  TraceNo i;
  uint32_t retired = 0;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T && T->root != 0 && T->traceno == i) {
      GCtrace *root = traceref(J, T->root);
      if (la_load64_acq(&T->retire_epoch) == LJ_TRACE_SCOPE_FLUSHING ||
	  (root && root->traceno == T->root &&
	   la_load64_acq(&root->retire_epoch) == LJ_TRACE_SCOPE_FLUSHING)) {
	trace_scope_clear_slot(J, i, T, epoch);
	retired++;
      }
    }
  }
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T && T->root == 0 && T->traceno == i &&
	la_load64_acq(&T->retire_epoch) == LJ_TRACE_SCOPE_FLUSHING) {
      trace_scope_clear_slot(J, i, T, epoch);
      retired++;
    }
  }
  if (retired)
    la_add64_rlx(&g->gc2.jit_scoped_slots_retired, retired);
  return retired;
}

/* Flush all traces. */
int lj_trace_flushall(lua_State *L)
{
  jit_State *J = L2J(L);
  ptrdiff_t i;
  if ((J2G(J)->hookmask & HOOK_GC))
    return 1;
  for (i = (ptrdiff_t)J->sizetrace-1; i > 0; i--) {
    GCtrace *T = traceref(J, i);
    if (T) {
      trace_exittab_reset(J, T);
      if (T->root == 0)
	trace_flushroot(J, T, 0);
      lj_gdbjit_deltrace(J, T);
      T->traceno = 0;  /* Blacklist the link for cont_stitch. */
      trace_link_rel(T, 0);
      traceslot_clear(J, i);
    }
  }
  J->cur.traceno = 0;
  J->freetrace = 0;
  /* Clear penalty cache. */
  memset(J->penalty, 0, sizeof(J->penalty));
  /* Free the whole machine code and invalidate all exit stub groups. */
  lj_mcode_free(J);
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
  lj_vmevent_send(J2G(J), TRACE,
    setstrV(V, V->top++, lj_str_newlit(V, "flush"));
  );
  return 0;
}

/* Request a leader-owned full trace flush through the safepoint protocol. */
int lj_trace_flushall_hs(lua_State *L)
{
  global_State *g = G(L);
  if ((g->hookmask & HOOK_GC))
    return 1;
  (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ);
  return 0;
}

void lj_trace_flushscope_hs(global_State *g, uint32_t work)
{
  if (work != 0) {
    (void)trace_flushscope_mark_deps(G2J(g));
    (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES);  /* 08 section 8.7 scoped boundary. */
    (void)lj_trace_flushscope_retire(g, la_load64_acq(&g->gc2.hs_epoch));
  }
}

uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)
{
  uint32_t work = lj_trace_flush(J, traceno);
  lj_trace_flushscope_hs(J2G(J), work);
  return work;
}

/* Initialize JIT compiler state. */
void lj_trace_initstate(global_State *g)
{
  jit_State *J = G2J(g);
  TValue *tv;

  /* Initialize aligned SIMD constants. */
  tv = LJ_KSIMD(J, LJ_KSIMD_ABS);
  tv[0].u64 = U64x(7fffffff,ffffffff);
  tv[1].u64 = U64x(7fffffff,ffffffff);
  tv = LJ_KSIMD(J, LJ_KSIMD_NEG);
  tv[0].u64 = U64x(80000000,00000000);
  tv[1].u64 = U64x(80000000,00000000);

  /* Initialize 32/64 bit constants. */
#if LJ_TARGET_X64 || LJ_TARGET_MIPS64
  J->k64[LJ_K64_M2P64].u64 = U64x(c3f00000,00000000);
#endif
#if LJ_TARGET_X86ORX64
  J->k64[LJ_K64_TOBIT].u64 = U64x(43380000,00000000);
  J->k64[LJ_K64_2P64].u64 = U64x(43f00000,00000000);
#endif
#if LJ_TARGET_MIPS64
  J->k64[LJ_K64_2P63].u64 = U64x(43e00000,00000000);
#endif
#if LJ_TARGET_MIPS
  J->k64[LJ_K64_2P31].u64 = U64x(41e00000,00000000);
#endif

#if LJ_TARGET_X86ORX64 || LJ_TARGET_MIPS64
  J->k32[LJ_K32_M2P64] = 0xdf800000;
#endif
#if LJ_TARGET_MIPS64
  J->k32[LJ_K32_2P63] = 0x5f000000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P52_2P31] = 0x59800004;
  J->k32[LJ_K32_2P52] = 0x59800000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P31] = 0x4f000000;
#endif

#if LJ_TARGET_PPC || LJ_TARGET_MIPS32
  J->k32[LJ_K32_VM_EXIT_HANDLER] = (uintptr_t)(void *)lj_vm_exit_handler;
  J->k32[LJ_K32_VM_EXIT_INTERP] = (uintptr_t)(void *)lj_vm_exit_interp;
#endif
#if LJ_TARGET_ARM64 || LJ_TARGET_MIPS64
  J->k64[LJ_K64_VM_EXIT_HANDLER].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_handler, 0);
  J->k64[LJ_K64_VM_EXIT_INTERP].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_interp, 0);
#endif
}

/* Free everything associated with the JIT compiler state. */
void lj_trace_freestate(global_State *g)
{
  jit_State *J = G2J(g);
#ifdef LUA_USE_ASSERT
  {  /* This assumes all traces have already been freed. */
    ptrdiff_t i;
    for (i = 1; i < (ptrdiff_t)J->sizetrace; i++)
      lj_assertG(i == (ptrdiff_t)J->cur.traceno || traceref(J, i) == NULL,
		 "trace still allocated");
  }
#endif
  lj_mem_freevec(g, J->snapmapbuf, J->sizesnapmap, SnapEntry);
  lj_mem_freevec(g, J->snapbuf, J->sizesnap, SnapShot);
  lj_mem_freevec(g, J->irbuf + J->irbotlim, J->irtoplim - J->irbotlim, IRIns);
  if (J->tracev)
    tracevec_free(g, J->tracev);
  lj_trace_freeretired(g);
  lj_mcode_freeall(g);
}

/* -- Penalties and blacklisting ------------------------------------------ */

/* Blacklist a bytecode instruction. */
static void blacklist_pc(GCproto *pt, BCIns *pc)
{
  if (bc_op(*pc) == BC_ITERN) {
    bc_publish_op(pc, BC_ITERC);
    bc_publish_op(pc+1+bc_j(pc[1]), BC_JMP);
  } else {
    bc_publish_op(pc, (int)bc_op(*pc)+(int)BC_ILOOP-(int)BC_LOOP);
    pt->flags |= PROTO_ILOOP;
  }
}

/* Penalize a bytecode instruction. */
static void penalty_pc(jit_State *J, GCproto *pt, BCIns *pc, TraceError e)
{
  uint32_t i, val = PENALTY_MIN;
  for (i = 0; i < PENALTY_SLOTS; i++)
    if (mref(J->penalty[i].pc, const BCIns) == pc) {  /* Cache slot found? */
      /* First try to bump its hotcount several times. */
      val = ((uint32_t)J->penalty[i].val << 1) +
	    (lj_prng_u64(&J2TG(J)->prng) & ((1u<<PENALTY_RNDBITS)-1));
      if (val > PENALTY_MAX) {
	blacklist_pc(pt, pc);  /* Blacklist it, if that didn't help. */
	return;
      }
      goto setpenalty;
    }
  /* Assign a new penalty cache slot. */
  i = J->penaltyslot;
  J->penaltyslot = (J->penaltyslot + 1) & (PENALTY_SLOTS-1);
  setmref(J->penalty[i].pc, pc);
setpenalty:
  J->penalty[i].val = (uint16_t)val;
  J->penalty[i].reason = e;
  hotcount_setg(J2G(J), pc+1, val);
}

/* -- Trace compiler state machine ---------------------------------------- */

/* Start tracing. */
static void trace_start(jit_State *J)
{
  TraceNo traceno;

  if ((J->pt->flags & PROTO_NOJIT)) {  /* JIT disabled for this proto? */
    if (J->parent == 0 && J->exitno == 0 && bc_op(*J->pc) != BC_ITERN) {
      /* Lazy bytecode patching to disable hotcount events. */
      lj_assertJ(bc_op(*J->pc) == BC_FORL || bc_op(*J->pc) == BC_ITERL ||
		 bc_op(*J->pc) == BC_LOOP || bc_op(*J->pc) == BC_FUNCF,
		 "bad hot bytecode %d", bc_op(*J->pc));
      bc_publish_op(J->pc, (int)bc_op(*J->pc)+(int)BC_ILOOP-(int)BC_LOOP);
      J->pt->flags |= PROTO_ILOOP;
    }
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }

  /* Ensuring forward progress for BC_ITERN can trigger hotcount again. */
  if (!J->parent && bc_op(*J->pc) == BC_JLOOP) {  /* Already compiled. */
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }

  /* Get a new trace number. */
  traceno = trace_findfree(J);
  if (LJ_UNLIKELY(traceno == 0)) {  /* No free trace? */
    lj_assertJ((J2G(J)->hookmask & HOOK_GC) == 0,
	       "recorder called from GC hook");
    (void)lj_trace_flushall_hs(J->L);
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }
  traceslot_pending(J, traceno);

  /* Setup enough of the current trace to be able to send the vmevent. */
  memset(&J->cur, 0, sizeof(GCtrace));
  J->cur.traceno = traceno;
  J->cur.nins = J->cur.nk = REF_BASE;
  J->cur.ir = J->irbuf;
  J->cur.snap = J->snapbuf;
  J->cur.snapmap = J->snapmapbuf;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->bcskip = 0;
  J->guardemit.irt = 0;
  J->postproc = LJ_POST_NONE;
  lj_resetsplit(J);
  J->retryrec = 0;
  J->ktrace = 0;
  trace_startpt_rel(&J->cur, J->pt);

  lj_vmevent_send_(J2G(J), TRACE,
    TValue savetv = J2TG(J)->tmptv;
    TValue savetv2 = J2TG(J)->tmptv2;
    TraceNo parent = J->parent;
    ExitNo exitno = J->exitno;
    setstrV(V, V->top++, lj_str_newlit(V, "start"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
    setintV(V->top++, proto_bcpos(J->pt, J->pc));
    if (J->parent) {
      setintV(V->top++, J->parent);
      setintV(V->top++, J->exitno);
    } else {
      BCOp op = bc_op(*J->pc);
      if (op == BC_CALLM || op == BC_CALL || op == BC_ITERC) {
	setintV(V->top++, J->exitno);  /* Parent of stitched trace. */
	setintV(V->top++, -1);
      }
    }
  ,
    J2TG(J)->tmptv = savetv;
    J2TG(J)->tmptv2 = savetv2;
    J->parent = parent;
    J->exitno = exitno;
  );
  lj_record_setup(J);
}

/* Stop tracing. */
static void trace_stop(jit_State *J)
{
  BCIns *pc = mref(J->cur.startpc, BCIns);
  BCOp op = bc_op(J->cur.startins);
  GCproto *pt = trace_startpt_acq(&J->cur);
  TraceNo traceno = J->cur.traceno;
  GCtrace *T = J->curfinal;
  BCIns *patchpc = NULL;
  BCIns patchins = 0;
  GCtrace *parent = NULL;
  GCtrace *root = NULL;
  SnapShot *snap = NULL;
  int addroot = 0;

  switch (op) {
  case BC_FORL:
    /* The matching FORI is patched after trace publication. */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
    patchpc = pc;
    patchins = BCINS_AD((int)op+(int)BC_JLOOP-(int)BC_LOOP,
			bc_a(J->cur.startins), traceno);
  addroot:
    J->cur.nextroot = (TraceNo1)proto_trace_acq(pt);
    addroot = 1;
    break;
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    patchpc = pc;
    patchins = BCINS_AD(BC_JLOOP, J->cur.snap[0].nslots, traceno);
    goto addroot;
  case BC_JMP:
    lj_assertJ(J->parent != 0 && J->cur.root != 0, "not a side trace");
    parent = traceref(J, J->parent);
    root = traceref(J, J->cur.root);
    lj_assertJ(parent != NULL && root != NULL, "missing parent/root trace");
    /* Avoid compiling a side trace twice (stack resizing uses parent exit). */
    snap = &parent->snap[J->exitno];
    J->cur.nextside = (TraceNo1)trace_nextside_acq(root);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    parent = traceref(J, J->exitno);
    lj_assertJ(parent != NULL, "missing stitched trace");
    break;
  default:
    lj_assertJ(0, "bad stop bytecode %d", op);
    break;
  }

  /* Commit and publish the final trace before enabling bytecode/exits. */
  lj_mcode_commit(J, J->cur.mcode);
  lj_mcode_sync_core(J);
  J->postproc = LJ_POST_NONE;
  trace_save(J, T);

  switch (op) {
  case BC_FORL:
    bc_publish_op(pc+bc_j(J->cur.startins), BC_JFORI);
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    if (addroot)
      proto_trace_rel(pt, traceno);
    if (patchpc)
      bc_publish(patchpc, patchins);
    break;
  case BC_JMP:
    lj_assertJ(parent->exittab != NULL, "missing parent exit table");
    trace_exittarget_rel(parent, J->exitno, T->mcode);
    snap->count = SNAPCOUNT_DONE;
    if (T->topslot > snap->topslot) snap->topslot = T->topslot;
    root->nchild++;
    trace_nextside_rel(root, traceno);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    trace_link_rel(parent, traceno);
    break;
  default:
    break;
  }

  lj_vmevent_send(J2G(J), TRACE,
    setstrV(V, V->top++, lj_str_newlit(V, "stop"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
  );
}

/* Start a new root trace for down-recursion. */
static int trace_downrec(jit_State *J)
{
  /* Restart recording at the return instruction. */
  lj_assertJ(J->pt != NULL, "no active prototype");
  lj_assertJ(bc_isret(bc_op(*J->pc)), "not at a return bytecode");
  if (bc_op(*J->pc) == BC_RETM)
    return 0;  /* NYI: down-recursion with RETM. */
  J->parent = 0;
  J->exitno = 0;
  if (lj_trace_state_aborted(lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
    return 0;
  trace_start(J);
  return 1;
}

/* Abort tracing. */
static int trace_abort(jit_State *J)
{
  lua_State *L = J->L;
  TraceError e = LJ_TRERR_RECERR;
  TraceNo traceno;

  J->postproc = LJ_POST_NONE;
  lj_mcode_abort(J);
  if (J->curfinal) {
    trace_free_immediate(J2G(J), J->curfinal);
    J->curfinal = NULL;
  }
  if (tvisnumber(L->top-1))
    e = (TraceError)numberVint(L->top-1);
  /* MCODELM retries rebuild per-trace exit stubs in a fresh mcode area. */
  trace_exittab_free(J2G(J), &J->cur);
  if (e == LJ_TRERR_MCODELM) {
    L->top--;  /* Remove error object */
    if (lj_trace_state_aborted(lj_trace_state_store_active(J, LJ_TRACE_ASM)))
      return 0;
    return 1;  /* Retry ASM with new MCode area. */
  }
  /* Penalize or blacklist starting bytecode instruction. */
  if (J->parent == 0 && !bc_isret(bc_op(J->cur.startins))) {
    if (J->exitno == 0) {
      BCIns *startpc = mref(J->cur.startpc, BCIns);
      if (e == LJ_TRERR_RETRY)
	hotcount_setg(J2G(J), startpc+1, 1);  /* Immediate retry. */
      else
	penalty_pc(J, trace_startpt_acq(&J->cur), startpc, e);
    } else {
      GCtrace *T = traceref(J, J->exitno);
      if (T)
	trace_link_rel(T, J->exitno);  /* Self-link is blacklisted. */
    }
  }

  /* Is there anything to abort? */
  traceno = J->cur.traceno;
  if (traceno) {
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    lj_vmevent_send(J2G(J), TRACE,
      cTValue *bot = tvref(L->stack)+LJ_FR2;
      cTValue *frame;
      const BCIns *pc;
      BCPos pos = 0;
      setstrV(V, V->top++, lj_str_newlit(V, "abort"));
      setintV(V->top++, traceno);
      /* Find original Lua function call to generate a better error message. */
      for (frame = L->base-1, pc = J->pc; ; frame = frame_prev(frame)) {
	if (isluafunc(frame_func(frame))) {
	  pos = proto_bcpos(funcproto(frame_func(frame)), pc);
	  break;
	} else if (frame_prev(frame) <= bot) {
	  break;
	} else if (frame_iscont(frame)) {
	  pc = frame_contpc(frame) - 1;
	} else {
	  pc = frame_pc(frame) - 1;
	}
      }
      setfuncV(V, V->top++, frame_func(frame));
      setintV(V->top++, pos);
      copyTV(V, V->top++, L->top-1);
      copyTV(V, V->top++, &J->errinfo);
    );
    /* Drop aborted trace after the vmevent (which may still access it). */
    traceslot_clear(J, traceno);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
    J->cur.traceno = 0;
  }
  L->top--;  /* Remove error object */
  if (e == LJ_TRERR_DOWNREC) {
    return trace_downrec(J);
  } else if (e == LJ_TRERR_MCODEAL) {
    if (!J->mcarea) {  /* Disable JIT compiler if first mcode alloc fails. */
      J->flags &= ~JIT_F_ON;
      lj_dispatch_update(J2G(J), 0);
    }
    (void)lj_trace_flushall_hs(L);
  }
  return 0;
}

/* Perform pending re-patch of a bytecode instruction. */
static LJ_AINLINE void trace_pendpatch(jit_State *J, int force)
{
  if (LJ_UNLIKELY(J->patchpc)) {
    if (force || J->bcskip == 0) {
      bc_publish(J->patchpc, J->patchins);
      J->patchpc = NULL;
    } else {
      J->bcskip = 0;
    }
  }
}

/* State machine for the trace compiler. Protected callback. */
static TValue *trace_state(lua_State *L, lua_CFunction dummy, void *ud)
{
  jit_State *J = (jit_State *)ud;
  UNUSED(dummy);
  do {
  retry:
    switch ((uint32_t)lj_trace_state_load(J)) {
    case LJ_TRACE_START:
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	goto retry;  /* trace_start() may change state. */
      trace_start(J);
      lj_dispatch_update(J2G(J), 0);
      if (lj_trace_state_aborted(lj_trace_state_load(J)))
	goto retry;
      if (lj_trace_state_load(J) != LJ_TRACE_RECORD_1ST)
	break;
      /* fallthrough */

    case LJ_TRACE_RECORD_1ST:
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	goto retry;
      /* fallthrough */
    case LJ_TRACE_RECORD:
      trace_pendpatch(J, 0);
      setvmstate(J2G(J), RECORD);
      lj_vmevent_send_(J2G(J), RECORD,
	/* Save/restore state for trace recorder. */
	TValue savetv = J2TG(J)->tmptv;
	TValue savetv2 = J2TG(J)->tmptv2;
	TraceNo parent = J->parent;
	ExitNo exitno = J->exitno;
	setintV(V->top++, J->cur.traceno);
	setfuncV(V, V->top++, J->fn);
	setintV(V->top++, J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1);
	setintV(V->top++, J->framedepth);
      ,
	J2TG(J)->tmptv = savetv;
	J2TG(J)->tmptv2 = savetv2;
	J->parent = parent;
	J->exitno = exitno;
      );
      lj_record_ins(J);
      break;

    case LJ_TRACE_END:
      trace_pendpatch(J, 1);
      J->loopref = 0;
      if ((J->flags & JIT_F_OPT_LOOP) &&
	  J->cur.link == J->cur.traceno && J->framedepth + J->retdepth == 0) {
	setvmstate(J2G(J), OPT);
	lj_opt_dce(J);
	if (lj_opt_loop(J)) {  /* Loop optimization failed? */
	  J->cur.link = 0;
	  J->cur.linktype = LJ_TRLINK_NONE;
	  J->loopref = J->cur.nins;
	  if (lj_trace_state_aborted(
		lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	    goto retry;  /* Try to continue recording. */
	  break;
	}
	J->loopref = J->chain[IR_LOOP];  /* Needed by assembler. */
      }
      lj_opt_split(J);
      lj_opt_sink(J);
      if (!J->loopref) J->cur.snap[J->cur.nsnap-1].count = SNAPCOUNT_DONE;
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_ASM)))
	goto retry;
      break;

    case LJ_TRACE_ASM:
      setvmstate(J2G(J), ASM);
      lj_asm_trace(J, &J->cur);
      trace_stop(J);
      setvmstate(J2G(J), INTERP);
      lj_trace_state_store(J, LJ_TRACE_IDLE);
      lj_dispatch_update(J2G(J), 0);
      lj_jit_token_release(J);
      return NULL;

    default:  /* Trace aborted asynchronously. */
      setintV(L->top++, (int32_t)LJ_TRERR_RECERR);
      /* fallthrough */
    /* lj_err_throw() clears ACTIVE for synchronous recorder errors, too. */
    case (LJ_TRACE_ERR & ~LJ_TRACE_ACTIVE):
    case LJ_TRACE_ERR:
      trace_pendpatch(J, 1);
      if (trace_abort(J))
	goto retry;
      setvmstate(J2G(J), INTERP);
      lj_trace_state_store(J, LJ_TRACE_IDLE);
      lj_dispatch_update(J2G(J), 0);
      lj_jit_token_release(J);
      return NULL;
    }
  } while (lj_trace_state_load(J) > LJ_TRACE_RECORD);
  if (lj_trace_state_aborted(lj_trace_state_load(J)))
    goto retry;
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
    lj_jit_token_release(J);
  return NULL;
}

/* -- Event handling ------------------------------------------------------ */

/* A bytecode instruction is about to be executed. Record it. */
void lj_trace_ins(jit_State *J, const BCIns *pc)
{
  /* Note: J->L must already be set. pc is the true bytecode PC here. */
  J->pc = pc;
  J->fn = curr_func(J->L);
  J->pt = isluafunc(J->fn) ? funcproto(J->fn) : NULL;
  while (lj_vm_cpcall(J->L, NULL, (void *)J, trace_state) != 0)
    lj_trace_state_store_active(J, LJ_TRACE_ERR);
}

/* A hotcount triggered. Start recording a root trace. */
#if LJ_TARGET_X64
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)
#else
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc)
#endif
{
  /* Note: pc is the interpreter bytecode PC here. It's offset by 1. */
  ERRNO_SAVE
  /* Reset hotcount. */
  hotcount_setg(J2G(J), pc, J->param[JIT_P_hotloop]*HOTCOUNT_LOOP);
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      !(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT)) &&
      lj_jit_token_try(J)) {
#if LJ_TARGET_X64
    J->L = L;
#endif
    J->parent = 0;  /* Root trace. */
    J->exitno = 0;
    if (!lj_trace_state_aborted(
	  lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc-1);
    else
      lj_jit_token_release(J);
  }
  ERRNO_RESTORE
}

/* Check for a hot side exit. If yes, start recording a side trace. */
static void trace_hotside(jit_State *J, const BCIns *pc, lua_State *L,
			  TraceNo parent, ExitNo exitno)
{
  SnapShot *snap = &traceref(J, parent)->snap[exitno];
  uint32_t hotexit = J->param[JIT_P_hotexit];
  uint8_t count;
  if (!(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT)) &&
      isluafunc(curr_func(L))) {
    count = snap->count;
    if (count == SNAPCOUNT_DONE)
      return;
    if ((uint32_t)count + 1u < hotexit) {
      snap->count = (uint8_t)(count + 1u);
      return;
    }
    if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
      return;
    if (!lj_jit_token_try(J))
      return;
    if (count < SNAPCOUNT_DONE-1)
      snap->count = (uint8_t)(count + 1u);
    J->L = L;
    J->parent = parent;
    J->exitno = exitno;
    /* J->parent is non-zero for a side trace. */
    if (!lj_trace_state_aborted(
	  lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc);
    else
      lj_jit_token_release(J);
  }
}

static int trace_poll_pending(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && la_load32_acq(&tg->poll) != 0;
}

/* Stitch a new trace to the previous trace. */
#if LJ_TARGET_X64
void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc, lua_State *L,
				 TraceNo traceno)
#else
void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc)
#endif
{
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      !(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT)) &&
      lj_jit_token_try(J)) {
#if LJ_TARGET_X64
    J->L = L;
#endif
    J->parent = 0;  /* Have to treat it like a root trace. */
#if LJ_TARGET_X64
    J->exitno = traceno;  /* Invoking trace for stitching. */
#endif
    if (!lj_trace_state_aborted(
	  lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc);
    else
      lj_jit_token_release(J);
  }
}


/* Tiny struct to pass data to protected call. */
typedef struct ExitDataCP {
  jit_State *J;
  lua_State *L;
  void *exptr;		/* Pointer to exit state. */
  TraceNo parent;	/* Exited trace. */
  ExitNo exitno;	/* Exited snapshot. */
  const BCIns *pc;	/* Restart interpreter at this PC. */
} ExitDataCP;

/* Need to protect lj_snap_restore because it may throw. */
static TValue *trace_exit_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ExitDataCP *exd = (ExitDataCP *)ud;
  /* Always catch error here and don't call error function. */
  cframe_errfunc(L->cframe) = 0;
  cframe_nres(L->cframe) = -2*LUAI_MAXSTACK*(int)sizeof(TValue);
#if LJ_TARGET_X64 && !LJ_ABI_WIN
  exd->pc = lj_snap_restore_exit(exd->J, exd->exptr, exd->L,
				 exd->parent, exd->exitno);
#else
  exd->pc = lj_snap_restore(exd->J, exd->exptr);
#endif
  UNUSED(dummy);
  return NULL;
}

#ifndef LUAJIT_DISABLE_VMEVENT
/* Push all registers from exit state. */
static void trace_exit_regs(lua_State *V, ExitState *ex)
{
  int32_t i;
  setintV(V->top++, RID_NUM_GPR);
  setintV(V->top++, RID_NUM_FPR);
  for (i = 0; i < RID_NUM_GPR; i++) {
    if (sizeof(ex->gpr[i]) == sizeof(int32_t))
      setintV(V->top++, (int32_t)ex->gpr[i]);
    else
      setnumV(V->top++, (lua_Number)ex->gpr[i]);
  }
#if !LJ_SOFTFP
  for (i = 0; i < RID_NUM_FPR; i++) {
    setnumV(V->top, ex->fpr[i]);
    if (LJ_UNLIKELY(tvisnan(V->top)))
      setnanV(V->top);
    V->top++;
  }
#endif
}
#endif

#if defined(EXITSTATE_PCREG) || (LJ_UNWIND_JIT && !EXITTRACE_VMSTATE)
/* Determine trace number from pc of exit instruction. */
static TraceNo trace_exit_find(jit_State *J, MCode *pc)
{
  TraceNo traceno;
  for (traceno = 1; traceno < J->sizetrace; traceno++) {
    GCtrace *T = traceref(J, traceno);
    if (T && pc >= T->mcode && pc < (MCode *)((char *)T->mcode + T->szmcode))
      return traceno;
  }
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

/* A trace exited. Restore interpreter state. */
#if LJ_TARGET_X64 && !LJ_ABI_WIN
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
			      TraceNo parent, ExitNo exitno)
#else
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
#endif
{
  ERRNO_SAVE
#if !(LJ_TARGET_X64 && !LJ_ABI_WIN)
  lua_State *L = J->L;
  TraceNo parent = J->parent;
  ExitNo exitno = J->exitno;
#else
  TGState *tg = J2TG(J);
#endif
  ExitState *ex = (ExitState *)exptr;
  ExitDataCP exd;
  int errcode;
#if LJ_TARGET_X64 && !LJ_ABI_WIN
  int exitcode = tg->jit_exitcode;
#else
  int exitcode = J->exitcode;
#endif
  TValue exiterr;
  const BCIns *pc, *retpc;
  void *cf;
  GCtrace *T;

  setnilV(&exiterr);
  if (exitcode) {  /* Trace unwound with error code. */
#if LJ_TARGET_X64 && !LJ_ABI_WIN
    tg->jit_exitcode = 0;
#else
    J->exitcode = 0;
#endif
    copyTV(L, &exiterr, L->top-1);
  }

#ifdef EXITSTATE_PCREG
  parent = trace_exit_find(J, (MCode *)(intptr_t)ex->gpr[EXITSTATE_PCREG]);
#else
  UNUSED(ex);
#endif
  T = traceref(J, parent); UNUSED(T);
#ifdef EXITSTATE_CHECKEXIT
  if (exitno == T->nsnap) {  /* Treat stack check like a parent exit. */
    lj_assertJ(T->root != 0, "stack check in root trace");
    exitno = T->ir[REF_BASE].op2;
    parent = T->ir[REF_BASE].op1;
    T = traceref(J, parent);
  }
#endif
  lj_assertJ(T != NULL && exitno < T->nsnap, "bad trace or exit number");
  exd.J = J;
  exd.L = L;
  exd.exptr = exptr;
  exd.parent = parent;
  exd.exitno = exitno;
  errcode = lj_vm_cpcall(L, NULL, &exd, trace_exit_cp);
  if (errcode)
    return -errcode;  /* Return negated error code. */

  if (exitcode) copyTV(L, L->top++, &exiterr);  /* Anchor the error object. */

  if (!(LJ_HASPROFILE && (G(L)->hookmask & HOOK_PROFILE)))
    lj_vmevent_send(G(L), TEXIT,
      lj_state_checkstack(V, 4+RID_NUM_GPR+RID_NUM_FPR+LUA_MINSTACK);
      setintV(V->top++, parent);
      setintV(V->top++, exitno);
      trace_exit_regs(V, ex);
    );

  pc = exd.pc;
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (exitcode) {
    return -exitcode;
  } else if (LJ_HASPROFILE && (G(L)->hookmask & HOOK_PROFILE)) {
    /* Just exit to interpreter. */
  } else if (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize) {
    if (!(G(L)->hookmask & HOOK_GC))
      lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
  } else if ((J->flags & JIT_F_ON) && !trace_poll_pending(L)) {
    trace_hotside(J, pc, L, parent, exitno);
  }
  /* Return MULTRES or 0 or -17. */
  ERRNO_RESTORE
  switch (bc_op(*pc)) {
  case BC_CALLM: case BC_CALLMT:
    return (int)((BCReg)(L->top - L->base) - bc_a(*pc) - bc_c(*pc) - LJ_FR2);
  case BC_RETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc) - bc_d(*pc));
  case BC_TSETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc));
  case BC_JLOOP:
    retpc = &traceref(J, bc_d(*pc))->startins;
    if (bc_isret(bc_op(*retpc)) || bc_op(*retpc) == BC_ITERN) {
      /* Dispatch to original ins to ensure forward progress. */
      if (lj_trace_state_load(J) != LJ_TRACE_RECORD) return -17;
      /* Unpatch bytecode when recording. */
      J->patchins = *pc;
      J->patchpc = (BCIns *)pc;
      bc_publish(J->patchpc, *retpc);
      J->bcskip = 1;
    }
    return 0;
  default:
    if (bc_isfunc_or_ff(bc_op(*pc)))
      return (int)((BCReg)(L->top - L->base) + 1);
    return 0;
  }
}

#if LJ_UNWIND_JIT
/* Given an mcode address determine trace exit address for unwinding. */
uintptr_t LJ_FASTCALL lj_trace_unwind(jit_State *J, uintptr_t addr, ExitNo *ep)
{
#if EXITTRACE_VMSTATE
  TGState *tg = J2TG(J);
  TraceNo traceno = tg ?
    (TraceNo)(int32_t)la_load32_acq((uint32_t *)&tg->vmstate) :
    (TraceNo)J2G(J)->vmstate;
#else
  TraceNo traceno = trace_exit_find(J, (MCode *)addr);
#endif
  GCtrace *T = traceref(J, traceno);
  if (T
#if EXITTRACE_VMSTATE
      && addr >= (uintptr_t)T->mcode && addr < (uintptr_t)T->mcode + T->szmcode
#endif
     ) {
    SnapShot *snap = T->snap;
    SnapNo lo = 0, exitno = T->nsnap;
    uintptr_t ofs = (uintptr_t)((MCode *)addr - T->mcode);  /* MCode units! */
    /* Rightmost binary search for mcode offset to determine exit number. */
    do {
      SnapNo mid = (lo+exitno) >> 1;
      if (ofs < snap[mid].mcofs) exitno = mid; else lo = mid + 1;
    } while (lo < exitno);
    exitno--;
    *ep = exitno;
#ifdef exitstub_trace_addr
    return (uintptr_t)exitstub_trace_addr(T, exitno);
#elif defined(EXITSTUBS_PER_GROUP)
    return (uintptr_t)exitstub_addr(J, exitno);
#endif
  }
  /* Cannot correlate addr with trace/exit. This will be fatal. */
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

#endif
