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
        trace_link_acq(child) != childno ||
        trace_linktype_acq(child) != LJ_TRLINK_LOOP ||
        trace_mcloop_acq(child) == 0 ||
        trace_mcloop_acq(child) >= trace_szmcode_acq(child) ||
        (trace_mcloop_acq(child) & (sizeof(MCode)-1u)) != 0)
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
