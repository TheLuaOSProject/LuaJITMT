/* Verify that an open CALLM consumer identity exit restores the producer's
** complete result vector and resumes in the interpreter with the right frame. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_bc.h"
#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

static unsigned cases_run;
static unsigned captured_cases_run;

typedef struct CapturedClone {
  GCtrace semantic;
  GCtrace owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, ktrace;
} CapturedClone;

typedef struct CapturedJSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef1 ktrace;
} CapturedJSave;

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn=funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L,1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner,
                         const char *producer, const char *consumer, int n)
{
  lua_Number result;
  TValue *top=L->top;
  TValue *base=L->base;
  void *cframe=L->cframe;
  lua_getglobal(L,runner); lua_getglobal(L,producer);
  lua_getglobal(L,consumer); lua_pushinteger(L,n);
  ljt_lua_pcall(L,3,1,"open CALLM consumer exit");
  assert(lua_isnumber(L,-1)); result=lua_tonumber(L,-1); lua_pop(L,1);
  assert(L->top==top && L->base==base && L->cframe==cframe &&
         lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static lua_Number invoke_captured(lua_State *L, const char *runner,
                                 const char *producer, int n)
{
  lua_Number result;
  TValue *top=L->top, *base=L->base;
  void *cframe=L->cframe;
  lua_getglobal(L,runner); lua_getglobal(L,producer); lua_pushinteger(L,n);
  ljt_lua_pcall(L,2,1,"captured open CALLM consumer exit");
  assert(lua_isnumber(L,-1)); result=lua_tonumber(L,-1); lua_pop(L,1);
  assert(L->top==top && L->base==base && L->cframe==cframe &&
         lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt=funcproto(global_function(L,"open_run"));
  BCIns *bc=proto_bc(pt);
  for (MSize i=1;i<pt->sizebc;i++) {
    BCIns ins=(BCIns)la_load32_acq((uint32_t *)&bc[i]);
    if (bc_op(ins)==BC_JFORL) {
      GCtrace *T=traceref_safe(L2J(L),bc_d(ins));
      if (T && trace_runnable_acq(T,bc_d(ins)) && !T->root &&
          trace_startpt_acq(T)==pt && T->linktype==LJ_TRLINK_LOOP &&
          T->link==T->traceno) return T;
    }
  }
  return NULL;
}

static GCtrace *root_trace_named(lua_State *L, const char *name)
{
  GCproto *pt=funcproto(global_function(L,name));
  BCIns *bc=proto_bc(pt);
  for (MSize i=1;i<pt->sizebc;i++) {
    BCIns ins=(BCIns)la_load32_acq((uint32_t *)&bc[i]);
    if (bc_op(ins)==BC_JFORL) {
      GCtrace *T=traceref_safe(L2J(L),bc_d(ins));
      if (T && trace_runnable_acq(T,bc_d(ins)) && !T->root &&
          trace_startpt_acq(T)==pt && T->linktype==LJ_TRLINK_LOOP &&
          T->link==T->traceno) return T;
    }
  }
  return NULL;
}

static IRRef captured_semantic_nins(const GCtrace *T)
{
  const IRIns *ir=trace_ir_acq(T);
  IRRef nins=trace_nins_acq((GCtrace *)T);
  while (nins>REF_BASE) {
    IRIns tail=ir_load_acq(&ir[nins-1u]);
    if (tail.o==IR_RENAME && tail.t.irt==IRT_NIL && tail.op1>=REF_FIRST &&
        tail.op1<nins-1u && tail.op2<trace_nsnap_acq((GCtrace *)T) &&
        !tail.s) { nins--; continue; }
    if (tail.o==IR_NOP && tail.t.irt==IRT_NIL && !tail.op1 && !tail.op2 &&
        !tail.prev) { nins--; continue; }
    break;
  }
  return nins;
}

static IRRef captured_ktrace(const GCtrace *T)
{
  IRRef ref, nk=trace_nk_acq((GCtrace *)T), fallback=0;
  const IRIns *ir=trace_ir_acq(T);
  for (ref=nk;ref<REF_TRUE;) {
    IRIns k=ir_load_acq(&ir[ref]);
    if (k.o==IR_KGC && ir_kgc_load_acq(&ir[ref])==obj2gco(T)) return ref;
    if (k.o==IR_KNUM && !fallback) fallback=ref;
    if (k.o==IR_KNUM || k.o==IR_KINT64 ||
        (LJ_GC64 && (k.o==IR_KGC || k.o==IR_KPTR || k.o==IR_KKPTR)))
      ref+=2u;
    else if (k.o==IR_KINT || k.o==IR_KGC || k.o==IR_KPTR ||
             k.o==IR_KKPTR || k.o==IR_KPRI || k.o==IR_KNULL) ref++;
    else return 0;
  }
  return fallback;
}

static void captured_clone_init(CapturedClone *c, GCtrace *T)
{
  IRRef nk=trace_nk_acq(T), n=trace_nins_acq(T);
  memset(c,0,sizeof(*c));
  c->nins=captured_semantic_nins(T); c->ktrace=captured_ktrace(T);
  assert(c->ktrace && c->nins<=n);
  c->ir=(IRIns *)calloc((size_t)n+1u,sizeof(IRIns));
  c->snap=(SnapShot *)calloc(trace_nsnap_acq(T),sizeof(SnapShot));
  c->snapmap=(SnapEntry *)calloc(trace_nsnapmap_acq(T),sizeof(SnapEntry));
  assert(c->ir && c->snap && c->snapmap);
  memcpy(&c->ir[nk],&trace_ir_acq(T)[nk],(size_t)(n-nk)*sizeof(IRIns));
  memcpy(c->snap,trace_snap_acq(T),
         (size_t)trace_nsnap_acq(T)*sizeof(SnapShot));
  memcpy(c->snapmap,trace_snapmap_acq(T),
         (size_t)trace_nsnapmap_acq(T)*sizeof(SnapEntry));
  c->owner=*T; c->owner.ir=c->ir; c->owner.snap=c->snap;
  c->owner.snapmap=c->snapmap;
  c->semantic=c->owner; c->semantic.nins=c->nins;
  c->ir[c->ktrace].o=IR_KNUM; c->ir[c->ktrace].t.irt=IRT_NUM;
  c->ir[c->ktrace].op12=0; c->ir[c->ktrace].prev=0;
  c->ir[c->ktrace+1u].tv.u64=0;
}

static void captured_clone_reset(CapturedClone *c, GCtrace *T)
{
  IRRef nk=trace_nk_acq(T), n=trace_nins_acq(T);
  memcpy(&c->ir[nk],&trace_ir_acq(T)[nk],(size_t)(n-nk)*sizeof(IRIns));
  memcpy(c->snap,trace_snap_acq(T),
         (size_t)trace_nsnap_acq(T)*sizeof(SnapShot));
  memcpy(c->snapmap,trace_snapmap_acq(T),
         (size_t)trace_nsnapmap_acq(T)*sizeof(SnapEntry));
  c->owner=*T; c->owner.ir=c->ir; c->owner.snap=c->snap;
  c->owner.snapmap=c->snapmap;
  c->semantic=c->owner; c->semantic.nins=c->nins;
  c->ir[c->ktrace].o=IR_KNUM; c->ir[c->ktrace].t.irt=IRT_NUM;
  c->ir[c->ktrace].op12=0; c->ir[c->ktrace].prev=0;
  c->ir[c->ktrace+1u].tv.u64=0;
}

static void captured_clone_free(CapturedClone *c)
{
  free(c->ir); free(c->snap); free(c->snapmap); memset(c,0,sizeof(*c));
}

static CapturedJSave captured_enter_semantic(jit_State *J, CapturedClone *c,
                                             GCtrace *T)
{
  CapturedJSave s={J->curfinal,J->pt,J->startpc,J->loopref,J->parent,
    J->exitno,J->baseslot,J->framedepth,J->retdepth,J->ktrace};
  J->curfinal=NULL; J->pt=trace_startpt_acq(T);
  J->startpc=trace_startpc_acq(T); J->loopref=0;
  for (IRRef r=REF_FIRST;r<c->nins;r++)
    if (ir_load_acq(&c->ir[r]).o==IR_LOOP) { J->loopref=r; break; }
  J->parent=0; J->exitno=0; J->baseslot=1u+LJ_FR2;
  J->framedepth=0; J->retdepth=0; J->ktrace=c->ktrace;
  return s;
}

static void captured_leave_semantic(jit_State *J, const CapturedJSave *s)
{
  J->curfinal=s->curfinal; J->pt=s->pt; J->startpc=s->startpc;
  J->loopref=s->loopref; J->parent=s->parent; J->exitno=s->exitno;
  J->baseslot=s->baseslot; J->framedepth=s->framedepth;
  J->retdepth=s->retdepth; J->ktrace=s->ktrace;
}

static int captured_validate(lua_State *L, GCtrace *T, CapturedClone *c,
                             int postra)
{
  jit_State *J=L2J(L);
  int ok;
  if (!postra) {
    CapturedJSave save=captured_enter_semantic(J,c,T);
    LJArm64IRReject reject;
    MSize index=(MSize)((trace_startpc_acq(T)-proto_bc(J->pt)));
    BCIns original=(BCIns)la_load32_acq((const uint32_t *)&proto_bc(J->pt)[index]);
    bc_publish((const uint32_t *)&proto_bc(J->pt)[index],trace_startins_acq(T));
    ok=lj_asm_arm64_ir_admit(J,&c->semantic,&reject);
    bc_publish((const uint32_t *)&proto_bc(J->pt)[index],original);
    captured_leave_semantic(J,&save);
  } else {
    LJArm64PostRAView v={0}; IRRef n=0;
    v.ir=c->ir; v.owner=&c->owner; v.cts=ctype_ctsG(G(L));
    v.snap=c->snap; v.snapmap=c->snapmap;
    v.proto_bc=proto_bc(trace_startpt_acq(T)); v.proto=trace_startpt_acq(T);
    v.startpc=trace_startpc_acq(T); v.nins=trace_nins_acq(T);
    v.nk=trace_nk_acq(T); v.nsnap=trace_nsnap_acq(T);
    v.nsnapmap=trace_nsnapmap_acq(T); v.spadjust=trace_spadjust_acq(T);
    v.proto_sizebc=v.proto->sizebc; v.proto_numparams=v.proto->numparams;
    v.root_topslot=trace_topslot_acq(T); v.startins=trace_startins_acq(T);
    ok=lj_asm_arm64_postra_admit(&v,&n);
  }
  return ok;
}

static SnapNo consumer_identity_exit(GCtrace *T, GCfunc *consumer,
                                     MSize callmpos)
{
  GCproto *pt=trace_startpt_acq(T);
  for (SnapNo s=0;s<T->nsnap;s++) {
    SnapShot *snap=&T->snap[s];
    const BCIns *pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    IRRef r=snap_ref_acq(snap);
    IRIns guard;
    if (pc!=proto_bc(pt)+callmpos || r<REF_FIRST || r>=T->nins) continue;
    guard=ir_load_acq(&T->ir[r]);
    if (guard.o==IR_EQ && guard.t.irt==(IRT_FUNC|IRT_GUARD) &&
        irref_isk(guard.op2) && T->ir[guard.op2].o==IR_KGC &&
        ir_kgc_acq(&T->ir[guard.op2])==obj2gco(consumer))
      return s;
  }
  return UINT16_MAX;
}

static MSize callm_position(GCproto *pt, unsigned nresults)
{
  BCIns *bc=proto_bc(pt);
  MSize found=UINT32_MAX;
  assert(nresults>=1u && nresults<=3u);
  for (MSize p=1;p+1u<pt->sizebc;p++) {
    BCIns ins=(BCIns)la_load32_acq((uint32_t *)&bc[p]);
    if (bc_op(ins)==BC_CALLM) {
      assert(found==UINT32_MAX);
      found=p;
      assert(bc_b(ins)==2u && bc_c(ins)==0u);
      assert(bc_op(bc[p-1u])==BC_CALL && bc_b(bc[p-1u])==0u);
      assert(bc_a(bc[p-1u])==bc_a(ins)+1u+LJ_FR2);
    }
  }
  assert(found!=UINT32_MAX);
  return found;
}

static void assert_open_vector_snapshot(GCtrace *T, SnapNo exit,
					MSize callmpos, unsigned nresults)
{
  GCproto *pt=trace_startpt_acq(T);
  BCIns callm=proto_bc(pt)[callmpos];
  SnapShot *snap=&T->snap[exit];
  MSize firstarg=1u+LJ_FR2+bc_a(callm)+1u+LJ_FR2;
  for (unsigned result=0;result<nresults;result++) {
    unsigned found=0;
    MSize slot=firstarg+result;
    for (MSize i=0;i<snap->nent;i++) {
      SnapEntry entry=T->snapmap[snap->mapofs+i];
      IRRef ref;
      IRIns value;
      if (snap_slot(entry)!=slot) continue;
      assert(!(entry&(SNAP_NORESTORE|SNAP_FRAME)));
      ref=snap_ref(entry);
      assert((ref>=REF_FIRST && ref<T->nins) || irref_isk(ref));
      value=ir_load_acq(&T->ir[ref]);
      if (irref_isk(ref))
        assert(value.o==IR_KNUM || value.o==IR_KINT);
      assert(irt_type(value.t)==IRT_NUM || irt_type(value.t)==IRT_INT);
      found++;
    }
    assert(found==1u && "each open producer result must be restored at CALLM");
  }
}

static void check_identity_exit(lua_State *L, GCtrace *T, SnapNo expected_exit,
                                const char *consumer, MCode *mcode,
                                MSize szmcode, uint64_t generation,
                                const void *mcode_bytes)
{
  lua_Number expected=invoke(L,"open_oracle","open_oracle_leaf",consumer,64);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf",consumer,64)==expected);
  assert(root_trace(L)==T && trace_runnable_acq(T,T->traceno));
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno &&
         lj_trace_test_first_exitno()==expected_exit);
  assert(T->arm64_generation==generation &&
         trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode &&
         memcmp(trace_mcode_acq(T),mcode_bytes,szmcode)==0);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void run_case_shape(lua_State *L, unsigned nresults,
			   const char *params, const char *body,
			   const char *alternate)
{
  const char *producer[] = {"return x", "return x,.25", "return x,.25,.5"};
  GCtrace *T=NULL;
  GCproto *pt;
  GCfunc *consumer_a;
  SnapNo exit;
  MSize callmpos;
  MCode *mcode;
  MSize szmcode;
  uint64_t generation;
  void *mcode_bytes;
  lua_Number expected;
  char install[1024];
  assert(nresults>=1u && nresults<=3u);
  snprintf(install,sizeof(install),
    "install_open_exit_case(%u,\"%s\",\"%s\",\"%s\",\"%s\")", nresults,
    producer[nresults-1u],body,alternate,params);
  ljt_lua_dostring(L,install);
  consumer_a=global_function(L,"open_consumer_a");
  pt=funcproto(global_function(L,"open_run"));
  callmpos=callm_position(pt,nresults);
  for (unsigned attempt=0;attempt<8u && !T;attempt++) {
    (void)invoke(L,"open_oracle","open_oracle_leaf","open_consumer_a",96);
    (void)invoke(L,"open_run","open_leaf","open_consumer_a",96);
    T=root_trace(L);
  }
  assert(T);
  exit=consumer_identity_exit(T,consumer_a,callmpos);
  assert(exit!=UINT16_MAX);
  assert_open_vector_snapshot(T,exit,callmpos,nresults);
  mcode=trace_mcode_acq(T); szmcode=trace_szmcode_acq(T);
  assert(mcode!=NULL && szmcode!=0);
  generation=T->arm64_generation;
  mcode_bytes=malloc(szmcode);
  assert(mcode_bytes!=NULL);
  memcpy(mcode_bytes,mcode,szmcode);

  /* The first candidate call must leave native code at its CALLM identity
  ** guard after the producer CALL returned its complete open result vector.
  ** The interpreter continuation consumes that vector using the new callee. */
  expected=invoke(L,"open_oracle","open_oracle_leaf","open_consumer_b",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf","open_consumer_b",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno &&
         lj_trace_test_first_exitno()==exit);
  assert(T->arm64_generation==generation &&
         trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode &&
         memcmp(trace_mcode_acq(T),mcode_bytes,szmcode)==0 &&
         root_trace(L)==T && trace_runnable_acq(T,T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));

  /* Restore the original consumer, then require native reentry into the same
  ** selected mcode generation both before and after a full collection. */
  expected=invoke(L,"open_oracle","open_oracle_leaf","open_consumer_a",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf","open_consumer_a",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 && root_trace(L)==T &&
         T->arm64_generation==generation && trace_mcode_acq(T)==mcode &&
         trace_szmcode_acq(T)==szmcode &&
         memcmp(trace_mcode_acq(T),mcode_bytes,szmcode)==0);
  lua_gc(L,LUA_GCCOLLECT,0);
  check_identity_exit(L,T,exit,"open_consumer_b",mcode,szmcode,
                      generation,mcode_bytes);
  expected=invoke(L,"open_oracle","open_oracle_leaf","open_consumer_a",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf","open_consumer_a",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 && root_trace(L)==T &&
         T->arm64_generation==generation && trace_mcode_acq(T)==mcode &&
         trace_szmcode_acq(T)==szmcode &&
         memcmp(trace_mcode_acq(T),mcode_bytes,szmcode)==0);
  free(mcode_bytes);
  cases_run++;
  printf("open CALLM consumer exit results=%u exact-exit=%u same-mcode GC/reentry passed\n",
         nresults,(unsigned)exit);
}

static void run_case(lua_State *L, unsigned nresults)
{
  static const char *body[] = {
    "return a+1", "return a+b+2", "return a+b+c+3"
  };
  static const char *alternate[] = {
    "return a+17", "return a+101*b+17", "return a+101*b+10007*c+17"
  };
  static const char *params[] = {"a", "a,b", "a,b,c"};
  assert(nresults >= 1u && nresults <= 3u);
  run_case_shape(L,nresults,params[nresults-1u],body[nresults-1u],
                 alternate[nresults-1u]);
}

static SnapNo captured_replay_snapshot(GCtrace *T, MSize callpos,
                                       BCReg source_reg)
{
  GCproto *pt=trace_startpt_acq(T);
  IRRef loopref=0;
  SnapNo best=UINT16_MAX;
  for (IRRef r=REF_FIRST;r<T->nins;r++)
    if (ir_load_acq(&T->ir[r]).o==IR_LOOP) { loopref=r; break; }
  assert(loopref);
  for (SnapNo s=0;s<T->nsnap;s++) {
    SnapShot *snap=&T->snap[s];
    uint64_t footer;
    uintptr_t pc, lo=(uintptr_t)proto_bc(pt), hi=(uintptr_t)(proto_bc(pt)+pt->sizebc);
    MSize pos;
    int has_source=0;
    if (snap->ref<=loopref || snap->mapofs>T->nsnapmap ||
        snap->nent>T->nsnapmap-snap->mapofs ||
        T->nsnapmap-snap->mapofs-snap->nent<2u) continue;
    memcpy(&footer,&T->snapmap[snap->mapofs+snap->nent],sizeof(footer));
    pc=(uintptr_t)(footer>>8);
    if ((uint8_t)footer || pc<lo || pc>=hi ||
        (pc-lo)%sizeof(BCIns)) continue;
    pos=(MSize)((pc-lo)/sizeof(BCIns));
    if (pos>callpos) continue;
    for (MSize i=0;i<snap->nent;i++)
      if (snap_slot(T->snapmap[snap->mapofs+i])==1u+LJ_FR2+source_reg)
        has_source=1;
    if (has_source && (best==UINT16_MAX || snap->ref>T->snap[best].ref)) best=s;
  }
  return best;
}

static IRRef captured_preloop_ref(GCtrace *T, MSize absolute_slot,
                                  IRRef current_ref)
{
  IRRef loopref=0, bestref=0;
  SnapNo best=UINT16_MAX;
  for (IRRef r=REF_FIRST;r<T->nins;r++)
    if (ir_load_acq(&T->ir[r]).o==IR_LOOP) { loopref=r; break; }
  assert(loopref);
  for (SnapNo s=0;s<T->nsnap;s++) {
    SnapShot *snap=&T->snap[s];
    if (snap->ref>loopref || snap->mapofs>T->nsnapmap ||
        snap->nent>T->nsnapmap-snap->mapofs) continue;
    for (MSize i=0;i<snap->nent;i++) {
      SnapEntry entry=T->snapmap[snap->mapofs+i];
      IRRef value=snap_ref(entry);
      if (!(entry&0x00ff0000u) && snap_slot(entry)==absolute_slot &&
          value>=REF_FIRST && value<loopref && value!=current_ref &&
          (best==UINT16_MAX || snap->ref>T->snap[best].ref)) {
        best=s; bestref=value;
      }
    }
  }
  return bestref;
}

static void captured_expect(CapturedClone *c, lua_State *L, GCtrace *T,
                            int expected, int mutation)
{
  int sem, post;
  sem=captured_validate(L,T,c,0);
  post=captured_validate(L,T,c,1);
  if (sem!=expected || post!=expected)
    fprintf(stderr,"captured CALLM mutation=%d expected=%d semantic=%d postRA=%d\n",
            mutation,expected,sem,post);
  assert(sem==expected && post==expected);
}

static void captured_admission_cases(lua_State *L, GCtrace *T,
                                     MSize callmpos, unsigned nresults)
{
  GCproto *pt=trace_startpt_acq(T);
  CapturedClone c;
  BCIns *bc=proto_bc(pt);
  SnapNo out_exit=consumer_identity_exit(T,global_function(L,"captured_consumer_a"),callmpos);
  BCIns source;
  BCReg source_reg;
  SnapNo replay;
  MSize mapindex=UINT32_MAX, outindex=UINT32_MAX;
  assert(out_exit!=UINT16_MAX && callmpos>=3u);
  captured_clone_init(&c,T);
  captured_clone_reset(&c,T);
  captured_expect(&c,L,T,1,0);

  /* Wrong source local: preserve the arithmetic bytecode but change its
  ** current-iteration CGET to read the captured function parameter. */
  source=(BCIns)la_load32_acq((const uint32_t *)&bc[callmpos-3u]);
  assert(bc_op(source)==BC_CGET);
  source_reg=bc_d(source);
  {
    BCIns bad=BCINS_AD(BC_CGET,bc_a(source),1u+LJ_FR2);
    captured_clone_reset(&c,T);
    bc_publish((const uint32_t *)&bc[callmpos-3u],bad);
    captured_expect(&c,L,T,0,1);
    bc_publish((const uint32_t *)&bc[callmpos-3u],source);
  }
  /* Clobber the producer function's unchanged formal before CALL. */
  {
    BCIns producer=(BCIns)la_load32_acq((const uint32_t *)&bc[callmpos-1u]);
    BCIns bad;
    assert(bc_op(producer)==BC_CALL);
    bad=BCINS_AD(BC_CGET,bc_a(producer),bc_d(source));
    captured_clone_reset(&c,T);
    bc_publish((const uint32_t *)&bc[callmpos-3u],bad);
    captured_expect(&c,L,T,0,2);
    bc_publish((const uint32_t *)&bc[callmpos-3u],source);
  }

  /* Corrupt the source snapshot entry that supplies this exact CGET. */
  replay=captured_replay_snapshot(T,callmpos-1u,source_reg);
  assert(replay!=UINT16_MAX);
  for (MSize i=0;i<c.snap[replay].nent;i++)
    if (snap_slot(c.snapmap[c.snap[replay].mapofs+i])==
        1u+LJ_FR2+source_reg) {
      assert(mapindex==UINT32_MAX); mapindex=c.snap[replay].mapofs+i;
    }
  assert(mapindex!=UINT32_MAX);
  /* Alter the source-producing IR reference independently of the map row. */
  captured_clone_reset(&c,T);
  {
    IRRef source_ref=snap_ref(c.snapmap[mapindex]);
    IRRef previous_ref;
    assert(source_ref>=REF_FIRST && source_ref<T->nins);
    previous_ref=captured_preloop_ref(T,snap_slot(c.snapmap[mapindex]),source_ref);
    assert(previous_ref && previous_ref!=source_ref);
    c.ir[source_ref].op1=REF_NIL;
    captured_expect(&c,L,T,0,3);
    captured_clone_reset(&c,T);
    c.snapmap[mapindex]=SNAP(snap_slot(c.snapmap[mapindex]),0,previous_ref);
    captured_expect(&c,L,T,0,4);
  }
  /* Replace the exact mapped source value with nil. */
  {
    SnapEntry saved=c.snapmap[mapindex];
    captured_clone_reset(&c,T);
    saved=c.snapmap[mapindex];
    c.snapmap[mapindex]=SNAP(snap_slot(saved),0,REF_NIL);
    captured_expect(&c,L,T,0,5);
    c.snapmap[mapindex]=saved;
  }

  /* Corrupt the mapped producer vector at the exact consumer identity exit. */
  {
    BCIns callm=(BCIns)la_load32_acq((const uint32_t *)&bc[callmpos]);
    MSize slot=1u+LJ_FR2+bc_a(callm)+1u+LJ_FR2;
    captured_clone_reset(&c,T);
    for (MSize i=0;i<c.snap[out_exit].nent;i++)
      if (snap_slot(c.snapmap[c.snap[out_exit].mapofs+i])==slot) {
        assert(outindex==UINT32_MAX); outindex=c.snap[out_exit].mapofs+i;
      }
    assert(outindex!=UINT32_MAX);
    {
      SnapEntry saved=c.snapmap[outindex];
      c.snapmap[outindex]=SNAP(snap_slot(saved),0,REF_NIL);
      captured_expect(&c,L,T,0,4);
      c.snapmap[outindex]=saved;
    }
  }
  captured_clone_free(&c);
  captured_cases_run++;
  printf("captured CALLM source/map negatives results=%u passed\n",nresults);
}

static void run_captured_case(lua_State *L,unsigned nresults)
{
  const char *producer[]={"return x","return x,.25","return x,.25,.5"};
  const char *body[]={"return a+1","return a+b+2","return a+b+c+3"};
  const char *alt[]={"return a+17","return a+101*b+17","return a+101*b+10007*c+17"};
  const char *params[]={"a","a,b","a,b,c"};
  char install[1200];
  GCtrace *T=NULL;
  GCproto *pt;
  MSize callpos;
  GCfunc *consumer;
  SnapNo exit;
  MCode *mcode;
  MSize szmcode;
  uint64_t generation;
  void *bytes;
  lua_Number expected;
  snprintf(install,sizeof(install),"install_open_exit_case(%u,\"%s\",\"%s\",\"%s\",\"%s\")",
           nresults,producer[nresults-1u],body[nresults-1u],alt[nresults-1u],
           params[nresults-1u]);
  ljt_lua_dostring(L,install);
  consumer=global_function(L,"captured_consumer_a");
  pt=funcproto(global_function(L,"captured_run"));
  callpos=callm_position(pt,nresults);
  for (unsigned attempt=0;attempt<8u && !T;attempt++) {
    (void)invoke_captured(L,"captured_oracle","captured_oracle_leaf",96);
    (void)invoke_captured(L,"captured_run","captured_leaf",96);
    T=root_trace_named(L,"captured_run");
  }
  assert(T);
  exit=consumer_identity_exit(T,consumer,callpos);
  assert(exit!=UINT16_MAX);
  assert_open_vector_snapshot(T,exit,callpos,nresults);
  captured_admission_cases(L,T,callpos,nresults);
  mcode=trace_mcode_acq(T); szmcode=trace_szmcode_acq(T);
  assert(mcode && szmcode);
  generation=T->arm64_generation;
  bytes=malloc(szmcode); assert(bytes); memcpy(bytes,mcode,szmcode);
  ljt_lua_dostring(L,"captured_set(captured_consumer_b); captured_oracle_set(captured_oracle_consumer_b)");
  expected=invoke_captured(L,"captured_oracle","captured_oracle_leaf",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke_captured(L,"captured_run","captured_leaf",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 && lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno &&
         lj_trace_test_first_exitno()==exit);
  assert(root_trace_named(L,"captured_run")==T &&
         T->arm64_generation==generation && trace_mcode_acq(T)==mcode &&
         memcmp(trace_mcode_acq(T),bytes,szmcode)==0);
  ljt_lua_dostring(L,"captured_set(captured_consumer_a); captured_oracle_set(captured_oracle_consumer_a)");
  lua_gc(L,LUA_GCCOLLECT,0);
  expected=invoke_captured(L,"captured_oracle","captured_oracle_leaf",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke_captured(L,"captured_run","captured_leaf",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 &&
         root_trace_named(L,"captured_run")==T &&
         T->arm64_generation==generation && trace_mcode_acq(T)==mcode &&
         trace_szmcode_acq(T)==szmcode &&
         memcmp(trace_mcode_acq(T),bytes,szmcode)==0);
  free(bytes);
  printf("captured CALLM current-iteration args results=%u exit=%u native reuse/GC passed\n",
         nresults,(unsigned)exit);
}

int main(void)
{
  lua_State *L=ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); "
      "assert(select(2,b:recv(60))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "function install_open_exit_case(n,producer,body,alt,params) "
      "open_consumer_a=assert(loadstring('return function('..params..') '..body..' end'," 
        "'@open-consumer-a'))(); "
      "open_consumer_b=assert(loadstring('return function('..params..') '..alt..' end'," 
        "'@open-consumer-b'))(); "
      "local run_src='return function(fn,consumer,count) local total=.5; "
        "for i=1,count do total=total+consumer(fn(i+.5)) end; "
        "return total end'; "
      "open_run=assert(loadstring(run_src,'@open-callm-run'))(); "
      "open_oracle=assert(loadstring(run_src,'@open-callm-oracle'))(); "
      "open_leaf=assert(loadstring('return function(x) '..producer..' end'," 
        "'@open-producer'))(); "
      "open_oracle_leaf=assert(loadstring('return function(x) '..producer..' end'," 
        "'@open-producer-oracle'))(); "
      "captured_consumer_a=assert(loadstring('return function('..params..') '..body..' end',"
        "'@captured-consumer-a'))(); "
      "captured_consumer_b=assert(loadstring('return function('..params..') '..alt..' end',"
        "'@captured-consumer-b'))(); "
      "captured_oracle_consumer_a=assert(loadstring('return function('..params..') '..body..' end',"
        "'@captured-oracle-consumer-a'))(); "
      "captured_oracle_consumer_b=assert(loadstring('return function('..params..') '..alt..' end',"
        "'@captured-oracle-consumer-b'))(); "
      "local function make_captured(initial) local consumer=initial; "
        "local function run(fn,count) local total=.5; "
          "for i=1,count do total=total+consumer(fn(i+.5)) end; return total end; "
        "local function set(v) consumer=v end; return run,set end; "
      "captured_run,captured_set=make_captured(captured_consumer_a); "
      "captured_oracle,captured_oracle_set=make_captured(captured_oracle_consumer_a); "
      "captured_leaf=assert(loadstring('return function(x) '..producer..' end',"
        "'@captured-producer'))(); "
      "captured_oracle_leaf=assert(loadstring('return function(x) '..producer..' end',"
        "'@captured-producer-oracle'))(); "
      "jit.off(open_oracle,true); jit.off(open_oracle_leaf,true); "
      "jit.off(captured_oracle,true); jit.off(captured_oracle_set,true); "
      "jit.off(captured_oracle_leaf,true); "
      "jit.off(captured_oracle_consumer_a,true); jit.off(captured_oracle_consumer_b,true); "
      "jit.off(captured_set,true); jit.on(captured_run,true); "
      "jit.on(captured_leaf,true); jit.on(captured_consumer_a,true); "
      "jit.on(captured_consumer_b,true); "
      "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; "
    "jit.off(install_open_exit_case,true)");
  run_case(L,1); run_case(L,2); run_case(L,3);
  /* CALLM passes the producer's exact open vector. The callee then pads
  ** missing fixed parameters with nil or ignores extra arguments, matching
  ** ordinary Lua call semantics. Keep these four cross-arity shapes separate
  ** from the original same-arity exit cases above. */
  run_case_shape(L,1,"a,b,c","return a+1","return a+17");
  run_case_shape(L,2,"a,b,c","return a+b+2","return a+101*b+17");
  run_case_shape(L,3,"a","return a+1","return a+17");
  run_case_shape(L,3,"a,b","return a+b+2","return a+101*b+17");
  run_case_shape(L,3,"","return 1.25","return 17.25");
  run_captured_case(L,1); run_captured_case(L,2); run_captured_case(L,3);
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("open CALLM consumer exits OK: %u runtime shapes\n",cases_run);
  printf("captured open CALLM consumer cases OK: %u runtime shapes\n",captured_cases_run);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-open-callm-exits SKIP"); return 0; }
#endif
