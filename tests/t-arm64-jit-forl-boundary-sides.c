/* Ordinary branch bodies must publish distinct native FORL children. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"
#if LJ_TARGET_ARM64 && defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_trace.h"

static void run(lua_State *L, const char *source)
{
  int status = luaL_dostring(L, source);
  if (status) fprintf(stderr, "FORL boundary: %s\n", lua_tostring(L, -1));
  assert(status == 0);
}

static double call(lua_State *L, const char *name, unsigned mode, int n)
{
  double result;
  lua_getglobal(L, name);
  for (unsigned i = 0; i < 3; i++) lua_pushinteger(L, mode == i+1);
  lua_pushinteger(L, n);
  assert(lua_pcall(L, 4, 1, 0) == 0);
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return result;
}

static void compare(lua_State *L, unsigned mode, int n)
{
  double expected = call(L, "oracle", mode, n);
  assert(call(L, "kernel", mode, n) == expected);
}

#ifdef LJ_TRACE_TEST_HELPERS
static GCtrace *root_for_kernel(lua_State *L)
{
  jit_State *J = L2J(L);
  GCproto *pt;
  lua_getglobal(L, "kernel");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  for (MSize i = 1; i < trace_sizetrace_acq(J); i++) {
    GCtrace *T = traceref_safe(J, (TraceNo)i);
    if (trace_runnable_acq(T, (TraceNo)i) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL)
      return T;
  }
  return NULL;
}

static void check_children(lua_State *L)
{
  jit_State *J = L2J(L);
  GCtrace *root = root_for_kernel(L);
  TraceNo rootno, childno;
  ExitNo exits[3];
  if (!root || trace_nchild_acq(root) != 3) {
    for (MSize t = 1; t < trace_sizetrace_acq(J); t++) {
      GCtrace *T = traceref_safe(J, (TraceNo)t);
      if (trace_runnable_acq(T, (TraceNo)t))
        fprintf(stderr, "trace=%u root=%u nchild=%u head=%u link=%u type=%u op=%u\n",
          (unsigned)t, trace_root_acq(T), trace_nchild_acq(T),
          trace_nextside_acq(T), trace_link_acq(T), trace_linktype_acq(T),
          bc_op(trace_startins_acq(T)));
    }
  }
  assert(root && trace_nchild_acq(root) == 3);
  rootno = trace_traceno_acq(root);
  childno = trace_nextside_acq(root);
  for (unsigned i = 0; i < 3; i++) {
    GCtrace *child = traceref_safe(J, childno);
    IRIns base;
    uint32_t contract, required = 3u | (0x7ffu << 13);
    assert(trace_runnable_acq(child, childno));
    assert(trace_root_acq(child) == rootno);
    assert(trace_startpt_acq(child) == trace_startpt_acq(root));
    assert(trace_linktype_acq(child) == LJ_TRLINK_ROOT);
    assert(trace_link_acq(child) == rootno && trace_mcloop_acq(child) == 0);
    base = trace_ir_acq(child)[REF_BASE];
    assert(base.o == IR_BASE && base.op1 == rootno);
    exits[i] = (ExitNo)base.op2;
    assert(exits[i] < trace_nsnap_acq(root));
    for (unsigned j = 0; j < i; j++) assert(exits[i] != exits[j]);
    assert(trace_exittarget_arm64_acq(root, exits[i]) == trace_mcode_acq(child));
    contract = lj_trace_test_arm64_side_scalar_copy_contract(J, childno);
    if ((contract & required) != required)
      fprintf(stderr, "child %u copy=%#x required=%#x\n", childno, contract, required);
    assert((contract & required) == required);
    childno = trace_nextside_acq(child);
  }
  assert(childno == 0);
}

static void native_runs(lua_State *L, int descending)
{
  jit_State *J = L2J(L);
  GCtrace *root = root_for_kernel(L);
  TraceNo rootno = trace_traceno_acq(root);
  for (unsigned mode = 1; mode <= 3; mode++) {
    lj_trace_test_reset_exit_stats();
    lj_trace_test_root_entry_reset();
    for (unsigned i = 0; i < 40; i++) compare(L, mode, i & 1 ? 48 : descending ? -192 : 256);
    assert(lj_trace_test_root_entry_successes() > 0);
    assert(lj_trace_test_root_entry_publishes() == lj_trace_test_root_entry_successes());
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_exit_calls() > 0 && lj_trace_test_exit_calls() <= 80);
    /* The loop-completion exit comes from a child, ruling out an interpreted
    ** branch body reached by repeatedly leaving the parent each iteration. */
    {
      TraceNo first = lj_trace_test_first_exit_parent();
      TraceNo last = lj_trace_test_last_exit_parent();
      assert(first != rootno && last != rootno);
      assert(trace_root_acq(traceref_safe(J, first)) == rootno);
      assert(trace_root_acq(traceref_safe(J, last)) == rootno);
    }
  }
}
#endif

static void exercise(lua_State *L, const char *range, int descending)
{
  char source[2048];
  snprintf(source, sizeof(source),
    "local src='return function(x,y,z,n) local sum=.5 for i=%s do "
    "if x==1 then sum=sum+i*.25 end "
    "if y==1 then sum=sum-i*.5 end "
    "if z==1 then sum=sum+i*.75 end end return sum end'; "
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1','maxtrace=128'); "
    "kernel=assert(loadstring(src))(); oracle=assert(loadstring(src))(); "
    "jit.off(oracle,true); jit.on()", range);
  run(L, source);
  for (unsigned generation = 0; generation < 2; generation++) {
    fprintf(stderr, "FORL boundary range=%s generation=%u\n", range, generation);
    if (generation) run(L, "jit.flush()");
    for (unsigned mode = 0; mode <= 3; mode++)
      for (unsigned i = 0; i < 8; i++) compare(L, mode, descending ? 1 : 64);
#ifdef LJ_TRACE_TEST_HELPERS
    check_children(L);
    native_runs(L, descending);
#endif
    run(L, "collectgarbage('collect')");
#ifdef LJ_TRACE_TEST_HELPERS
    check_children(L);
    native_runs(L, descending);
#endif
    for (unsigned mode = 0; mode <= 3; mode++) {
      compare(L, mode, 0);
      compare(L, mode, 1);
      compare(L, mode, 7);
    }
    run(L, "local ok=pcall(kernel,0,0,0,true); assert(not ok)");
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L); luaL_openlibs(L);
  run(L, "jit=require('jit'); local th=require('threading'); "
      "ready=th.channel(1); stop=th.channel(1); "
      "peer=assert(th.spawn(function(r,s) assert(r:send(true,5)); "
      "assert(select(2,s:recv(120))); return true end,ready,stop)); "
      "assert(select(2,ready:recv(5)))");
  exercise(L, "1,n", 0);
  exercise(L, "64,n,-1", 1);
  exercise(L, "1,n,2", 0);
  run(L, "assert(stop:send(true,5)); local ok,value=peer:join(5); assert(ok and value)");
  lua_close(L);
  puts("ARM64 FORL boundary siblings: PASS");
  return 0;
}
#else
int main(void) { return 0; }
#endif
