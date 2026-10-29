/* Verify that an open CALLM consumer identity exit restores the producer's
** complete result vector and resumes in the interpreter with the right frame. */
#include <stdio.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && defined(LJ_TRACE_TEST_HELPERS)
#include <assert.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_bc.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

static unsigned cases_run;

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
                                MSize szmcode)
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
  assert(trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode);
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));
}

static void run_case(lua_State *L, unsigned nresults)
{
  const char *producer[] = {"return x", "return x,.25", "return x,.25,.5"};
  const char *consumer[] = {"return a+1", "return a+b+2", "return a+b+c+3"};
  const char *alternate[] = {"return a+17", "return a+101*b+17",
                             "return a+101*b+10007*c+17"};
  const char *params[] = {"a", "a,b", "a,b,c"};
  GCtrace *T=NULL;
  GCproto *pt;
  GCfunc *consumer_a;
  SnapNo exit;
  MSize callmpos;
  MCode *mcode;
  MSize szmcode;
  lua_Number expected;
  char install[1024];
  assert(nresults>=1u && nresults<=3u);
  snprintf(install,sizeof(install),
    "install_open_exit_case(%u,\"%s\",\"%s\",\"%s\",\"%s\")", nresults,
    producer[nresults-1u],consumer[nresults-1u],alternate[nresults-1u],
    params[nresults-1u]);
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
  assert(trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode &&
         root_trace(L)==T && trace_runnable_acq(T,T->traceno));
  assert(!trace_nchild_acq(T) && !trace_nextside_acq(T));

  /* Restore the original consumer, then require native reentry into the same
  ** selected mcode generation both before and after a full collection. */
  expected=invoke(L,"open_oracle","open_oracle_leaf","open_consumer_a",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf","open_consumer_a",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 && root_trace(L)==T &&
         trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode);
  lua_gc(L,LUA_GCCOLLECT,0);
  check_identity_exit(L,T,exit,"open_consumer_b",mcode,szmcode);
  expected=invoke(L,"open_oracle","open_oracle_leaf","open_consumer_a",64);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(invoke(L,"open_run","open_leaf","open_consumer_a",64)==expected);
  assert(lj_trace_test_root_entry_successes()>0 && root_trace(L)==T &&
         trace_mcode_acq(T)==mcode && trace_szmcode_acq(T)==szmcode);
  cases_run++;
  printf("open CALLM consumer exit results=%u exact-exit=%u same-mcode GC/reentry passed\n",
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
      "jit.off(open_oracle,true); jit.off(open_oracle_leaf,true); "
      "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64') end; "
    "jit.off(install_open_exit_case,true)");
  run_case(L,1); run_case(L,2); run_case(L,3);
  ljt_lua_dostring(L,"assert(stop:send(true,5)); local ok,v=peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("open CALLM consumer exits OK: %u runtime shapes\n",cases_run);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-open-callm-exits SKIP"); return 0; }
#endif
