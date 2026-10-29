/* Independent admission checks for a fixed nonleaf FUNCF root. */
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
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/proto_fixture_helpers.h"

static unsigned native_returns, native_identity_exits;
static unsigned semantic_bad, postra_bad, low_loopref_bad;

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn=funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L,1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner, const char *fn,
                         int n)
{
  lua_Number result;
  void *cframe=L->cframe;
  lua_getglobal(L,runner); lua_getglobal(L,fn); lua_pushinteger(L,n);
  ljt_lua_pcall(L,2,1,"FUNCF call runner");
  assert(lua_isnumber(L,-1)); result=lua_tonumber(L,-1); lua_pop(L,1);
  assert(L->cframe==cframe && lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *funcf_trace(lua_State *L, GCfunc *fn)
{
  GCproto *pt=funcproto(fn);
  const BCIns *bc=proto_bc(pt);
  for (MSize p=0;p<pt->sizebc;p++) {
    BCIns ins=(BCIns)la_load32_acq((const uint32_t *)&bc[p]);
    if (bc_op(ins)==BC_JFUNCF) {
      TraceNo tr=bc_d(ins);
      GCtrace *T=traceref_safe(L2J(L),tr);
      if (T && trace_runnable_acq(T,tr) && !T->root &&
          trace_startpt_acq(T)==pt && T->linktype==LJ_TRLINK_RETURN)
        return T;
    }
  }
  return NULL;
}

static SnapNo function_identity_exit(const GCtrace *T, IRRef *guardp)
{
  for (IRRef r=REF_FIRST;r<T->nins;r++) {
    IRIns ins=ir_load_acq(&T->ir[r]);
    if (ins.o==IR_EQ && ins.t.irt==(IRT_FUNC|IRT_GUARD) &&
        irref_isk(ins.op2) && T->ir[ins.op2].o==IR_KGC &&
        T->ir[ins.op2].t.irt==IRT_FUNC &&
        isluafunc(gco2func(ir_kgc_load_acq(&T->ir[ins.op2])))) {
      for (SnapNo s=0;s<T->nsnap;s++)
        if (T->snap[s].ref==r) {
          if (guardp) *guardp=r;
          return s;
        }
    }
  }
  assert(0 && "captured function identity guard missing");
  return 0;
}

typedef struct Fixture {
  GCtrace *live, copy;
  GCproto *pt, *childpt;
  GCfunc *child, *cfunc;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, guard, kfunc, argref;
  SnapNo guard_snap, return_snap;
  MSize callpos, return_entry;
} Fixture;

static GCproto *copy_proto(const GCproto *pt)
{
  return ljt_proto_clone_alloc(pt);
}

static void fixture_reset(Fixture *f)
{
  GCtrace *T=f->live;
  GCproto *pt=trace_startpt_acq(T);
  GCfunc *child=ir_kfunc(&T->ir[f->kfunc]);
  MSize startpos=proto_bcpos(pt,trace_startpc_acq(T));
  ljt_proto_clone_reset(f->pt,pt);
  ljt_proto_clone_reset(f->childpt,funcproto(child));
  memset(f->child,0,sizeof(*f->child));
  memcpy(f->child,child,sizeLfunc(0));
  setmref(f->child->l.pc,proto_bc(f->childpt));
  memcpy(f->ir+T->nk,T->ir+T->nk,(T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc],obj2gco(f->child));
  for (IRRef r=T->nk;r+1u<T->nins;r++) {
    const IRIns *constant=&T->ir[r];
    if (constant->o==IR_KNUM) {
      uintptr_t pc=(uintptr_t)T->ir[r+1u].tv.u64;
      const GCproto *owners[2]={pt,funcproto(child)};
      GCproto *copies[2]={f->pt,f->childpt};
      for (unsigned p=0;p<2;p++) {
        uintptr_t lo=(uintptr_t)proto_bc(owners[p]);
        uintptr_t hi=(uintptr_t)(proto_bc(owners[p])+owners[p]->sizebc);
        if (pc>=lo && pc<=hi) {
          MSize pos=(MSize)((pc-lo)/sizeof(BCIns));
          f->ir[r+1u].tv.u64=u64ptr(&proto_bc(copies[p])[pos]);
          break;
        }
      }
      r++;
    }
  }
  memcpy(f->snap,T->snap,T->nsnap*sizeof(SnapShot));
  memcpy(f->map,T->snapmap,T->nsnapmap*sizeof(SnapEntry));
  f->copy=*T; f->copy.ir=f->ir; f->copy.snap=f->snap; f->copy.snapmap=f->map;
  trace_startpt_rel(&f->copy,f->pt);
  setmref(f->copy.startpc,&proto_bc(f->pt)[startpos]);
  proto_bc(f->pt)[startpos]=T->startins;
  for (MSize s=0;s<T->nsnap;s++) {
    SnapShot *snap=&f->snap[s];
    const BCIns *pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    uintptr_t at=(uintptr_t)pc, lo=(uintptr_t)proto_bc(pt);
    uint64_t pcbase;
    assert(at>=lo && at<(uintptr_t)(proto_bc(pt)+pt->sizebc));
    pcbase=u64ptr(&proto_bc(f->pt)[(at-lo)/sizeof(BCIns)])<<8;
    memcpy(&f->map[snap->mapofs+snap->nent],&pcbase,sizeof(pcbase));
  }
  memset(f->J,0,sizeof(*f->J));
  f->J->pt=f->pt; f->J->startpc=trace_startpc_acq(&f->copy);
  f->J->baseslot=1u+LJ_FR2; f->J->loopref=0;
  f->view=(LJArm64PostRAView){0};
  f->view.ir=f->ir; f->view.owner=&f->copy; f->view.owner_private=1;
  f->view.snap=f->snap; f->view.snapmap=f->map;
  f->view.proto=f->pt; f->view.proto_bc=proto_bc(f->pt);
  f->view.startpc=trace_startpc_acq(&f->copy);
  f->view.nins=T->nins; f->view.nk=T->nk;
  f->view.nsnap=T->nsnap; f->view.nsnapmap=T->nsnapmap;
  f->view.spadjust=T->spadjust; f->view.proto_sizebc=f->pt->sizebc;
  f->view.proto_numparams=f->pt->numparams;
  f->view.root_topslot=T->topslot; f->view.startins=T->startins;
}

static int admit(Fixture *f, int postra)
{
  if (postra) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->n;
  return lj_asm_arm64_ir_admit(f->J,&f->copy,NULL);
}

enum {
  M_RETURN_VALUE, M_RETURN_PC, M_CALL_PC, M_CALL_ARITY, M_CALL_RESULT_COUNT,
  M_CAPTURED_CFUNC, M_RETURN_FRAME, M_COUNT
};

static void mutate(Fixture *f, unsigned which)
{
  SnapShot *snap=&f->snap[f->return_snap];
  BCIns *bc=proto_bc(f->pt);
  uint64_t pcbase;
  switch (which) {
  case M_RETURN_VALUE:
    assert(f->return_entry!=UINT32_MAX && f->argref);
    f->map[f->return_entry]=SNAP(snap_slot(f->map[f->return_entry]),0,f->argref);
    break;
  case M_RETURN_PC:
    pcbase=u64ptr(&bc[f->callpos])<<8;
    memcpy(&f->map[snap->mapofs+snap->nent],&pcbase,sizeof(pcbase));
    break;
  case M_CALL_PC: {
    snap=&f->snap[f->guard_snap];
    pcbase=u64ptr(&bc[f->pt->sizebc-1u])<<8;
    memcpy(&f->map[snap->mapofs+snap->nent],&pcbase,sizeof(pcbase));
    break;
  }
  case M_CALL_ARITY: setbc_b(&bc[f->callpos],bc_b(bc[f->callpos])+1u); break;
  case M_CALL_RESULT_COUNT: setbc_c(&bc[f->callpos],bc_c(bc[f->callpos])+1u); break;
  case M_CAPTURED_CFUNC: {
    assert(f->cfunc);
    ir_kgc_store_rel(&f->ir[f->kfunc],obj2gco(f->cfunc));
    break;
  }
  case M_RETURN_FRAME:
    f->map[f->return_entry]|=SNAP_FRAME;
    break;
  default: assert(0);
  }
}

static void admission(GCtrace *T, lua_State *L, GCfunc *cfunc)
{
  Fixture f={0};
  GCproto *pt=trace_startpt_acq(T);
  GCfunc *child=NULL;
  IRRef guard;
  f.live=T; f.guard_snap=function_identity_exit(T,&guard); f.guard=guard;
  f.kfunc=T->ir[guard].op2; f.n=T->nins; f.cfunc=cfunc;
  if (T->ir[f.n-1u].o==IR_NOP) f.n--;
  else while (T->ir[f.n-1u].o==IR_RENAME) f.n--;
  f.pt=copy_proto(pt);
  child=ir_kfunc(&T->ir[f.kfunc]);
  f.childpt=copy_proto(funcproto(child));
  f.child=calloc(1,sizeof(*f.child)); f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(T->nins,sizeof(IRIns)); f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.child&&f.J&&f.ir&&f.snap&&f.map);
  for (IRRef r=REF_FIRST;r<f.n;r++) {
    IRIns ins=ir_load_acq(&T->ir[r]);
    if (ins.o==IR_SLOAD && ins.op1==1u+LJ_FR2 &&
        irt_type(ins.t)==IRT_NUM && (ins.t.irt&IRT_GUARD)) f.argref=r;
  }
  f.callpos=UINT32_MAX;
  for (MSize p=1;p<pt->sizebc;p++) {
    BCIns ins=(BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[p]);
    if (bc_op(ins)==BC_CALL) f.callpos=p;
  }
  assert(f.callpos!=UINT32_MAX && f.argref);
  f.return_snap=UINT16_MAX; f.return_entry=UINT32_MAX;
  for (SnapNo s=0;s<T->nsnap;s++) {
    SnapShot *snap=&T->snap[s];
    const BCIns *pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    if (pc==&proto_bc(pt)[pt->sizebc-1u]) f.return_snap=s;
  }
  assert(f.return_snap!=UINT16_MAX);
  fixture_reset(&f);
  {
    SnapShot *snap=&f.snap[f.return_snap];
    BCIns ret=proto_bc(f.pt)[f.pt->sizebc-1u];
    MSize slot=1u+LJ_FR2+bc_a(ret);
    for (MSize i=0;i<snap->nent;i++) {
      MSize at=snap->mapofs+i;
      if (snap_slot(f.map[at])==slot) f.return_entry=at;
    }
  }
  assert(f.return_entry!=UINT32_MAX);
  for (int postra=0;postra<2;postra++) {
    for (unsigned m=0;m<M_COUNT;m++) {
      fixture_reset(&f);
      assert(admit(&f,postra));
      fixture_reset(&f); mutate(&f,m);
      if (admit(&f,postra)) {
        fprintf(stderr,"FUNCF clone accepted postra=%d mutation=%u\n",postra,m);
        assert(0);
      }
      if (postra) postra_bad++; else semantic_bad++;
    }
  }
  fixture_reset(&f);
  assert(admit(&f,0));
  f.J->loopref=1; /* Malformed pre-LOOP ref must fail closed, not scan it. */
  assert(!admit(&f,0)); low_loopref_bad++;
  free(f.pt); free(f.childpt); free(f.child); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
  (void)L;
}

static int c_target(lua_State *L)
{
  lua_pushnumber(L,lua_tonumber(L,1)*.5); return 1;
}

int main(void)
{
  lua_State *L=ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); "
    "assert(select(2,b:recv(90))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "local target=function(x) return x*.25 end; "
    "funcf_original_target=target; "
    "function set_target(v) target=v end; "
    "local readonly_target=function(x) return x*.25 end; "
    "funcf_readonly_candidate=function(x) "
    "local y=readonly_target(x); return y+.75 end; "
    "funcf_candidate=function(x) local y=target(x); return y+.75 end; "
    "funcf_oracle=function(x) return x*.25+.75 end; "
    "function funcf_run(fn,n) local sum=.5; "
    "for i=1,n do sum=sum+fn(i+.5) end; return sum end; "
    "jit.off(funcf_oracle,true); jit.off(funcf_run,true); "
    "jit.off(set_target,true); jit.flush(); jit.opt.start('hotloop=1',"
    "'hotexit=1','maxtrace=128'); jit.on(); "
    "jit.on(funcf_candidate,true); "
    "jit.on(funcf_readonly_candidate,true)");
  GCfunc *candidate=global_lfunc(L,"funcf_candidate");
  GCtrace *T=NULL;
  for (unsigned i=0;i<8&&!T;i++) {
    assert(invoke(L,"funcf_run","funcf_candidate",80)==
           invoke(L,"funcf_run","funcf_oracle",80));
    T=funcf_trace(L,candidate);
  }
  assert(T && T->linktype==LJ_TRLINK_RETURN);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"funcf_run","funcf_candidate",32)==
         invoke(L,"funcf_run","funcf_oracle",32));
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()==0);
  native_returns++;
  lua_gc(L,LUA_GCCOLLECT,0);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"funcf_run","funcf_candidate",16)==
         invoke(L,"funcf_run","funcf_oracle",16));
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()==0);
  native_returns++;
  lua_pushcfunction(L,c_target); lua_setglobal(L,"c_target");
  lua_getglobal(L,"set_target"); lua_getglobal(L,"c_target");
  ljt_lua_pcall(L,1,0,"replace captured callee");
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  {
    lua_Number expected=.5;
    for (int i=1;i<=16;i++) expected+=(i+.5)*.5+.75;
    lua_Number actual=invoke(L,"funcf_run","funcf_candidate",16);
    assert(actual==expected);
  }
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno &&
         lj_trace_test_first_exitno()==function_identity_exit(T,NULL));
  native_identity_exits++;
  lua_getglobal(L,"set_target"); lua_getglobal(L,"funcf_original_target");
  ljt_lua_pcall(L,1,0,"restore captured callee");
  lua_gc(L,LUA_GCCOLLECT,0);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"funcf_run","funcf_candidate",16)==
         invoke(L,"funcf_run","funcf_oracle",16));
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()==0);
  native_returns++;
  lua_getglobal(L,"c_target"); assert(tvisfunc(L->top-1));
  GCfunc *cfunc=funcV(L->top-1); lua_pop(L,1);
  admission(T,L,cfunc);
  {
    GCfunc *readonly=global_lfunc(L,"funcf_readonly_candidate");
    GCtrace *RT=NULL;
    for (unsigned i=0;i<8&&!RT;i++) {
      assert(invoke(L,"funcf_run","funcf_readonly_candidate",80)==
             invoke(L,"funcf_run","funcf_oracle",80));
      RT=funcf_trace(L,readonly);
    }
    assert(RT && RT->linktype==LJ_TRLINK_RETURN);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke(L,"funcf_run","funcf_readonly_candidate",32)==
           invoke(L,"funcf_run","funcf_oracle",32));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke(L,"funcf_run","funcf_readonly_candidate",16)==
           invoke(L,"funcf_run","funcf_oracle",16));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    admission(RT,L,cfunc);
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); "
                     "assert(ok and v)");
  lua_close(L);
  printf("FUNCF leaf call OK: native returns=%u identity exits=%u "
         "semantic negatives=%u postRA negatives=%u low-loopref negatives=%u\n",
         native_returns,native_identity_exits,semantic_bad,postra_bad,
         low_loopref_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-funcf-calls SKIP"); return 0; }
#endif
