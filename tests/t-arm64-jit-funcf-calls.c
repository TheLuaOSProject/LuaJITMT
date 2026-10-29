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
static unsigned tail_semantic_bad, tail_postra_bad;

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

static lua_Number invoke_direct(lua_State *L, const char *fn, lua_Number x)
{
  lua_Number result;
  void *cframe=L->cframe;
  lua_getglobal(L,fn); lua_pushnumber(L,x);
  ljt_lua_pcall(L,1,1,"FUNCF direct call");
  assert(lua_isnumber(L,-1)); result=lua_tonumber(L,-1); lua_pop(L,1);
  assert(L->cframe==cframe && lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static void invoke_bad_direct(lua_State *L, const char *fn)
{
  void *cframe=L->cframe;
  lua_getglobal(L,fn); lua_pushstring(L,"bad");
  assert(lua_pcall(L,1,1,0) != 0);
  lua_pop(L,1);
  assert(L->cframe==cframe && lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
}

static void invoke_bad_call(lua_State *L, const char *runner, const char *fn)
{
  void *cframe=L->cframe;
  lua_getglobal(L,runner); lua_getglobal(L,fn); lua_pushstring(L,"bad");
  assert(lua_pcall(L,2,1,0) != 0);
  lua_pop(L,1);
  assert(L->cframe==cframe && lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
}

static void invoke_pair(lua_State *L, const char *runner, const char *fn,
                        lua_Number x, lua_Number want0, lua_Number want1)
{
  void *cframe=L->cframe;
  lua_getglobal(L,runner); lua_getglobal(L,fn); lua_pushnumber(L,x);
  ljt_lua_pcall(L,2,2,"FUNCF two-result runner");
  assert(lua_isnumber(L,-2) && lua_tonumber(L,-2)==want0);
  assert(lua_isnumber(L,-1) && lua_tonumber(L,-1)==want1);
  lua_pop(L,2);
  assert(L->cframe==cframe && lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
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
  IRRef child_second_value;
} Fixture;

typedef struct MultiFixture {
  GCtrace *live, copy;
  GCproto *pt;
  jit_State *J;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  LJArm64PostRAView view;
  IRRef nins, semantic_nins, argref;
  SnapNo terminal;
  MSize second_entry;
} MultiFixture;

typedef struct TailFixture {
  GCtrace *live, copy;
  GCproto *pt, *childpt;
  GCfunc *child, *rootfn;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *map;
  jit_State *J;
  LJArm64PostRAView view;
  IRRef n, kfunc, argref;
  SnapNo terminal;
  MSize output_entry, child_entry, marker_entry, callpos;
} TailFixture;

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

static void multi_reset(MultiFixture *f)
{
  GCproto *source=trace_startpt_acq(f->live);
  MSize start=proto_bcpos(source,trace_startpc_acq(f->live));
  ljt_proto_clone_reset(f->pt,source);
  memcpy(f->ir+f->live->nk,trace_ir_acq(f->live)+f->live->nk,
         (f->nins-f->live->nk)*sizeof(IRIns));
  memcpy(f->snap,trace_snap_acq(f->live),f->live->nsnap*sizeof(SnapShot));
  memcpy(f->map,trace_snapmap_acq(f->live),
         f->live->nsnapmap*sizeof(SnapEntry));
  f->copy=*f->live; f->copy.ir=f->ir; f->copy.snap=f->snap;
  f->copy.snapmap=f->map;
  trace_startpt_rel(&f->copy,f->pt);
  setmref(f->copy.startpc,&proto_bc(f->pt)[start]);
  proto_bc(f->pt)[start]=f->live->startins;
  for (SnapNo s=0;s<f->live->nsnap;s++) {
    SnapShot *snap=&f->snap[s];
    MSize ofs=snap->mapofs+snap->nent;
    uint64_t pcbase;
    memcpy(&pcbase,&f->map[ofs],sizeof(pcbase));
    {
      const BCIns *pc=(const BCIns *)(uintptr_t)(pcbase>>8);
      MSize pos=proto_bcpos(source,pc);
      assert(pos<source->sizebc);
      pcbase=u64ptr(&proto_bc(f->pt)[pos])<<8;
      memcpy(&f->map[ofs],&pcbase,sizeof(pcbase));
    }
  }
  f->J->pt=f->pt; f->J->startpc=trace_startpc_acq(&f->copy);
  f->J->baseslot=1u+LJ_FR2; f->J->parent=0; f->J->exitno=0;
  f->J->loopref=0; f->J->framedepth=0; f->J->retdepth=0;
  f->semantic_nins=f->nins;
  while (f->semantic_nins>REF_FIRST &&
         (f->ir[f->semantic_nins-1u].o==IR_NOP ||
          f->ir[f->semantic_nins-1u].o==IR_RENAME))
    f->semantic_nins--;
  f->view=(LJArm64PostRAView){0};
  f->view.ir=f->ir; f->view.owner=&f->copy; f->view.owner_private=1;
  f->view.snap=f->snap; f->view.snapmap=f->map; f->view.proto=f->pt;
  f->view.proto_bc=proto_bc(f->pt); f->view.startpc=f->J->startpc;
  f->view.nins=f->nins; f->view.nk=f->live->nk;
  f->view.nsnap=f->live->nsnap; f->view.nsnapmap=f->live->nsnapmap;
  f->view.spadjust=f->live->spadjust; f->view.proto_sizebc=f->pt->sizebc;
  f->view.proto_numparams=f->pt->numparams;
  f->view.root_topslot=f->live->topslot; f->view.startins=f->live->startins;
}

static int multi_admit(MultiFixture *f, int postra)
{
  if (postra) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->semantic_nins;
  return lj_asm_arm64_ir_admit(f->J,&f->copy,NULL);
}

static void multi_drop_terminal_result(MultiFixture *f)
{
  SnapShot *terminal=&f->snap[f->terminal];
  MSize end=f->view.nsnapmap;
  assert(f->terminal+1u==f->view.nsnap &&
         f->second_entry>=terminal->mapofs &&
         f->second_entry<terminal->mapofs+terminal->nent &&
         terminal->nent>0 && f->second_entry<end);
  memmove(&f->map[f->second_entry],&f->map[f->second_entry+1u],
          (end-f->second_entry-1u)*sizeof(SnapEntry));
  terminal->nent--;
  f->view.nsnapmap--;
  f->copy.nsnapmap=f->view.nsnapmap;
}

static void multi_admission(GCtrace *T)
{
  MultiFixture f={0};
  GCproto *source=trace_startpt_acq(T);
  BCIns ret;
  MSize slot;
  unsigned semantic=0, postra=0;
  f.live=T; f.nins=T->nins;
  f.pt=ljt_proto_clone_alloc(source);
  assert(f.pt);
  f.J=calloc(1,sizeof(*f.J));
  f.ir=calloc(f.nins,sizeof(IRIns));
  f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.J && f.ir && f.snap && f.map);
  multi_reset(&f);
  assert(source->sizebc==f.pt->sizebc && source->framesize==f.pt->framesize);
  ret=(BCIns)la_load32_acq((const uint32_t *)&proto_bc(f.pt)[f.pt->sizebc-1u]);
  assert(bc_op(ret)==BC_RET && bc_d(ret)==3u);
  slot=1u+LJ_FR2+bc_a(ret)+1u;
  f.terminal=UINT16_MAX; f.second_entry=UINT32_MAX; f.argref=0;
  for (SnapNo s=0;s<T->nsnap;s++) {
    uint64_t pcbase;
    SnapShot *snap=&f.snap[s];
    memcpy(&pcbase,&f.map[snap->mapofs+snap->nent],sizeof(pcbase));
    if (!(uint8_t)pcbase &&
        (const BCIns *)(uintptr_t)(pcbase>>8)==
          &proto_bc(f.pt)[f.pt->sizebc-1u]) f.terminal=s;
  }
  assert(f.terminal!=UINT16_MAX);
  for (MSize i=0;i<f.snap[f.terminal].nent;i++) {
    MSize at=f.snap[f.terminal].mapofs+i;
    if (snap_slot(f.map[at])==slot) f.second_entry=at;
  }
  for (IRRef r=REF_FIRST;r<f.nins;r++) {
    IRIns ins=ir_load_acq(&f.ir[r]);
    if (ins.o==IR_SLOAD && ins.op1==1u+LJ_FR2 &&
        irt_type(ins.t)==IRT_NUM && (ins.t.irt&IRT_GUARD)) f.argref=r;
  }
  assert(f.second_entry!=UINT32_MAX && f.argref);
  for (int allocated=0;allocated<2;allocated++) {
    multi_reset(&f); assert(multi_admit(&f,allocated));
    multi_reset(&f);
    f.map[f.second_entry]=SNAP(slot,0,f.argref);
    if (multi_admit(&f,allocated)) {
      fprintf(stderr,"two-result FUNCF accepted substituted second return allocated=%d\n",
              allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;
    multi_reset(&f);
    multi_drop_terminal_result(&f);
    if (multi_admit(&f,allocated)) {
      fprintf(stderr,"two-result FUNCF accepted missing terminal result allocated=%d\n",
              allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;
    multi_reset(&f);
    setbc_d(&proto_bc(f.pt)[f.pt->sizebc-1u],2u);
    if (multi_admit(&f,allocated)) {
      fprintf(stderr,"two-result FUNCF accepted one-result return mutation allocated=%d\n",
              allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;
    if (allocated) {
      IRRef first=0, second=0;
      RegSP saved;
      SnapShot *terminal=&f.snap[f.terminal];
      for (MSize i=0;i<terminal->nent;i++) {
        SnapEntry entry=f.map[terminal->mapofs+i];
        IRRef value=snap_ref(entry);
        if (snap_slot(entry)==slot-1u) first=value;
        if (snap_slot(entry)==slot) second=value;
      }
      assert(first>=REF_FIRST && second>=REF_FIRST);
      if (first != second) {
        multi_reset(&f);
        assert(multi_admit(&f,1));
        saved=f.ir[second].prev;
        f.ir[second].prev=f.ir[first].prev;
        if (multi_admit(&f,1)) {
          fprintf(stderr,"two-result FUNCF accepted aliased allocated results\n");
          assert(0);
        }
        f.ir[second].prev=saved;
        assert(multi_admit(&f,1));
        postra++;
      }
    }
  }
  postra_bad+=postra; semantic_bad+=semantic;
  free(f.pt); free(f.J); free(f.ir); free(f.snap); free(f.map);
}

static void tail_reset(TailFixture *f)
{
  GCtrace *T=f->live;
  GCproto *source=trace_startpt_acq(T), *childsource;
  MSize start=proto_bcpos(source,trace_startpc_acq(T));
  assert(f->child && f->childpt && f->pt);
  childsource=funcproto(ir_kfunc(&T->ir[f->kfunc]));
  ljt_proto_clone_reset(f->pt,source);
  ljt_proto_clone_reset(f->childpt,childsource);
  memcpy(f->child,ir_kfunc(&T->ir[f->kfunc]),sizeLfunc(0));
  setmref(f->child->l.pc,proto_bc(f->childpt));
  memcpy(f->ir+T->nk,T->ir+T->nk,
         (T->nins-T->nk)*sizeof(IRIns));
  ir_kgc_store_rel(&f->ir[f->kfunc],obj2gco(f->child));
  for (IRRef r=T->nk;r+1u<T->nins;r++) {
    const IRIns *constant=&T->ir[r];
    if (constant->o==IR_KNUM) {
      uintptr_t pc=(uintptr_t)T->ir[r+1u].tv.u64;
      const GCproto *owners[2]={source,childsource};
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
  setmref(f->copy.startpc,&proto_bc(f->pt)[start]);
  proto_bc(f->pt)[start]=T->startins;
  for (SnapNo s=0;s<T->nsnap;s++) {
    SnapShot *snap=&f->snap[s];
    MSize at=snap->mapofs+snap->nent;
    uint64_t pcbase;
    const BCIns *pc=snap_pc(&T->snapmap[at]);
    uintptr_t address=(uintptr_t)pc;
    uintptr_t rootlo=(uintptr_t)proto_bc(source);
    uintptr_t roothi=(uintptr_t)(proto_bc(source)+source->sizebc);
    uintptr_t childlo=(uintptr_t)proto_bc(childsource);
    uintptr_t childhi=(uintptr_t)(proto_bc(childsource)+childsource->sizebc);
    if (address>=rootlo && address<roothi) {
      MSize pos=(MSize)((address-rootlo)/sizeof(BCIns));
      pc=&proto_bc(f->pt)[pos];
    } else {
      assert(address>=childlo && address<childhi);
      pc=&proto_bc(f->childpt)[(address-childlo)/sizeof(BCIns)];
    }
    {
      uint64_t original_pcbase;
      memcpy(&original_pcbase,&T->snapmap[at],sizeof(original_pcbase));
      pcbase=(u64ptr(pc)<<8) | (original_pcbase&0xffu);
    }
    memcpy(&f->map[at],&pcbase,sizeof(pcbase));
  }
  memset(f->J,0,sizeof(*f->J));
  f->J->pt=f->pt; f->J->startpc=trace_startpc_acq(&f->copy);
  f->J->baseslot=1u+LJ_FR2;
  f->J->cur.nins=T->nins; f->J->cur.nk=T->nk; f->J->cur.ir=f->ir;
  f->J->cur.snap=f->snap; f->J->cur.snapmap=f->map;
  f->J->cur.nsnap=T->nsnap; f->J->cur.nsnapmap=T->nsnapmap;
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

static int tail_admit(TailFixture *f, int postra)
{
  LJArm64IRReject reject={0};
  if (postra) return lj_asm_arm64_postra_admit(&f->view,NULL);
  f->copy.nins=f->n;
  if (!lj_asm_arm64_ir_admit(f->J,&f->copy,&reject)) {
    return 0;
  }
  return 1;
}

static void tail_drop_output(TailFixture *f)
{
  SnapShot *terminal=&f->snap[f->terminal];
  MSize end=f->view.nsnapmap;
  assert(f->terminal+1u==f->view.nsnap && terminal->nent>0 &&
         f->output_entry>=terminal->mapofs &&
         f->output_entry<terminal->mapofs+terminal->nent && f->output_entry<end);
  memmove(&f->map[f->output_entry],&f->map[f->output_entry+1u],
          (end-f->output_entry-1u)*sizeof(SnapEntry));
  terminal->nent--;
  f->view.nsnapmap--;
  f->copy.nsnapmap=f->view.nsnapmap;
  f->J->cur.nsnapmap=f->view.nsnapmap;
}

static void tail_admission(GCtrace *T, GCfunc *rootfn)
{
  TailFixture f={0};
  GCproto *source=trace_startpt_acq(T);
  IRRef guard;
  unsigned semantic=0, postra=0;
  f.live=T; f.rootfn=rootfn; f.n=T->nins;
  while (f.n>REF_FIRST &&
         (T->ir[f.n-1u].o==IR_NOP || T->ir[f.n-1u].o==IR_RENAME)) f.n--;
  f.terminal=T->nsnap-1u;
  (void)function_identity_exit(T,&guard);
  f.kfunc=T->ir[guard].op2;
  f.child=calloc(1,sizeof(*f.child));
  f.pt=copy_proto(source);
  f.childpt=copy_proto(funcproto(ir_kfunc(&T->ir[f.kfunc])));
  f.J=calloc(1,sizeof(*f.J)); f.ir=calloc(T->nins,sizeof(IRIns));
  f.snap=malloc(T->nsnap*sizeof(SnapShot));
  f.map=malloc(T->nsnapmap*sizeof(SnapEntry));
  assert(f.child && f.pt && f.childpt && f.J && f.ir && f.snap && f.map);
  tail_reset(&f);
  {
    BCIns call=(BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(f.pt)[f.pt->sizebc-1u]);
    assert(bc_op(call)==BC_CALLT && bc_b(call)==0u && bc_c(call)==2u);
    f.callpos=f.pt->sizebc-1u;
    for (MSize i=0;i<f.snap[f.terminal].nent;i++) {
      MSize at=f.snap[f.terminal].mapofs+i;
      SnapEntry entry=f.map[at];
      if (snap_slot(entry)==1u+LJ_FR2+bc_a(call)) f.output_entry=at;
      if (snap_slot(entry)==0 && snap_ref(entry)==f.kfunc) f.child_entry=at;
      if (entry==SNAP(1,SNAP_FRAME|SNAP_NORESTORE,REF_NIL)) f.marker_entry=at;
    }
    for (IRRef r=REF_FIRST;r<f.n;r++) {
      IRIns ins=ir_load_acq(&f.ir[r]);
      if (ins.o==IR_SLOAD && ins.op1==1u+LJ_FR2 &&
          (irt_type(ins.t)==IRT_INT || irt_type(ins.t)==IRT_NUM) &&
          (ins.t.irt&IRT_GUARD)) f.argref=r;
    }
  }
  assert(f.output_entry && f.child_entry && f.marker_entry && f.argref);
  for (int allocated=0;allocated<2;allocated++) {
    tail_reset(&f);
    if (!tail_admit(&f,allocated)) {
      fprintf(stderr,"tail CALLT baseline rejected allocated=%d nins=%u nsnap=%u map=%u\\n",
              allocated,f.view.nins,f.view.nsnap,f.view.nsnapmap);
      assert(0);
    }

    tail_reset(&f);
    f.map[f.output_entry]=SNAP(snap_slot(f.map[f.output_entry]),0,f.argref);
    if (tail_admit(&f,allocated)) {
      fprintf(stderr,"tail CALLT accepted substituted result allocated=%d\\n",allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;

    tail_reset(&f);
    f.map[f.output_entry]=SNAP(snap_slot(f.map[f.output_entry])+1u,0,
                               snap_ref(f.map[f.output_entry]));
    if (tail_admit(&f,allocated)) {
      fprintf(stderr,"tail CALLT accepted result in wrong slot allocated=%d\\n",allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;

    tail_reset(&f);
    tail_drop_output(&f);
    if (tail_admit(&f,allocated)) {
      fprintf(stderr,"tail CALLT accepted missing result map allocated=%d\\n",allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;

    tail_reset(&f);
    ir_kgc_store_rel(&f.ir[f.kfunc],obj2gco(rootfn));
    if (tail_admit(&f,allocated)) {
      fprintf(stderr,"tail CALLT accepted substituted child identity allocated=%d\\n",allocated);
      assert(0);
    }
    if (allocated) postra++; else semantic++;
  }
  tail_semantic_bad+=semantic; tail_postra_bad+=postra;
  free(f.pt); free(f.childpt); free(f.child); free(f.J);
  free(f.ir); free(f.snap); free(f.map);
}

enum {
  M_RETURN_VALUE, M_RETURN_PC, M_CALL_PC, M_CALL_ARITY, M_CALL_RESULT_COUNT,
  M_CAPTURED_CFUNC, M_RETURN_FRAME, M_CHILD_SECOND_VALUE, M_COUNT
};

static IRRef child_second_value_ir(GCtrace *T)
{
  IRRef r, found=0;
  for (r=REF_FIRST;r<T->nins;r++) {
    IRIns ins=ir_load_acq(&T->ir[r]);
    IRRef refs[2]={ins.op1,ins.op2};
    int literal=0;
    if (ins.o!=IR_ADD) continue;
    for (unsigned i=0;i<2;i++) {
      IRRef other=refs[i];
      if (irref_isk(other) && other>=T->nk && other<REF_TRUE &&
          T->ir[other].o==IR_KNUM &&
          ir_knum(&T->ir[other])->n==(lua_Number)1.25) literal=1;
    }
    if (!literal) continue;
    if (found) return 0;
    found=r;
  }
  return found;
}

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
  case M_CHILD_SECOND_VALUE:
    assert(f->child_second_value>=REF_FIRST);
    f->ir[f->child_second_value].op2=f->ir[f->child_second_value].op1;
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
  f.child_second_value=bc_b(proto_bc(f.pt)[f.callpos])==3u ?
    child_second_value_ir(T) : 0;
  if (bc_b(proto_bc(f.pt)[f.callpos])==3u)
    assert(f.child_second_value>=REF_FIRST);
  for (int postra=0;postra<2;postra++) {
    for (unsigned m=0;m<M_COUNT;m++) {
      if (m==M_CHILD_SECOND_VALUE && !f.child_second_value)
        continue;
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

static void test_pair_output_liveness(GCproto *source)
{
  GCproto *pt=copy_proto(source);
  BCIns *bc=proto_bc(pt);
  pt->sizebc=4;
  pt->framesize=5;
  pt->numparams=3;
  bc[0]=BCINS_AD(BC_FUNCF,0,0);
  /* Slot 2 is read before CALL overwrites it as its second result. */
  bc[1]=BCINS_ABC(BC_ADDVV,0,2,3);
  bc[2]=BCINS_ABC(BC_CALL,1,3,2);
  bc[3]=BCINS_AD(BC_RET1,0,2);
  assert(lj_asm_arm64_test_funcf_postra_needed(pt,1,2)==1);
  assert(lj_asm_arm64_test_funcf_postra_needed(pt,2,2)==0);
  assert(lj_asm_arm64_test_funcf_ir_output_slot_changed(pt,2,3)==1);
  assert(lj_asm_arm64_test_funcf_postra_output_slot_changed(pt,2,3)==1);
  free(pt);
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
    "funcf_multi_candidate=function(x) return x*.5,x+1.25 end; "
    "funcf_multi_oracle=function(x) return x*.5,x+1.25 end; "
    "local pair_target=function(x) return x*.5,x+1.25 end; "
    "funcf_pair_candidate=function(x) local a,b=pair_target(x); return a+b end; "
    "local pair_oracle_target=function(x) return x*.5,x+1.25 end; "
    "funcf_pair_oracle=function(x) local a,b=pair_oracle_target(x); return a+b end; "
    "function funcf_multi_run(fn,x) return fn(x) end; "
    "funcf_oracle=function(x) return x*.25+.75 end; "
    "function funcf_run(fn,n) local sum=.5; "
    "for i=1,n do sum=sum+fn(i+.5) end; return sum end; "
    "jit.off(funcf_oracle,true); jit.off(funcf_run,true); "
    "jit.off(set_target,true); jit.off(funcf_multi_run,true); "
    "jit.off(funcf_multi_oracle,true); jit.flush(); "
    "jit.off(funcf_pair_oracle,true); "
    "jit.opt.start('hotloop=1',"
    "'hotexit=1','maxtrace=128'); jit.on(); "
    "jit.on(funcf_candidate,true); "
    "jit.on(funcf_readonly_candidate,true); "
    "jit.on(funcf_multi_candidate,true); "
    "jit.on(funcf_pair_candidate,true)");
  lua_pushcfunction(L,c_target); lua_setglobal(L,"c_target");
  {
    GCfunc *pair=global_lfunc(L,"funcf_pair_candidate");
    GCtrace *PT=NULL;
    test_pair_output_liveness(funcproto(pair));
    for (unsigned i=0;i<256&&!PT;i++) {
      assert(invoke(L,"funcf_run","funcf_pair_candidate",80)==
             invoke(L,"funcf_run","funcf_pair_oracle",80));
      PT=funcf_trace(L,pair);
    }
    assert(PT && PT->linktype==LJ_TRLINK_RETURN);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke(L,"funcf_run","funcf_pair_candidate",32)==
           invoke(L,"funcf_run","funcf_pair_oracle",32));
    if (lj_trace_test_root_entry_successes()==0 ||
        lj_trace_test_exit_calls()!=0)
      fprintf(stderr,"pair CALL native witness root=%u runnable=%d entries=%u exits=%u\n",
              PT->traceno,trace_runnable_acq(PT,PT->traceno),
              lj_trace_test_root_entry_successes(),lj_trace_test_exit_calls());
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke(L,"funcf_run","funcf_pair_candidate",16)==
           invoke(L,"funcf_run","funcf_pair_oracle",16));
    if (lj_trace_test_root_entry_successes()==0 ||
        lj_trace_test_exit_calls()!=0)
      fprintf(stderr,"pair CALL post-GC root=%u runnable=%d entries=%u exits=%u parent=%u exit=%u\n",
              PT->traceno,trace_runnable_acq(PT,PT->traceno),
              lj_trace_test_root_entry_successes(),lj_trace_test_exit_calls(),
              lj_trace_test_first_exit_parent(),lj_trace_test_first_exitno());
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_bad_call(L,"funcf_multi_run","funcf_pair_candidate");
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()>0 &&
           lj_trace_test_first_exit_parent()==PT->traceno);
    native_identity_exits++;
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke(L,"funcf_run","funcf_pair_candidate",16)==
           invoke(L,"funcf_run","funcf_pair_oracle",16));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_getglobal(L,"c_target"); assert(tvisfunc(L->top-1));
    admission(PT,L,funcV(L->top-1)); lua_pop(L,1);
  }
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
  {
    GCfunc *multi=global_lfunc(L,"funcf_multi_candidate");
    GCtrace *MT=NULL;
    for (unsigned i=0;i<256&&!MT;i++) {
      invoke_pair(L,"funcf_multi_run","funcf_multi_candidate",11.5,5.75,12.75);
      invoke_pair(L,"funcf_multi_run","funcf_multi_oracle",11.5,5.75,12.75);
      MT=funcf_trace(L,multi);
    }
    assert(MT && MT->linktype==LJ_TRLINK_RETURN);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_pair(L,"funcf_multi_run","funcf_multi_candidate",12.5,6.25,13.75);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_pair(L,"funcf_multi_run","funcf_multi_candidate",13.5,6.75,14.75);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    lua_getglobal(L,"funcf_multi_run");
    lua_getglobal(L,"funcf_multi_candidate"); lua_pushstring(L,"bad");
    assert(lua_pcall(L,2,2,0)!=LUA_OK);
    lua_pop(L,1);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()>0 &&
           lj_trace_test_first_exit_parent()==MT->traceno);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_pair(L,"funcf_multi_run","funcf_multi_candidate",14.5,7.25,15.75);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    multi_admission(MT);
  }

  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); "
                     "assert(ok and v)");
  lua_close(L);
  L=ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); "
    "assert(select(2,b:recv(90))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "funcf_repeat_candidate=function(x) local y=x*.5; return y,y end; "
    "funcf_repeat_oracle=function(x) local y=x*.5; return y,y end; "
    "function funcf_multi_run(fn,x) return fn(x) end; "
    "jit.off(funcf_repeat_oracle,true); jit.off(funcf_multi_run,true); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=128'); jit.on(); "
    "jit.on(funcf_repeat_candidate,true)");
  {
    GCfunc *repeat=global_lfunc(L,"funcf_repeat_candidate");
    GCtrace *MT=NULL;
    for (unsigned i=0;i<256&&!MT;i++) {
      invoke_pair(L,"funcf_multi_run","funcf_repeat_candidate",11.5,5.75,5.75);
      invoke_pair(L,"funcf_multi_run","funcf_repeat_oracle",11.5,5.75,5.75);
      MT=funcf_trace(L,repeat);
    }
    assert(MT && MT->linktype==LJ_TRLINK_RETURN);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_pair(L,"funcf_multi_run","funcf_repeat_candidate",12.5,6.25,6.25);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_pair(L,"funcf_multi_run","funcf_repeat_candidate",13.5,6.75,6.75);
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    multi_admission(MT);
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); "
                     "assert(ok and v)");
  lua_close(L);
  L=ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(a,b) assert(a:send(true,5)); "
    "assert(select(2,b:recv(90))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "jit.off(true,true); "
    "local tail_source='local function leaf(x) return x*.25 end; "
      "return function(x) return leaf(x) end'; "
    "tail_candidate=assert(loadstring(tail_source))(); "
    "local tail_oracle_chunk=assert(loadstring(tail_source)); "
    "jit.off(tail_oracle_chunk,true); tail_oracle=tail_oracle_chunk(); "
    "function tail_run(n) for i=1,n do assert(tail_candidate(11.5)==2.875) end end; "
    "jit.off(tail_run,true); "
    "jit.opt.start('hotloop=1','hotexit=1',"
    "'maxtrace=128'); jit.on(); jit.on(tail_candidate,true)");
  lua_pushcfunction(L,c_target); lua_setglobal(L,"c_target");
  {
    GCfunc *tail=global_lfunc(L,"tail_candidate");
    GCtrace *TT=NULL;
    lua_getglobal(L,"tail_run"); lua_pushinteger(L,128);
    ljt_lua_pcall(L,1,0,"tail candidate warmup");
    TT=funcf_trace(L,tail);
    assert(TT && TT->linktype==LJ_TRLINK_RETURN);
    tail_admission(TT,tail);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke_direct(L,"tail_candidate",32.5)==
           invoke_direct(L,"tail_oracle",32.5));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke_direct(L,"tail_candidate",16.5)==
           invoke_direct(L,"tail_oracle",16.5));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    invoke_bad_direct(L,"tail_candidate");
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()>0 &&
           lj_trace_test_first_exit_parent()==TT->traceno);
    native_identity_exits++;
    lua_gc(L,LUA_GCCOLLECT,0);
    lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
    assert(invoke_direct(L,"tail_candidate",16.5)==
           invoke_direct(L,"tail_oracle",16.5));
    assert(lj_trace_test_root_entry_successes()>0 &&
           lj_trace_test_exit_calls()==0);
    native_returns++;
  }
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); "
                     "assert(ok and v)");
  lua_close(L);
  printf("FUNCF leaf call OK: native returns=%u identity exits=%u "
         "semantic negatives=%u postRA negatives=%u low-loopref negatives=%u "
         "tail semantic negatives=%u tail postRA negatives=%u\n",
         native_returns,native_identity_exits,semantic_bad,postra_bad,
         low_loopref_bad,tail_semantic_bad,tail_postra_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-funcf-calls SKIP"); return 0; }
#endif
