/* Native custom iterator roots, exact cold replay and private admission mutations. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_dispatch.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/proto_fixture_helpers.h"

static unsigned native_runs, semantic_bad, postra_bad;

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn = funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L, 1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner,
                         const char *leaf, int n)
{
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, runner); lua_getglobal(L, leaf); lua_pushinteger(L, n);
  ljt_lua_pcall(L, 2, 1, "Lua leaf call");
  assert(lua_isnumber(L, -1)); result = lua_tonumber(L, -1); lua_pop(L, 1);
  assert(L->cframe == cframe && lj_tg_load_jit_base(L2TG(L)) == NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt = funcproto(global_function(L, "leaf_run"));
  BCIns *bc = proto_bc(pt);
  for (MSize i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_JITERL) {
      GCtrace *T = traceref_safe(L2J(L), bc_d(ins));
      if (T && trace_runnable_acq(T, bc_d(ins)) && !T->root &&
          trace_startpt_acq(T) == pt && T->linktype == LJ_TRLINK_LOOP &&
          T->link == T->traceno) return T;
    }
  }
  return NULL;
}

static SnapNo identity_exit(const GCtrace *T, IRRef *guardp)
{
  for (IRRef r = REF_FIRST; r < T->nins; r++) {
    IRIns p = ir_load_acq(&T->ir[r]);
    if (p.o == IR_EQ && p.t.irt == (IRT_FUNC|IRT_GUARD) &&
        irref_isk(p.op2) && isluafunc(ir_kfunc(&T->ir[p.op2]))) {
      for (SnapNo s = 0; s < T->nsnap; s++)
        if (T->snap[s].ref == r) {
          if (guardp) *guardp = r;
          return s;
        }
    }
  }
  assert(0 && "exact Lua identity snapshot missing"); return 0;
}

static void native_call(lua_State *L, GCtrace *T, const char *leaf,
                        const char *oracle, int n, int changed)
{
  lua_Number expected = invoke(L, "leaf_oracle", oracle, n);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L, "leaf_run", leaf, n) == expected);
  assert(root_trace(L) == T && trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == T->traceno);
  if (changed) assert(lj_trace_test_first_exitno() == identity_exit(T, NULL));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
  native_runs++;
}

static GCproto *copy_proto(const GCproto *pt)
{
  return ljt_proto_clone_alloc(pt);
}

typedef struct Fixture {
  GCtrace *live, copy;
  GCproto *pt, *leafpt;
  GCfunc *leaf;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, guard, kfunc;
  SnapNo guard_snap;
  MSize callpos, fnentry, argentry;
} Fixture;

static void fixture_reset(Fixture *f)
{
  GCtrace *T = f->live;
  GCproto *pt = trace_startpt_acq(T);
  GCfunc *leaf = ir_kfunc(&T->ir[f->kfunc]);
  MSize startpos = proto_bcpos(pt, trace_startpc_acq(T));
  ljt_proto_clone_reset(f->pt, pt);
  ljt_proto_clone_reset(f->leafpt, funcproto(leaf));
  memset(f->leaf, 0, sizeof(*f->leaf));
  memcpy(f->leaf, leaf, sizeLfunc(0));
  setmref(f->leaf->l.pc, proto_bc(f->leafpt));
  memcpy(f->ir+T->nk, T->ir+T->nk, (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc], obj2gco(f->leaf));
  for (IRRef r=T->nk; r+1u<T->nins; r++) {
    const IRIns *constant=&T->ir[r];
    if (constant->o==IR_KNUM) {
      uintptr_t pc=(uintptr_t)T->ir[r+1u].tv.u64;
      const GCproto *owners[2]={pt,funcproto(leaf)};
      GCproto *copies[2]={f->pt,f->leafpt};
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
  memcpy(f->snap, T->snap, T->nsnap*sizeof(SnapShot));
  memcpy(f->map, T->snapmap, T->nsnapmap*sizeof(SnapEntry));
  f->copy = *T; f->copy.ir=f->ir; f->copy.snap=f->snap; f->copy.snapmap=f->map;
  trace_startpt_rel(&f->copy, f->pt);
  setmref(f->copy.startpc, &proto_bc(f->pt)[startpos]);
  proto_bc(f->pt)[startpos] = T->startins;
  for (MSize s=0; s<T->nsnap; s++) {
    SnapShot *snap=&f->snap[s];
    const BCIns *pc=snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
    const GCproto *liveleaf=funcproto(leaf);
    uintptr_t at=(uintptr_t)pc, rootlo=(uintptr_t)proto_bc(pt);
    uintptr_t leaflo=(uintptr_t)proto_bc(liveleaf);
    uint64_t pcbase;
    uint64_t oldfooter;
    memcpy(&oldfooter, &T->snapmap[snap->mapofs+snap->nent], sizeof(oldfooter));
    if (at>=rootlo && at<rootlo+pt->sizebc*sizeof(BCIns)) {
      MSize pos=(MSize)((at-rootlo)/sizeof(BCIns));
      pcbase=(u64ptr(&proto_bc(f->pt)[pos])<<8);
    } else {
      assert(at>=leaflo && at<leaflo+liveleaf->sizebc*sizeof(BCIns));
      MSize pos=(MSize)((at-leaflo)/sizeof(BCIns));
      pcbase=(u64ptr(&proto_bc(f->leafpt)[pos])<<8);
    }
    pcbase |= (uint8_t)oldfooter;
    memcpy(&f->map[snap->mapofs+snap->nent], &pcbase, sizeof(pcbase));
  }
  memset(f->J,0,sizeof(*f->J));
  f->J->pt=f->pt; f->J->startpc=trace_startpc_acq(&f->copy);
  f->J->baseslot=1+LJ_FR2;
  for (IRRef r=REF_FIRST; r<f->n; r++)
    if (f->ir[r].o==IR_LOOP) f->J->loopref=r;
  f->view=(LJArm64PostRAView){0};
  f->view.ir=f->ir; f->view.owner=&f->copy; f->view.owner_private=1;
  f->view.snap=f->snap; f->view.snapmap=f->map;
  f->view.proto=f->pt; f->view.proto_bc=proto_bc(f->pt);
  f->view.startpc=trace_startpc_acq(&f->copy);
  f->view.nins=T->nins; f->view.nk=T->nk;
  f->view.nsnap=T->nsnap; f->view.nsnapmap=T->nsnapmap;
  f->view.spadjust=T->spadjust; f->view.proto_sizebc=f->pt->sizebc;
  f->view.proto_numparams=f->pt->numparams; f->view.root_topslot=T->topslot;
  f->view.startins=T->startins;
}

static int admit(Fixture *f, int allocated)
{
  if (allocated) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->n;
  return lj_asm_arm64_ir_admit(f->J,&f->copy,NULL);
}

static void admission(GCtrace *T)
{
  Fixture f={0};
  MSize loopmap=0, control=0, visible=0, child=0;
  IRRef state=0, phi=0;
  LJArm64IteratorSource iter;
  f.live=T; f.guard_snap=identity_exit(T,&f.guard); f.kfunc=T->ir[f.guard].op2;
  f.n=T->nins;
  if (T->ir[f.n-1u].o==IR_NOP) f.n--;
  else while (T->ir[f.n-1u].o==IR_RENAME) f.n--;
  f.pt=copy_proto(trace_startpt_acq(T));
  f.leafpt=copy_proto(funcproto(ir_kfunc(&T->ir[f.kfunc])));
  f.leaf=calloc(1,sizeof(*f.leaf)); f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(T->nins,sizeof(IRIns)); f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.leaf&&f.J&&f.ir&&f.snap&&f.map);
  fixture_reset(&f);
  assert(lj_asm_arm64_iterl_tuple(f.pt, f.J->startpc, T->startins,
                                 T->startins, 1, &iter));
  for (MSize i=0;i<T->nsnap;i++) {
    if (T->snap[i].ref==f.J->loopref) loopmap=i;
    uint64_t footer;
    memcpy(&footer,&T->snapmap[T->snap[i].mapofs+T->snap[i].nent],sizeof(footer));
    if ((uint8_t)footer && !child) child=i;
  }
  for (MSize i=0;i<T->snap[loopmap].nent;i++) {
    MSize at=T->snap[loopmap].mapofs+i;
    if (snap_slot(T->snapmap[at])==iter.base+LJ_FR2) control=at;
    if (snap_slot(T->snapmap[at])==iter.base+1u+LJ_FR2) visible=at;
  }
  for (IRRef r=REF_FIRST;r<f.n;r++) {
    if (f.ir[r].o==IR_SLOAD && f.ir[r].op1==iter.base-1u+LJ_FR2) state=r;
    if (f.ir[r].o==IR_PHI && f.ir[r].op1==snap_ref(f.map[control])) phi=r;
  }
  assert(loopmap&&control&&visible&&state&&phi);
  if (!child) child=loopmap; /* NUM iterator has no child overflow map. */
  for (int allocated=0;allocated<2;allocated++) {
    for (unsigned m=0;m<12;m++) {
      fixture_reset(&f);
      if (!admit(&f,allocated)) {
        LJArm64IRReject reject={0};
        lj_asm_arm64_ir_admit(f.J,&f.copy,&reject);
        fprintf(stderr,"iterator baseline postra=%d reason=%u ref=%u op=%u\n",
          allocated,reject.reason,reject.ref,reject.op);
        assert(0);
      }
      fixture_reset(&f);
      switch (m) {
      case 0: setbc_c(&proto_bc(f.pt)[iter.call_pc],2); break;
      case 1: setbc_b(&proto_bc(f.pt)[iter.call_pc],3); break;
      case 2: setbc_op(&proto_bc(f.pt)[iter.setup_pc],BC_ISNEXT); break;
      case 3: f.ir[f.ir[f.guard].op1].op1++; break;
      case 4: f.ir[state].op1++; break;
      case 5: f.map[control]=(f.map[control]&~0xffffu)|state; break;
      case 6: f.map[visible]=(f.map[visible]&~0xffffu)|state; break;
      case 7: f.ir[phi].op2=state; break;
      case 8: f.snap[loopmap].mapofs=UINT32_MAX; break;
      case 9: {
        MSize ofs=f.snap[child].mapofs+f.snap[child].nent;
        uint64_t footer; memcpy(&footer,&f.map[ofs],sizeof(footer));
        footer=(footer&~UINT64_C(255))|1u;
        memcpy(&f.map[ofs],&footer,sizeof(footer)); break;
      }
      case 10: setbc_d(&proto_bc(f.leafpt)[f.leafpt->sizebc-1u],3); break;
      case 11: {
        MSize pc;
        for (pc=iter.body_pc;pc<iter.call_pc;pc++)
          if (bc_op(proto_bc(f.pt)[pc])==BC_CSET) break;
        assert(pc<iter.call_pc);
        setbc_a(&proto_bc(f.pt)[pc],iter.base-2u); break;
      }
      }
      if (admit(&f,allocated)) {
        fprintf(stderr,"iterator accepted mutation %u postra=%d\n",m,allocated);
        assert(0);
      }
      if (allocated) postra_bad++; else semantic_bad++;
    }
  }
  free(f.pt); free(f.leafpt); free(f.leaf); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

typedef struct EntryRace {
  TGState *tg;
  BCIns *pc;
  BCIns replacement;
  uint32_t done, prior_recovery;
  lua_State *L;
  GCfunc *expected;
} EntryRace;

static void *entry_race(void *arg)
{
  EntryRace *race=(EntryRace *)arg;
  unsigned i;
  for (i=0;i<10000000u;i++) {
    if (lj_trace_test_root_entry_paused()==LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA)
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(i<10000000u);
  assert(curr_func(race->L)==race->expected);
  race->prior_recovery=lj_trace_test_root_entry_startins_calls();
  if (race->pc) bc_publish(race->pc,race->replacement);
  else lj_tg_profile_request_rel(race->tg,1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->done,1);
  return NULL;
}

static void entry_rechecks(lua_State *L, GCtrace *T)
{
  TGState *tg=L2TG(L);
  EntryRace race={0};
  pthread_t worker;
  lua_Number expected=invoke(L,"leaf_oracle","oracle_step",1);
  race.tg=tg; race.L=L; race.expected=global_function(L,"leaf_run");
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA);
  assert(pthread_create(&worker,NULL,entry_race,&race)==0);
  assert(invoke(L,"leaf_run","leaf_step",1)==expected);
  assert(pthread_join(worker,NULL)==0 && la_load32_acq(&race.done));
  assert(lj_trace_test_root_entry_publishes()==1 &&
         lj_trace_test_root_entry_cleanups()==1 &&
         lj_trace_test_root_entry_successes()==0 &&
         lj_trace_test_exit_calls()==0 &&
         lj_trace_test_root_entry_startins_calls()==race.prior_recovery+1u &&
         !lj_tg_profile_request_acq(tg));
  {
    GCfunc *fn=global_function(L,"leaf_run");
    TValue saved;
    MSize frame=funcproto(fn)->framesize;
    int top=lua_gettop(L);
    int32_t vmstate=lj_tg_vmstate_load_acq(tg);
    BCIns *pc=(BCIns *)trace_startpc_acq(T), live=(BCIns)la_load32_acq((const uint32_t *)pc);
    BCReg a=bc_a(live);
    LJTraceRootEntry entry;
    assert(lua_checkstack(L,(int)frame+4)); lua_settop(L,(int)frame);
    copyTV(L,&saved,L->base-2); setfuncV(L,L->base-2,fn);
    setintV(&L->base[a],1); setintV(&L->base[a-1u],1);
    lj_tg_vmstate_store_rel(tg,(int32_t)~LJ_VMST_INTERP);
    lj_trace_test_root_entry_reset();
    entry=lj_trace_enter_root(L2J(L),pc,T->traceno,L,L->base,live,NULL);
    assert(entry.trace==T && entry.target);
    lj_tg_store_jit_base(tg,NULL);
    /* The wrong consumed key/control state cannot acquire native authority. */
    setintV(&L->base[a-1u],2);
    entry=lj_trace_enter_root(L2J(L),pc,T->traceno,L,L->base,live,NULL);
    assert(!entry.trace && !entry.target && !lj_tg_load_jit_base(tg));
    setintV(&L->base[a-1u],1);
    race=(EntryRace){0}; race.pc=pc; race.L=L; race.expected=fn;
    race.replacement=BCINS_AD(BC_JITERL,a+1u,T->traceno);
    lj_trace_test_root_entry_reset();
    lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA);
    assert(pthread_create(&worker,NULL,entry_race,&race)==0);
    entry=lj_trace_enter_root(L2J(L),pc,T->traceno,L,L->base,live,NULL);
    assert(pthread_join(worker,NULL)==0 && la_load32_acq(&race.done));
    assert(!entry.trace && !entry.target && !lj_tg_load_jit_base(tg) &&
           lj_trace_test_root_entry_cleanups()==1);
    bc_publish(pc,live);
    copyTV(L,L->base-2,&saved);
    lj_tg_vmstate_store_rel(tg,vmstate); lua_settop(L,top);
  }
}

static void run(lua_State *L, const char *source, const char *context)
{
  (void)context; ljt_lua_dostring(L, source);
}

int main(void)
{
  lua_State *L=luaL_newstate();
  assert(L); luaL_openlibs(L);
  run(L,
    "jit.off(); jit.opt.start('hotloop=1','hotexit=255','maxtrace=128'); "
    "local source=[[return function(fn,n) local s=.5; "
      "for k in fn,n,0 do s=s+k; if k>=n then break end end return s end]]; "
    "iter_source=source; leaf_run=assert(loadstring(source))(); "
    "leaf_oracle=assert(loadstring(source))(); jit.off(leaf_oracle,true); "
    "leaf_step=function(n,k) return k+1 end; "
    "leaf_other=function(n,k) return k+2 end; "
    "oracle_step=function(n,k) return k+1 end; "
    "oracle_other=function(n,k) return k+2 end; "
    "jit.off(oracle_step,true); jit.off(oracle_other,true); "
    "local th=require'threading'; iter_release=th.channel(0); "
    "iter_peer=th.spawn(function(ch) local _,ok=ch:recv(60); assert(ok); "
      "return true end,iter_release); jit.on()", "iterator setup");
  for (unsigned shape=0;shape<2;shape++) {
    if (shape) run(L,
      "local s=iter_source:gsub('fn,n,0','fn,n,.5'); "
      "leaf_run=assert(loadstring(s))(); leaf_oracle=assert(loadstring(s))(); "
      "jit.off(leaf_oracle,true)", "NUM iterator source");
  for (unsigned generation=0;generation<2;generation++) {
    for (unsigned i=0;i<80;i++) {
      lua_Number expected=invoke(L,"leaf_oracle","oracle_step",64);
      GCproto *rp=funcproto(global_function(L,"leaf_run"));
      GCproto *lp=funcproto(global_function(L,"leaf_step"));
      /* Target this source edge after a flush. A leaf FUNCF can hash to the
      ** same hot-count bucket and penalize it before ITERL becomes hot.
      ** One call decrement then one loop decrement must heat ITERL, while
      ** unrelated function-entry buckets remain cold. */
      assert(hotcount_setl(G(L),L,proto_bc(rp)+1,65535));
      assert(hotcount_setl(G(L),L,proto_bc(lp)+1,65535));
      for (MSize pc=1;pc<rp->sizebc;pc++)
        if (bc_op(proto_bc(rp)[pc])==BC_ITERL)
          assert(hotcount_setl(G(L),L,proto_bc(rp)+pc+1,HOTCOUNT_CALL));
      assert(invoke(L,"leaf_run","leaf_step",64)==expected);
    }
    GCtrace *T=root_trace(L);
    if (!T) {
      fprintf(stderr,"missing iterator root shape=%u generation=%u\n",shape,generation);
      GCproto *rp=funcproto(global_function(L,"leaf_run"));
      GCproto *lp=funcproto(global_function(L,"leaf_step"));
      LJArm64IteratorSource tuple;
      BCIns *loop=NULL;
      for (MSize pc=1;pc<rp->sizebc;pc++)
        if (bc_op(proto_bc(rp)[pc])==BC_ITERL) loop=&proto_bc(rp)[pc];
      assert(loop);
      unsigned ri=(u32ptr(loop+1)>>2)&(HOTCOUNT_SIZE-1u);
      unsigned li=(u32ptr(proto_bc(lp)+1)>>2)&(HOTCOUNT_SIZE-1u);
      fprintf(stderr,"root flags=%u frame=%u leaf flags=%u header=%u jitflags=%u state=%u "
        "rootidx=%u leafidx=%u counters=%u/%u tuple=%d\n",rp->flags,rp->framesize,
        lp->flags,bc_op(proto_bc(lp)[0]),jit_flags_acq(L2J(L)),lj_trace_state_load(L2J(L)),
        ri,li,L2TG(L)->hotcount[ri],L2TG(L)->hotcount[li],
        lj_asm_arm64_iterl_tuple(rp,loop,*loop,*loop,1,&tuple));
      run(L,"require('jit.bc').dump(leaf_run); require('jit.bc').dump(leaf_step)",
          "missing root bytecode");
      fflush(stdout);
    }
    assert(T);
    assert(trace_arm64_admission_acq(T)==TRACE_ARM64_ITERL_ADMITTED);
    admission(T);
    entry_rechecks(L,T);
    for (int n=1;n<=96;n+=5) native_call(L,T,"leaf_step","oracle_step",n,0);
    native_call(L,T,"leaf_other","oracle_other",64,1);
    lua_gc(L,LUA_GCCOLLECT,0);
    native_call(L,T,"leaf_step","oracle_step",96,0);
    run(L,"jit.flush()", "iterator flush");
  }
  }
  run(L,"assert(iter_release:send(true,5)); "
    "local ok,value=iter_peer:join(5); assert(ok and value)","iterator peer join");
  lua_close(L);
  printf("ARM64 ITERL native PASS: %u calls, %u/%u private rejections, "
    "identity/short/full-GC/flush with live peer\n",native_runs,semantic_bad,postra_bad);
  return 0;
}
#else
int main(void) { puts("ARM64 ITERL native SKIP: requires ARM64 helpers"); return 0; }
#endif
