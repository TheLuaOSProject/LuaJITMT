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
    const uint32_t ancestry_mutations = 0xf80f0000u;
    if ((contract & ancestry_mutations) != ancestry_mutations)
      fprintf(stderr, "ancestry mutations expected=%08x observed=%08x\n",
              ancestry_mutations, contract);
    assert((contract & ancestry_mutations) == ancestry_mutations); }
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
	run(L, "for k=1,64 do assert(depth_kernel(2,2,2,1,48)==-146.5) end");
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_exit_calls() > 0);

  /* Retain the historical wide direct-child path beyond the 64-entry deep
  ** ancestry index. The real root owns 65 separately selected cold exits;
  ** the summary helper validates its topology and retirement eligibility. */
  run(L,
    "local jit=require('jit'); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=512',"
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
  lua_close(L);
  puts("ARM64 scalar side adaptation: PASS");
  return 0;
}
#else
int main(void) { return 0; }
#endif
