/*
** Native ARM64 proof for the first generalized scalar side-loop topology.
** The side is recorded after an INT root sees a NUM accumulator, then closes
** naturally at its own loop label. Repeated parent exits must target its body
** directly. Successful native entry and bounded machine-code exits are
** required while the fractional calls run.
*/
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_trace.h"

extern int lj_mcode_test_arm64_range(uintptr_t lo, uintptr_t hi,
				     uintptr_t *min, uintptr_t *max);
extern int lj_mcode_test_arm64_expandrange(uintptr_t offset,
					   uintptr_t *min, uintptr_t *max);
extern void lj_mcode_test_arm64_range_extensions_reset(void);
extern uint32_t lj_mcode_test_arm64_range_extensions_count(void);

static void run(lua_State *L, const char *source)
{
  int status = luaL_dostring(L, source);
  if (status != 0) {
    fprintf(stderr, "side adaptation Lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    assert(status == 0);
  }
}

static GCtrace *find_side(lua_State *L, jit_State *J, const char *name,
                         GCtrace **parentp, TraceNo *parentno)
{
  MSize n;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *p = traceref_safe(J, (TraceNo)n);
    TraceNo childno;
    GCtrace *selected = NULL;
    MSize count, i;
    if (!trace_runnable_acq(p, (TraceNo)n) ||
        trace_root_acq(p) != 0 || trace_startpt_acq(p) != pt)
      continue;
    count = trace_nchild_acq(p);
    if (count == 0 || count >= trace_sizetrace_acq(J)) continue;
    childno = trace_nextside_acq(p);
    for (i = 0; i < count; i++) {
      GCtrace *child = traceref_safe(J, childno);
      IRIns base;
      assert(trace_runnable_acq(child, childno));
      assert(trace_root_acq(child) == (TraceNo)n);
      base = trace_ir_acq(child)[REF_BASE];
      assert(base.o == IR_BASE && (TraceNo)base.op1 == (TraceNo)n);
      assert(base.op2 < trace_nsnap_acq(p));
      assert(trace_exittarget_arm64_acq(p, (ExitNo)base.op2) ==
             trace_mcode_acq(child));
      if (trace_linktype_acq(child) == LJ_TRLINK_LOOP) {
        assert(trace_link_acq(child) == childno);
        assert(trace_mcloop_acq(child) > 0 &&
               trace_mcloop_acq(child) < trace_szmcode_acq(child) &&
               (trace_mcloop_acq(child) & (sizeof(MCode)-1u)) == 0);
        selected = child;
      } else {
        assert(trace_linktype_acq(child) == LJ_TRLINK_ROOT &&
               trace_link_acq(child) == (TraceNo)n &&
               trace_mcloop_acq(child) == 0);
      }
      childno = trace_nextside_acq(child);
    }
    assert(childno == 0);
    if (selected) {
      *parentp = p;
      *parentno = (TraceNo)n;
      return selected;
    }
  }
  return NULL;
}

static GCtrace *find_root_for_proto(jit_State *J, GCproto *pt)
{
  MSize n;
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *T = traceref_safe(J, (TraceNo)n);
    if (trace_runnable_acq(T, (TraceNo)n) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL)
      return T;
  }
  return NULL;
}

static GCtrace *find_largest_root_for_proto(jit_State *J, GCproto *pt)
{
  GCtrace *best = NULL;
  MSize n;
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *T = traceref_safe(J, (TraceNo)n);
    if (trace_runnable_acq(T, (TraceNo)n) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        (best == NULL || trace_nchild_acq(T) > trace_nchild_acq(best)))
      best = T;
  }
  return best;
}

static MSize side_depth(jit_State *J, GCtrace *T)
{
  MSize depth = 0, limit;
  TraceNo no;
  TraceVec *tv = tracevec_acq(J);
  GCtrace *root;
  if (!tv || !T) return 0;
  no = trace_traceno_acq(T);
  root = traceref_safe(J, trace_root_acq(T));
  if (!trace_runnable_acq(root, trace_root_acq(T))) return 0;
  limit = trace_nchild_acq(root)+1u;
  while (trace_root_acq(T) != 0 && depth < limit) {
    IRIns base = trace_ir_acq(T)[REF_BASE];
    TraceNo parent = (TraceNo)base.op1;
    if (base.o != IR_BASE || parent == 0 || parent == no ||
        (MSize)parent >= tv->sizetrace) return 0;
    T = traceref_safe(J, parent);
    if (!trace_runnable_acq(T, parent)) return 0;
    no = parent;
    depth++;
  }
  return trace_root_acq(T) == 0 ? depth : 0;
}

static GCtrace *find_deepest_side(lua_State *L, jit_State *J,
                                  const char *name, MSize minimum_depth)
{
  GCproto *pt;
  GCtrace *deepest = NULL;
  MSize best = 0, n;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *T = traceref_safe(J, (TraceNo)n);
    MSize depth;
    if (!trace_runnable_acq(T, (TraceNo)n) ||
        trace_startpt_acq(T) != pt || trace_root_acq(T) == 0) continue;
    depth = side_depth(J, T);
    if (depth > best) best = depth, deepest = T;
  }
  lj_gc2_smr_read_leave(J2G(J));
  return best >= minimum_depth ? deepest : NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCtrace *parent, *child;
  TraceNo parentno, childno;
  IRIns base;
  ExitNo exitno;
  {
    const uintptr_t range = ((uintptr_t)1 << LJ_TARGET_JUMPRANGE) - 0x10000u;
    const uintptr_t base = (uintptr_t)0x100000000ull;
    uintptr_t min, max;
    assert(lj_mcode_test_arm64_range(base, base + range, &min, &max));
    assert(min == base && max == base + range);
    assert(lj_mcode_test_arm64_range(base + 0x1234u,
				       base + range - 0x10000u,
				       &min, &max));
    assert(min <= base + 0x1234u &&
	   max >= base + range - 0x10000u);
    assert(!lj_mcode_test_arm64_range(base, base + range + 1u,
				      &min, &max));
    assert(!lj_mcode_test_arm64_range(base + 1u, base, &min, &max));
    assert(!lj_mcode_test_arm64_range(UINTPTR_MAX - 0x1000u,
				      UINTPTR_MAX, &min, &max));
    assert(lj_mcode_test_arm64_expandrange(range / 2u, &min, &max));
    assert(max - min == range);
    assert(!lj_mcode_test_arm64_expandrange(range + 0x10000u,
					    &min, &max));
  }
  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  lj_trace_test_root_entry_reset();
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=2','maxtrace=64'); "
    "kernel=function(n,step) local i,sum=1,0 "
    "while i<=n do sum=sum+step; i=i+1 end return sum end; "
    "for i=1,8 do assert(kernel(64,1)==64) end; "
    "for i=1,8 do assert(kernel(64,1.25)==80) end");
  child = find_side(L, J, "kernel", &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_root_acq(child) == parentno);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    if (contract != 0x1fffu) fprintf(stderr, "copied side contract=%#x\n", contract);
    assert(contract == 0x1fffu); }
  base = trace_ir_acq(child)[REF_BASE];
  exitno = (ExitNo)base.op2;
  assert(exitno < trace_nsnap_acq(parent));
  /* The selected parent exit really points at the child machine body, with
  ** arm64e authentication normalized by the production accessor. */
  assert(trace_exittarget_arm64_acq(parent, exitno) == trace_mcode_acq(child));

  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(kernel(64,1.25)==80) end");
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  /* Every successful handoff follows the post-publish child/body and exact
  ** target checks. Full function completion accounts for native exits; cap
  ** exits at two per call to rule out an interpreter exit per loop iteration. */
  assert(lj_trace_test_root_entry_publishes() ==
         lj_trace_test_root_entry_successes());
  assert(lj_trace_test_root_entry_successes() <= 2u*100u);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_exit_calls() <= 2u*100u);

  /* Short and long bodies must execute the adapted native traces and preserve
  ** results. A full collection must retain the function/proto and generations. */
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "assert(kernel(8,1.25)==10); assert(kernel(1024,1.25)==1280); "
         "collectgarbage('collect')");
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_publishes() ==
         lj_trace_test_root_entry_successes());
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_exit_calls() <= 4u);
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(kernel(64,1.25)==80) end");
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_publishes() ==
         lj_trace_test_root_entry_successes());
  assert(lj_trace_test_root_entry_successes() <= 2u*100u);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_exit_calls() <= 2u*100u);

  /* Overflow must retain LuaJIT numeric semantics across the loop boundary. */
  run(L, "overflow_kernel=function(n) local i,sum=1,2147483600 "
         "while i<=n do sum=sum+1; i=i+1 end return sum end; "
         "assert(overflow_kernel(128)==2147483728)");

  /* A full trace flush must retire both sides cleanly; the same INT-to-NUM
  ** transition can publish and enter a fresh child generation afterward. */
  run(L, "local jit=require('jit'); jit.flush(); "
         "for i=1,8 do assert(kernel(64,1)==64) end; "
         "for i=1,8 do assert(kernel(64,1.25)==80) end");
  child = find_side(L, J, "kernel", &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_root_acq(child) == parentno);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    assert(contract == 0x1fffu); }
  base = trace_ir_acq(child)[REF_BASE];
  exitno = (ExitNo)base.op2;
  assert(exitno < trace_nsnap_acq(parent));
  assert(trace_exittarget_arm64_acq(parent, exitno) == trace_mcode_acq(child));
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(kernel(64,1.25)==80) end");
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_publishes() ==
         lj_trace_test_root_entry_successes());
  assert(lj_trace_test_root_entry_successes() <= 2u*100u);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_exit_calls() <= 2u*100u);

  /* An overflow exit mid-loop carries parent state into a side trace. */
  run(L, "local jit=require('jit'); jit.flush(); "
         "overflow_kernel=function(n,step,sum) local i=1 "
         "while i<=n do sum=sum+step; i=i+1 end return sum end; "
         "for i=1,8 do assert(overflow_kernel(64,1,0)==64) end; "
         "for i=1,8 do assert(overflow_kernel(64,1,2147483600)==2147483664) end");
  child = find_side(L, J, "overflow_kernel", &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_root_acq(child) == parentno);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    if (contract != 0x1fffu) fprintf(stderr, "overflow copied contract=%#x\n", contract);
    assert(contract == 0x1fffu); }
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(overflow_kernel(64,1,2147483600)==2147483664) end");
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_publishes() ==
         lj_trace_test_root_entry_successes());
  assert(lj_trace_test_root_entry_successes() <= 2u*100u);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_exit_calls() <= 2u*100u);

  /* A later type mismatch must leave the immutable child generation cleanly
  ** and preserve ordinary Lua arithmetic errors. */
  run(L, "local ok,err=pcall(kernel,64,true); "
         "assert(not ok and string.find(err,'arithmetic'))");

  /* Reach a third side level using distinct guarded paths, then prove the
  ** copied immediate-parent chain rejects skipped, foreign-root and stale-
  ** generation mutations in both independent admission passes. */
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=128'); "
    "depth_kernel=function(a,b,c,d,n) local s=.5 "
    "for i=1,n do if a==1 then if b==1 then if c==1 then "
    "if d==1 then s=s+i else s=s+i*.125 end "
    "else if d==1 then s=s+i*.25 else s=s+i*.0625 end end "
    "else if c==1 then if d==1 then s=s+i*.5 else s=s+i*.03125 end "
    "else if d==1 then s=s+i*.125 else s=s+i*.015625 end end end "
    "else if b==1 then if c==1 then if d==1 then s=s-i "
    "else s=s-i*.125 end else if d==1 then s=s-i*.25 "
    "else s=s-i*.0625 end end else if c==1 then if d==1 then "
    "s=s-i*.5 else s=s-i*.03125 end else if d==1 then "
    "s=s-i*.125 else s=s-i*.015625 end end end end end "
    "return s end; "
    "for k=1,100 do depth_kernel(1,1,1,1,48) end; "
    "for k=1,100 do depth_kernel(2,1,1,1,48) end; "
    "for k=1,100 do depth_kernel(2,2,1,1,48) end; "
    "for k=1,100 do depth_kernel(2,2,2,1,48) end");
  child = find_deepest_side(L, J, "depth_kernel", 3u);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J,
      childno);
    const uint32_t ancestry_mutations = 0xf87f0000u;
    if ((contract & ancestry_mutations) != ancestry_mutations)
      fprintf(stderr, "ancestry mutations expected=%08x observed=%08x\n",
              ancestry_mutations, contract);
    assert((contract & ancestry_mutations) == ancestry_mutations); }
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
	run(L, "for k=1,64 do assert(depth_kernel(2,2,2,1,48)==-146.5) end");
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_exit_calls() > 0);

  /* Retire a middle ancestor in a real depth-three graph. Its descendant
  ** must retire with it, while the direct side and canonical root keep their
  ** published code and generation and remain enterable after full GC. */
  run(L,
    "depth_oracle=assert(loadstring(string.dump(depth_kernel))); "
    "jit.off(depth_oracle,true)");
  run(L, "assert(depth_kernel(2,1,1,1,48)==-1175.5)");
  run(L, "assert(depth_oracle(2,1,1,1,48)==-1175.5)");
  {
    GCtrace *depth3 = child, *depth2, *depth1, *depth_root;
    TraceNo depth3no = childno, depth2no, depth1no, depth_rootno;
    ExitNo direct_exit;
    MCode *depth_root_code, *depth1_code;
    uint64_t depth_root_generation, depth1_generation;
    uint32_t summary;
    uint32_t scoped_proof;
    IRIns edge;
    edge = trace_ir_acq(depth3)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    depth2no = (TraceNo)edge.op1;
    depth2 = traceref_safe(J, depth2no);
    assert(depth2 != NULL && trace_runnable_acq(depth2, depth2no));
    edge = trace_ir_acq(depth2)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    depth1no = (TraceNo)edge.op1;
    depth1 = traceref_safe(J, depth1no);
    assert(depth1 != NULL && trace_runnable_acq(depth1, depth1no));
    edge = trace_ir_acq(depth1)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    depth_rootno = (TraceNo)edge.op1;
    depth_root = traceref_safe(J, depth_rootno);
    assert(depth_root != NULL && trace_runnable_acq(depth_root, depth_rootno));
    assert(trace_root_acq(depth3) == depth_rootno &&
           trace_root_acq(depth2) == depth_rootno &&
           trace_root_acq(depth1) == depth_rootno);
    assert(side_depth(J, depth3) == 3u && side_depth(J, depth2) == 2u &&
           side_depth(J, depth1) == 1u);
    assert(trace_nchild_acq(depth_root) == 3u &&
           trace_nextside_acq(depth_root) == depth3no);
    summary = lj_trace_test_arm64_side_graph_summary(J, depth_rootno);
    assert(summary == ((3u << 16) | 3u));
    scoped_proof = lj_trace_test_arm64_scoped_retire_proof(
      J, depth_rootno, depth2no);
    assert(scoped_proof == 15u);
    edge = trace_ir_acq(depth1)[REF_BASE];
    direct_exit = (ExitNo)edge.op2;
    assert(direct_exit < trace_nsnap_acq(depth_root) &&
           trace_exittarget_arm64_acq(depth_root, direct_exit) ==
             trace_mcode_acq(depth1));
    depth_root_code = trace_mcode_acq(depth_root);
    depth1_code = trace_mcode_acq(depth1);
    depth_root_generation = depth_root->arm64_generation;
    depth1_generation = depth1->arm64_generation;

    assert(lj_trace_flushscope(J, depth2no) == 1u);
    assert(!trace_runnable_acq(depth2, depth2no) &&
           !trace_runnable_acq(depth3, depth3no));
    assert(trace_runnable_acq(depth1, depth1no) &&
           trace_runnable_acq(depth_root, depth_rootno));
    assert(trace_mcode_acq(depth1) == depth1_code &&
           trace_mcode_acq(depth_root) == depth_root_code &&
           depth1->arm64_generation == depth1_generation &&
           depth_root->arm64_generation == depth_root_generation);
    assert(trace_nchild_acq(depth_root) == 1u &&
           trace_nextside_acq(depth_root) == depth1no &&
           trace_nextside_acq(depth1) == 0);
    assert(lj_trace_test_arm64_side_graph_summary(J, depth_rootno) ==
           ((1u << 16) | 1u));
    assert(trace_exittarget_arm64_acq(depth_root, direct_exit) ==
           depth1_code);

    lua_gc(L, LUA_GCCOLLECT, 0);
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    run(L,
      "for k=1,64 do assert(depth_kernel(2,1,1,1,48)=="
        "depth_oracle(2,1,1,1,48)) end");
    assert(lj_trace_test_root_entry_publishes() > 0 &&
           lj_trace_test_root_entry_successes() > 0 &&
           lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_first_exit_parent() == depth1no &&
           lj_trace_test_first_exitno() < trace_nsnap_acq(depth1) &&
           lj_trace_test_exit_calls() > 0);
    assert(trace_runnable_acq(depth1, depth1no) &&
           trace_runnable_acq(depth_root, depth_rootno) &&
           trace_mcode_acq(depth1) == depth1_code &&
           trace_mcode_acq(depth_root) == depth_root_code &&
           depth1->arm64_generation == depth1_generation &&
           depth_root->arm64_generation == depth_root_generation &&
           trace_nchild_acq(depth_root) == 1u &&
           lj_trace_test_arm64_side_graph_summary(J, depth_rootno) ==
             ((1u << 16) | 1u));
  }

  /* Retain the historical wide direct-child path beyond the 64-entry deep
  ** ancestry index. The real root owns 65 separately selected cold exits;
  ** the summary helper validates its topology and retirement eligibility. */
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4096',"
    "'maxside=256','maxrecord=4000'); "
    "local parts={'return function(z,a,x,n) local s=.5 for i=1,n do "
    "if z==1 then if a==1 then s=s+i*.001 else s=s+i*.002 end end '}; "
    "for k=1,65 do parts[#parts+1]="
    "('if x==%d then s=s+i*0.0001 end '):format(k) end; "
    "parts[#parts+1]='end return s end'; local body=table.concat(parts); "
    "wide_kernel=assert(loadstring(body))(); "
    "wide_oracle=assert(loadstring(body))(); jit.off(wide_oracle,true); "
    "for i=1,100 do assert(wide_kernel(0,0,0,32)==wide_oracle(0,0,0,32)) end; "
    "for i=1,20 do assert(wide_kernel(1,1,0,32)==wide_oracle(1,1,0,32)) end; "
    "for k=1,65 do for i=1,10 do "
    "assert(wide_kernel(0,0,k,32)==wide_oracle(0,0,k,32)) end end; "
    "for i=1,20 do assert(wide_kernel(1,2,0,32)==wide_oracle(1,2,0,32)) end; "
    "collectgarbage('collect'); "
    "for k=1,65 do assert(wide_kernel(0,0,k,32)==wide_oracle(0,0,k,32)) end; "
    "for i=1,20 do assert(wide_kernel(1,2,0,32)==wide_oracle(1,2,0,32)) end");
  lua_getglobal(L, "wide_kernel");
  assert(lua_isfunction(L, -1));
  {
    GCproto *pt = funcproto(funcV(L->top-1));
    GCtrace *wide_root = find_root_for_proto(J, pt);
    uint32_t summary;
    assert(wide_root != NULL && trace_nchild_acq(wide_root) == 67u);
    summary = lj_trace_test_arm64_side_graph_summary(
      J, trace_traceno_acq(wide_root));
    if (summary != ((2u << 16) | 67u))
      fprintf(stderr, "wide graph summary expected=%#x observed=%#x count=%u\n",
              (2u << 16) | 67u, summary, trace_nchild_acq(wide_root));
    assert(summary == ((2u << 16) | 67u));
    run(L, "collectgarbage('collect'); "
           "for k=1,65 do assert(wide_kernel(0,0,k,32)==wide_oracle(0,0,k,32)) end; "
           "for i=1,20 do assert(wide_kernel(1,2,0,32)==wide_oracle(1,2,0,32)) end");
    assert(lj_trace_test_arm64_side_graph_summary(
      J, trace_traceno_acq(wide_root)) == summary);
    run(L, "require('jit').flush(); "
           "for i=1,100 do assert(wide_kernel(0,0,0,32)==wide_oracle(0,0,0,32)) end; "
           "for i=1,10 do assert(wide_kernel(1,2,0,32)==wide_oracle(1,2,0,32)) end");
  }
  lua_pop(L, 1);

  /* Extend a live 64-child graph with a depth-three member. Existing root
  ** code and independent siblings must remain runnable across the growth. */
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4096',"
    "'maxside=256','maxrecord=4000'); "
    "function make_cap_kernel(nx) "
    "local p={'return function(x,y,w,n) local s=.5 for i=1,n do "
    "if x==1 then if y==1 then s=s+i*.001 else "
    "if w==1 then s=s+i*.002 else s=s+i*.003 end end end '}; "
    "for k=2,nx do p[#p+1]=('if x==%d then s=s+i*0.0001 end '):format(k) end; "
    "p[#p+1]='end return s end'; local src=table.concat(p); "
    "local f=assert(loadstring(src))(); local o=assert(loadstring(src))(); "
    "jit.off(o,true); return f,o end; "
    "cap_kernel,cap_oracle=make_cap_kernel(63); "
    "local function check(x,y,w) "
    "assert(cap_kernel(x,y,w,32)==cap_oracle(x,y,w,32)) end; "
    "for i=1,100 do check(0,0,0) end; "
    "for i=1,50 do check(1,1,1) end; "
    "for i=1,50 do check(1,2,1) end; "
    "for k=2,63 do for i=1,100 do check(k,0,0) end end; "
    "for k=2,63 do check(k,0,0) end; "
    "collectgarbage('collect')");
  lua_getglobal(L, "cap_kernel");
  assert(lua_isfunction(L, -1));
  {
    GCproto *pt = funcproto(funcV(L->top-1));
    GCtrace *cap_root = find_largest_root_for_proto(J, pt);
    TraceNo rootno;
    MCode *root_mcode;
    uint32_t summary, successes;
    if (cap_root == NULL || trace_nchild_acq(cap_root) != 64u)
      fprintf(stderr, "cap graph expected=64 root=%p count=%u trace_slots=%u\n",
              (void *)cap_root,
              cap_root != NULL ? trace_nchild_acq(cap_root) : 0u,
              (unsigned)trace_sizetrace_acq(J));
    assert(cap_root != NULL && trace_nchild_acq(cap_root) == 64u);
    rootno = trace_traceno_acq(cap_root);
    root_mcode = trace_mcode_acq(cap_root);
    summary = lj_trace_test_arm64_side_graph_summary(J, rootno);
    if (summary != ((2u << 16) | 64u))
      fprintf(stderr, "cap graph summary expected=%#x observed=%#x\n",
              (2u << 16) | 64u, summary);
    assert(summary == ((2u << 16) | 64u));
    successes = lj_trace_test_root_entry_successes();
    run(L,
      "for i=1,50 do assert(cap_kernel(1,2,2,32)=="
      "cap_oracle(1,2,2,32)) end; "
      "for i=1,50 do assert(cap_kernel(2,0,0,32)=="
      "cap_oracle(2,0,0,32)) end; collectgarbage('collect')");
    assert(trace_runnable_acq(cap_root, rootno) &&
           trace_traceno_acq(cap_root) == rootno &&
           trace_mcode_acq(cap_root) == root_mcode &&
           trace_nchild_acq(cap_root) == 65u);
    assert(lj_trace_test_arm64_side_graph_summary(J, rootno) ==
           ((3u << 16) | 65u));
    assert(lj_trace_test_root_entry_successes() > successes);
  }
  lua_pop(L, 1);

  /* A pre-existing depth-three graph can also grow past 64 through a new
  ** direct sibling. Scoped removal must preserve all independent siblings. */
  lj_mcode_test_arm64_range_extensions_reset();
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=512',"
    "'maxside=256','maxrecord=4000'); "
    "cap_deep_kernel,cap_deep_oracle=make_cap_kernel(63); "
    "local function check(x,y,w) "
    "assert(cap_deep_kernel(x,y,w,32)==cap_deep_oracle(x,y,w,32)) end; "
    "for i=1,100 do check(0,0,0) end; "
    "for i=1,50 do check(1,1,1) end; "
    "for i=1,50 do check(1,2,1) end; "
    "for k=2,62 do for i=1,100 do check(k,0,0) end end; "
    "for k=2,62 do check(k,0,0) end; "
    "for i=1,50 do check(1,2,2) end; collectgarbage('collect')");
  lua_getglobal(L, "cap_deep_kernel");
  assert(lua_isfunction(L, -1));
  {
    GCproto *pt = funcproto(funcV(L->top-1));
    GCtrace *cap_root = find_largest_root_for_proto(J, pt);
    TraceNo rootno;
    MCode *root_mcode;
    uint32_t summary, successes;
    if (cap_root == NULL || trace_nchild_acq(cap_root) != 64u)
      fprintf(stderr, "deep cap graph expected=64 root=%p count=%u trace_slots=%u\n",
              (void *)cap_root,
              cap_root != NULL ? trace_nchild_acq(cap_root) : 0u,
              (unsigned)trace_sizetrace_acq(J));
    assert(cap_root != NULL && trace_nchild_acq(cap_root) == 64u);
    rootno = trace_traceno_acq(cap_root);
    root_mcode = trace_mcode_acq(cap_root);
    summary = lj_trace_test_arm64_side_graph_summary(J, rootno);
    if (summary != ((3u << 16) | 64u))
      fprintf(stderr, "deep cap graph summary expected=%#x observed=%#x\n",
              (3u << 16) | 64u, summary);
    assert(summary == ((3u << 16) | 64u));
    printf("ARM64 MAP_JIT range extensions during cap graph: %u\n",
	   lj_mcode_test_arm64_range_extensions_count());
    successes = lj_trace_test_root_entry_successes();
    run(L,
      "for i=1,50 do assert(cap_deep_kernel(63,0,0,32)=="
      "cap_deep_oracle(63,0,0,32)) end; "
      "for i=1,50 do assert(cap_deep_kernel(2,0,0,32)=="
      "cap_deep_oracle(2,0,0,32)) end; collectgarbage('collect')");
    assert(trace_runnable_acq(cap_root, rootno) &&
           trace_traceno_acq(cap_root) == rootno &&
           trace_mcode_acq(cap_root) == root_mcode &&
           trace_nchild_acq(cap_root) == 65u);
    assert(lj_trace_test_arm64_side_graph_summary(J, rootno) ==
           ((3u << 16) | 65u));
    assert(lj_trace_test_root_entry_successes() > successes);
    {
      GCtrace *deepest = find_deepest_side(
        L, J, "cap_deep_kernel", 3u);
      TraceNo deepestno, parentno;
      GCtrace *parent;
      IRIns base;
      assert(deepest != NULL);
      deepestno = trace_traceno_acq(deepest);
      base = trace_ir_acq(deepest)[REF_BASE];
      assert(base.o == IR_BASE && (TraceNo)base.op1 != rootno);
      parentno = (TraceNo)base.op1;
      parent = traceref_safe(J, parentno);
      assert(parent != NULL && trace_runnable_acq(parent, parentno));
      assert(lj_trace_flushscope(J, deepestno) == 1u);
      assert(!trace_runnable_acq(deepest, deepestno) &&
             trace_runnable_acq(parent, parentno) &&
             trace_runnable_acq(cap_root, rootno) &&
             trace_nchild_acq(cap_root) == 64u);
      assert(lj_trace_test_arm64_side_graph_summary(J, rootno) ==
             ((2u << 16) | 64u));
      successes = lj_trace_test_root_entry_successes();
      run(L,
        "for i=1,50 do assert(cap_deep_kernel(2,0,0,32)=="
        "cap_deep_oracle(2,0,0,32)) end; collectgarbage('collect')");
      assert(trace_mcode_acq(cap_root) == root_mcode &&
             trace_runnable_acq(parent, parentno) &&
             trace_runnable_acq(cap_root, rootno) &&
             trace_nchild_acq(cap_root) == 64u &&
             lj_trace_test_root_entry_successes() > successes);
    }
    assert(lj_trace_flushall_gc(L) == 0);
    assert(!trace_runnable_acq(cap_root, rootno));
  }

  /* Exercise canonical-root scoped teardown with a full depth-three chain
  ** still linked. This runs last so the temporary prototype's hot counters
  ** cannot perturb the independent wide-capacity fixtures above. */
  run(L,
    "root_flush_kernel=assert(loadstring(string.dump(depth_kernel))); "
    "root_flush_oracle=assert(loadstring(string.dump(root_flush_kernel))); "
    "local jit=require('jit'); jit.off(root_flush_oracle,true); "
    "jit.on(root_flush_kernel,true); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=128'); "
    "for k=1,100 do assert(root_flush_kernel(1,1,1,1,48)=="
      "root_flush_oracle(1,1,1,1,48)) end; "
    "for k=1,100 do assert(root_flush_kernel(2,1,1,1,48)=="
      "root_flush_oracle(2,1,1,1,48)) end; "
    "for k=1,100 do assert(root_flush_kernel(2,2,1,1,48)=="
      "root_flush_oracle(2,2,1,1,48)) end; "
    "for k=1,100 do assert(root_flush_kernel(2,2,2,1,48)=="
      "root_flush_oracle(2,2,2,1,48)) end");
  {
    GCtrace *root, *side3, *side2, *side1;
    TraceNo rootno, side3no, side2no, side1no;
    GCproto *pt;
    IRIns edge;
    uint64_t root_generation;
    side3 = find_deepest_side(L, J, "root_flush_kernel", 3u);
    assert(side3 != NULL);
    edge = trace_ir_acq(side3)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    side2 = traceref_safe(J, (TraceNo)edge.op1);
    assert(side2 != NULL);
    edge = trace_ir_acq(side2)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    side1 = traceref_safe(J, (TraceNo)edge.op1);
    assert(side1 != NULL);
    edge = trace_ir_acq(side1)[REF_BASE];
    assert(edge.o == IR_BASE && (TraceNo)edge.op1 != 0);
    rootno = (TraceNo)edge.op1;
    root = traceref_safe(J, rootno);
    assert(root != NULL && trace_nchild_acq(root) == 3u &&
           side_depth(J, side3) == 3u && side_depth(J, side2) == 2u &&
           side_depth(J, side1) == 1u);
    side3no = trace_traceno_acq(side3);
    side2no = trace_traceno_acq(side2);
    side1no = trace_traceno_acq(side1);
    pt = trace_startpt_acq(root);
    root_generation = root->arm64_generation;
    assert(lj_trace_test_arm64_side_graph_summary(J, rootno) ==
           ((3u << 16) | 3u));

    assert(lj_trace_flushscope(J, rootno) == 1u);
    assert(!trace_runnable_acq(root, rootno) &&
           !trace_runnable_acq(side3, side3no) &&
           !trace_runnable_acq(side2, side2no) &&
           !trace_runnable_acq(side1, side1no));
    run(L,
      "jit.off(root_flush_kernel,true); "
      "for k=1,32 do assert(root_flush_kernel(2,2,2,1,48)=="
        "root_flush_oracle(2,2,2,1,48)) end; "
      "collectgarbage('collect'); "
      "jit.on(root_flush_kernel,true)");
    lj_trace_test_root_entry_reset();
    run(L,
      "for k=1,128 do assert(root_flush_kernel(2,2,2,1,48)=="
        "root_flush_oracle(2,2,2,1,48)) end");
    {
      GCtrace *reentry = find_root_for_proto(J, pt);
      assert(reentry != NULL && trace_mcode_acq(reentry) != NULL &&
             reentry->arm64_generation != root_generation &&
             lj_trace_test_root_entry_successes() > 0);
    }
  }
  lua_pop(L, 1);
  lua_close(L);
  puts("ARM64 scalar side adaptation: PASS");
  return 0;
}
#else
int main(void) { return 0; }
#endif
