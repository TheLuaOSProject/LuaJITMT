/* Nested BC_LOOP sides retain the same ancestry and snapshot proofs as FORL. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    fprintf(stderr, "nested while: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static void check_case(lua_State *L, int fractional)
{
  jit_State *J = L2J(L);
  GCproto *pt;
  GCtrace *root = NULL, *chain[4] = {NULL};
  TraceNo ids[4] = {0};
  MCode *addresses[4];
  unsigned char *code[4];
  MSize sizes[4], n;
  uint64_t generations[4];
  char source[2048];
  unsigned k;
  int written = snprintf(source, sizeof(source),
    "local jit=require('jit'); jit.off(true,true); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=128'); "
    "kernel=assert(loadstring('return function(mode,n) local s,i=%s,1 "
    "while i<=n do if mode==1 then s=s+%s "
    "elseif mode==2 then s=s-%s elseif mode==3 then s=s+%s "
    "else s=s-%s end i=i+1 end return s end'))(); "
    "oracle=assert(loadstring(string.dump(kernel))); jit.off(oracle,true); "
    "for mode=1,4 do for k=1,80 do "
    "assert(kernel(mode,48)==oracle(mode,48)) end end",
    fractional ? ".5" : "0", fractional ? "i*.25" : "i",
    fractional ? "i*.5" : "i", fractional ? "i*.75" : "i*3",
    fractional ? "i*1.25" : "i*5");
  assert(written > 0 && (size_t)written < sizeof(source));
  run(L, source);
  lua_getglobal(L, "kernel");
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *T = traceref_safe(J, (TraceNo)n);
    if (trace_runnable_acq(T, (TraceNo)n) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_LOOP) {
      assert(root == NULL);
      root = T;
    }
  }
  assert(root != NULL && trace_nchild_acq(root) == 3u);
  ids[0] = trace_traceno_acq(root);
  chain[0] = root;
  /* The flattened list is newest-first; each IR_BASE names its immediate
  ** parent, independently of that list order. Require all three real edges. */
  ids[3] = trace_nextside_acq(root);
  for (k = 3; k != 0; k--) {
    IRIns base;
    chain[k] = traceref_safe(J, ids[k]);
    assert(trace_runnable_acq(chain[k], ids[k]));
    assert(trace_root_acq(chain[k]) == ids[0] &&
           trace_link_acq(chain[k]) == ids[0] &&
           trace_linktype_acq(chain[k]) == LJ_TRLINK_ROOT);
    base = trace_ir_acq(chain[k])[REF_BASE];
    assert(base.o == IR_BASE && base.op1 != 0 && base.op1 != ids[k]);
    if (k == 1) assert((TraceNo)base.op1 == ids[0]);
    else ids[k-1] = (TraceNo)base.op1;
    {
      GCtrace *parent = traceref_safe(J, (TraceNo)base.op1);
      assert(trace_runnable_acq(parent, (TraceNo)base.op1));
      assert(base.op2 < trace_nsnap_acq(parent) &&
             trace_exittarget_arm64_acq(parent, (ExitNo)base.op2) ==
               trace_mcode_acq(chain[k]));
    }
  }
  for (k = 0; k < 4; k++) {
    addresses[k] = trace_mcode_acq(chain[k]);
    sizes[k] = trace_szmcode_acq(chain[k]);
    generations[k] = la_load64_acq(&chain[k]->arm64_generation);
    assert(addresses[k] && sizes[k] && generations[k]);
    code[k] = (unsigned char *)malloc(sizes[k]);
    assert(code[k]);
    memcpy(code[k], addresses[k], sizes[k]);
  }
  lj_gc2_smr_read_leave(J2G(J));
  {
    uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(J, ids[3]);
    const uint32_t required = 0xf80f0003u;
    if ((contract & required) != required)
      fprintf(stderr, "while copy contract expected=%08x actual=%08x\n",
              required, contract);
    assert((contract & required) == required);
  }
  assert(lj_trace_test_arm64_side_graph_summary(J, ids[0]) ==
         ((3u << 16) | 3u));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  run(L, "local jit=require('jit'); jit.off(true,true); "
         "for k=1,64 do assert(kernel(4,48)==oracle(4,48)) end");
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_root_entry_publishes() ==
           lj_trace_test_root_entry_successes() &&
         lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_first_exit_parent() == ids[3] &&
         lj_trace_test_exit_calls() >= 64u &&
         lj_trace_test_exit_calls() <= 128u);
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (k = 0; k < 4; k++) {
    GCtrace *T = traceref_safe(J, ids[k]);
    assert(T == chain[k] && trace_runnable_acq(T, ids[k]) &&
           trace_mcode_acq(T) == addresses[k] &&
           trace_szmcode_acq(T) == sizes[k] &&
           la_load64_acq(&T->arm64_generation) == generations[k] &&
           memcmp(code[k], addresses[k], sizes[k]) == 0);
    free(code[k]);
  }
  lj_gc2_smr_read_leave(J2G(J));
  printf("nested while %s: depth=3, copied ancestry negatives, GC/native reentry, unchanged code\n",
         fractional ? "fractional" : "integer");
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  run(L, "local t=require('threading'); ready,stop=t.channel(1),t.channel(1); "
         "peer=assert(t.spawn(function(r,s) assert(r:send(true,5)); "
         "assert(select(2,s:recv(120))); return true end,ready,stop)); "
         "assert(select(2,ready:recv(5)))");
  check_case(L, 0);
  check_case(L, 1);
  run(L, "assert(stop:send(true,5)); assert(peer:join())");
  lua_close(L);
  puts("nested while side adaptation OK");
  return 0;
}
#else
int main(void) { puts("nested while side adaptation SKIP: ARM64 required"); return 0; }
#endif
