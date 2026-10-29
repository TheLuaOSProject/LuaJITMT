/* Bounded dead-numeric CALLM cones, selected native exits, and independent
** semantic/post-RA rejection of live or snapshot-bound mutations. */
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
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"
#include "lib/lua_fixture_helpers.h"

static GCfunc *global_function(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1)); fn=funcV(L->top-1);
  assert(isluafunc(fn)); lua_pop(L,1);
  return fn;
}

static lua_Number invoke(lua_State *L, const char *runner,
                         const char *producer)
{
  lua_Number result;
  lua_getglobal(L,runner); lua_getglobal(L,producer); lua_pushinteger(L,96);
  ljt_lua_pcall(L,2,1,"dead numeric CALLM");
  assert(lua_isnumber(L,-1)); result=lua_tonumber(L,-1); lua_pop(L,1);
  assert(lj_tg_load_jit_base(L2TG(L))==NULL &&
         !lj_tg_in_native_acq(L2TG(L)));
  return result;
}

static GCtrace *root_trace(lua_State *L)
{
  GCproto *pt=funcproto(global_function(L,"leaf_run"));
  const BCIns *bc=proto_bc(pt);
  MSize i;
  for(i=1;i<pt->sizebc;i++) {
    BCIns ins=(BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if(bc_op(ins)==BC_JFORL) {
      TraceNo no=bc_d(ins);
      GCtrace *T=traceref_safe(L2J(L),no);
      if(T && trace_runnable_acq(T,no) && !T->root &&
         trace_startpt_acq(T)==pt && T->linktype==LJ_TRLINK_LOOP &&
         T->link==T->traceno) return T;
    }
  }
  return NULL;
}

static int dead_numeric_op(IRIns p)
{
  return p.t.irt==IRT_NUM && !ra_hasreg(p.r) && !ra_hasspill(p.s) &&
    (p.o==IR_ADD || p.o==IR_SUB || p.o==IR_MUL || p.o==IR_DIV);
}

static int ir_uses(const IRIns *ir, IRRef use, IRRef value)
{
  IRIns p=ir_load_acq(&ir[use]);
  IRMode mode;
  if(p.o>=IR__MAX) return 0;
  mode=lj_ir_mode[p.o];
  return (irm_op1(mode)==IRMref && p.op1==value) ||
         (irm_op2(mode)==IRMref && p.op2==value);
}

static void run_proof_mutations(lua_State *L, GCtrace *T)
{
  Arm64TraceFixture f;
  GCproto *pt;
  MSize callm=UINT32_MAX, result_entry=UINT32_MAX;
  IRRef dag_parent=0, dag_user=0, dag_live=0, wrong=0;
  SnapEntry oldmap, oldpost;
  unsigned semantic_bad=0, postra_bad=0;
  arm64_fixture_init_trace(&f,L,"leaf_run_clone",T->traceno);
  pt=trace_startpt_acq(T);
  memcpy(f.postmap,trace_snapmap_acq(T),T->nsnapmap*sizeof(SnapEntry));
  f.view.ir=f.postra;
  f.view.owner=T;
  f.view.owner_private=1;
  f.view.proto=pt;
  f.view.proto_bc=proto_bc(pt);
  f.view.startpc=trace_startpc_acq(T);
  f.view.proto_sizebc=pt->sizebc;
  for(MSize p=1;p+1u<pt->sizebc;p++)
    if(bc_op((BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[p]))==BC_CALLM) {
      assert(callm==UINT32_MAX); callm=p;
    }
  assert(callm!=UINT32_MAX && callm>1u &&
         bc_op(proto_bc(pt)[callm-1u])==BC_CALL &&
         bc_b(proto_bc(pt)[callm-1u])==0u &&
         bc_a(proto_bc(pt)[callm-1u])==bc_a(proto_bc(pt)[callm])+1u+LJ_FR2);

  /* Select a real unlocated intermediate-to-terminal edge from the retained
  ** body DAG. The baseline proof below makes this selection non-vacuous. */
  for(IRRef r=REF_FIRST;r<f.semantic_nins && !dag_parent;r++) {
    IRIns p=ir_load_acq(&f.postra[r]);
    if(!dead_numeric_op(p)) continue;
    for(IRRef u=r+1u;u<f.semantic_nins;u++) {
      IRIns user=ir_load_acq(&f.postra[u]);
      if(ir_uses(f.postra,u,r) && dead_numeric_op(user) &&
         user.o==IR_MUL) {
        for(IRRef live=u+1u;live<f.semantic_nins;live++) {
          IRIns consumer=ir_load_acq(&f.postra[live]);
          if(ir_uses(f.postra,live,u) && dead_numeric_op(consumer) &&
             consumer.o==IR_ADD) {
            dag_parent=r; dag_user=u; dag_live=live; break;
          }
        }
        if(dag_parent) break;
      }
    }
  }
  if (!(dag_parent && dag_user && dag_live &&
        lj_asm_arm64_ir_admit(f.J,&f.semantic,NULL) &&
        lj_asm_arm64_postra_admit(&f.view,NULL))) {
    fprintf(stderr,"dead numeric baseline: source=%u MUL=%u ADD=%u semantic=%d postRA=%d\n",
            (unsigned)dag_parent,(unsigned)dag_user,(unsigned)dag_live,
            lj_asm_arm64_ir_admit(f.J,&f.semantic,NULL),
            lj_asm_arm64_postra_admit(&f.view,NULL));
    abort();
  }

  /* Bind the CALLM continuation's final returned argument to another
  ** valid-looking NUM ref. Both independent source replays must reject. */
  {
    GCproto *clonept=trace_startpt_acq(&f.semantic);
    MSize slot=1u+LJ_FR2+bc_a(proto_bc(clonept)[callm])+1u+LJ_FR2+2u;
    for(SnapNo s=0;s<f.semantic.nsnap;s++) {
      SnapShot *snap=&f.snaps[s];
      uint64_t footer;
      memcpy(&footer,&f.map[snap->mapofs+snap->nent],sizeof(footer));
      if((uint8_t)footer ||
         (const BCIns *)(uintptr_t)(footer>>8)!=proto_bc(clonept)+callm) continue;
      for(MSize i=0;i<snap->nent;i++) {
        MSize at=snap->mapofs+i;
        if(snap_slot(f.map[at])==slot && !(f.map[at]&0x00ff0000u)) {
          assert(result_entry==UINT32_MAX); result_entry=at;
        }
      }
    }
  }
  assert(result_entry!=UINT32_MAX);
  oldmap=f.map[result_entry]; oldpost=f.postmap[result_entry];
  wrong=dag_parent;
  assert(wrong!=snap_ref(oldmap));
  f.map[result_entry]=SNAP(snap_slot(oldmap),0,wrong);
  assert(!lj_asm_arm64_ir_admit(f.J,&f.semantic,NULL)); semantic_bad++;
  f.map[result_entry]=oldmap;
  f.postmap[result_entry]=SNAP(snap_slot(oldpost),0,wrong);
  assert(!lj_asm_arm64_postra_admit(&f.view,NULL)); postra_bad++;
  f.postmap[result_entry]=oldpost;

  /* A live allocated ADD cannot hide behind an unlocated MUL. The complete
  ** source -> MUL -> ADD chain must be rejected even though the MUL alone
  ** could legally fuse into the ADD on ARM64. */
  f.postra[dag_live].r=RID_D0;
  assert(!lj_asm_arm64_postra_admit(&f.view,NULL)); postra_bad++;
  f.postra[dag_live]=f.good[dag_live];
  arm64_fixture_reset(&f);
  assert(lj_asm_arm64_ir_admit(f.J,&f.semantic,NULL) &&
         lj_asm_arm64_postra_admit(&f.view,NULL));
  arm64_fixture_free(&f);
  printf("dead numeric CALLM: semantic-negative=%u postRA-negative=%u DAG-edge=%u->%u->%u\n",
         semantic_bad,postra_bad,(unsigned)dag_parent,(unsigned)dag_user,
         (unsigned)dag_live);
}

int main(void)
{
  /* This fixture uses an arbitrary trace id; it intentionally selects the
  ** explicit-trace initializer below instead of the trace-1 convenience. */
  (void)arm64_fixture_init;
  lua_State *L=ljt_lua_newstate_openlibs();
  GCtrace *T;
  lua_Number expected;
  MCode *code;
  MCode *codecopy;
  MSize size;
  uint64_t generation;
  assert(LJ_FR2==1);
  ljt_lua_dostring(L,
    "local th=require('threading'); ready=th.channel(1); stop=th.channel(1); "
    "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
      "assert(select(2,s:recv(120000))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); jit.off(true,true); "
    "leaf_candidate=function(x) return (x*.25+1)*.5,.25,.5 end; "
    "leaf_oracle=function(x) return (x*.25+1)*.5,.25,.5 end; "
    "local src='return function(fn,n) local add=function() return 1.25 end; "
      "local sum=.5; for i=1,n do sum=sum+add(fn(i+.5)) end; return sum end'; "
    "leaf_run=assert(loadstring(src,'@dead-numeric-root'))(); "
    "leaf_run_clone=assert(loadstring(src,'@dead-numeric-clone'))(); "
    "leaf_oracle_run=assert(loadstring(src,'@dead-numeric-oracle'))(); "
    "jit.off(leaf_oracle,true); jit.off(leaf_run_clone,true); "
    "jit.off(leaf_oracle_run,true); jit.on(leaf_candidate,true); "
    "jit.on(leaf_run,true); jit.on(); jit.opt.start('hotloop=1',"
      "'hotexit=1000','maxtrace=64')");
  expected=invoke(L,"leaf_oracle_run","leaf_oracle");
  for(unsigned i=0;i<16;i++) assert(invoke(L,"leaf_run","leaf_candidate")==expected);
  T=root_trace(L); assert(T);
  code=trace_mcode_acq(T); size=trace_szmcode_acq(T);
  generation=la_load64_acq(&T->arm64_generation);
  assert(code && size && generation);
  codecopy=(MCode *)malloc(size); assert(codecopy);
  memcpy(codecopy,code,size);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"leaf_run","leaf_candidate")==expected);
  assert(root_trace(L)==T && lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno);
  lua_gc(L,LUA_GCCOLLECT,0);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"leaf_run","leaf_candidate")==expected && root_trace(L)==T);
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 &&
         lj_trace_test_first_exit_parent()==T->traceno &&
         trace_mcode_acq(T)==code && trace_szmcode_acq(T)==size &&
         la_load64_acq(&T->arm64_generation)==generation &&
         memcmp(codecopy,trace_mcode_acq(T),size)==0);
  run_proof_mutations(L,T);
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  free(codecopy);
  printf("dead numeric CALLM OK: exact root exits across GC, code reused with live peer\n");
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-lua-dead-numeric SKIP"); return 0; }
#endif
