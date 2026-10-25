/*
** Native ARM64 proof for the first generalized scalar side-loop topology.
** The side is recorded after an INT root sees a NUM accumulator, then closes
** naturally at its own loop label. Repeated parent exits must target its body
** directly: no interpreter exit is recorded while the fractional calls run.
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

static GCtrace *find_side(jit_State *J, GCtrace **parentp, TraceNo *parentno)
{
  MSize n;
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *p = traceref_safe(J, (TraceNo)n);
    TraceNo childno;
    GCtrace *child;
    IRIns base;
    if (!trace_runnable_acq(p, (TraceNo)n) ||
        trace_root_acq(p) != 0 || trace_nchild_acq(p) != 1)
      continue;
    childno = trace_nextside_acq(p);
    child = traceref_safe(J, childno);
    if (!trace_runnable_acq(child, childno) ||
        trace_root_acq(child) != (TraceNo)n ||
        ((trace_linktype_acq(child) == LJ_TRLINK_LOOP &&
          (trace_link_acq(child) != childno ||
           trace_mcloop_acq(child) == 0 ||
           trace_mcloop_acq(child) >= trace_szmcode_acq(child) ||
           (trace_mcloop_acq(child) & (sizeof(MCode)-1u)) != 0)) ||
         (trace_linktype_acq(child) == LJ_TRLINK_ROOT &&
          (trace_link_acq(child) != (TraceNo)n || trace_mcloop_acq(child) != 0)) ||
         (trace_linktype_acq(child) != LJ_TRLINK_LOOP &&
          trace_linktype_acq(child) != LJ_TRLINK_ROOT)))
      continue;
    base = trace_ir_acq(child)[REF_BASE];
    if (base.o != IR_BASE || (TraceNo)base.op1 != (TraceNo)n)
      continue;
    *parentp = p;
    *parentno = (TraceNo)n;
    return child;
  }
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCtrace *parent, *child;
  TraceNo parentno, childno;
  IRIns base;
  ExitNo exitno;
  uint32_t entries;
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
  child = find_side(J, &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_nextside_acq(parent) == childno);
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
  entries = lj_trace_test_root_entry_startins_calls();
  assert(entries > 0);
  /* If the patched exit edge returned to the interpreter, lj_trace_exit would
  ** be counted. The loop results and entry activity above are from the warmed
  ** native dispatch path. */
  assert(lj_trace_test_exit_calls() == 0);

  /* Short and long body counts must preserve the same scalar result. A full
  ** collection retains the globally rooted function/proto and published JIT
  ** generations; the adapted native path remains usable afterward. */
  run(L, "assert(kernel(8,1.25)==10); assert(kernel(1024,1.25)==1280); "
         "collectgarbage('collect')");
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(kernel(64,1.25)==80) end");
  assert(lj_trace_test_root_entry_startins_calls() > 0);
  assert(lj_trace_test_exit_calls() == 0);

  /* Overflow must retain LuaJIT numeric semantics across the loop boundary. */
  run(L, "overflow_kernel=function(n) local i,sum=1,2147483600 "
         "while i<=n do sum=sum+1; i=i+1 end return sum end; "
         "assert(overflow_kernel(128)==2147483728)");

  /* A full trace flush must retire both sides cleanly; the same INT-to-NUM
  ** transition can publish and enter a fresh child generation afterward. */
  run(L, "local jit=require('jit'); jit.flush(); "
         "for i=1,8 do assert(kernel(64,1)==64) end; "
         "for i=1,8 do assert(kernel(64,1.25)==80) end");
  child = find_side(J, &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_nextside_acq(parent) == childno);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    assert(contract == 0x1fffu); }
  base = trace_ir_acq(child)[REF_BASE];
  exitno = (ExitNo)base.op2;
  assert(exitno < trace_nsnap_acq(parent));
  assert(trace_exittarget_arm64_acq(parent, exitno) == trace_mcode_acq(child));
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(kernel(64,1.25)==80) end");
  assert(lj_trace_test_root_entry_startins_calls() > 0);
  assert(lj_trace_test_exit_calls() == 0);

  /* An overflow exit mid-loop carries parent state into a side trace. */
  run(L, "local jit=require('jit'); jit.flush(); "
         "overflow_kernel=function(n,step,sum) local i=1 "
         "while i<=n do sum=sum+step; i=i+1 end return sum end; "
         "for i=1,8 do assert(overflow_kernel(64,1,0)==64) end; "
         "for i=1,8 do assert(overflow_kernel(64,1,2147483600)==2147483664) end");
  child = find_side(J, &parent, &parentno);
  assert(child != NULL);
  childno = trace_traceno_acq(child);
  assert(trace_nextside_acq(parent) == childno);
  { uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    if (contract != 0x1fffu) fprintf(stderr, "overflow copied contract=%#x\n", contract);
    assert(contract == 0x1fffu); }
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_reset();
  run(L, "for i=1,100 do assert(overflow_kernel(64,1,2147483600)==2147483664) end");
  assert(lj_trace_test_root_entry_startins_calls() > 0);
  assert(lj_trace_test_exit_calls() == 0);

  /* A later type mismatch must leave the immutable child generation cleanly
  ** and preserve ordinary Lua arithmetic errors. */
  run(L, "local ok,err=pcall(kernel,64,true); "
         "assert(not ok and string.find(err,'arithmetic'))");
  lua_close(L);
  puts("ARM64 scalar side adaptation: PASS");
  return 0;
}
#else
int main(void) { return 0; }
#endif
