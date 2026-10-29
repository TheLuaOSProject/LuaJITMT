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

enum { MAX_WHILE_BODIES = 261 };

static void check_case(lua_State *L, int fractional, unsigned bodies)
{
  jit_State *J = L2J(L);
  GCproto *pt;
  GCtrace *root = NULL, *chain[MAX_WHILE_BODIES] = {NULL};
  TraceNo ids[MAX_WHILE_BODIES] = {0};
  MCode *addresses[MAX_WHILE_BODIES];
  unsigned char *code[MAX_WHILE_BODIES];
  MSize sizes[MAX_WHILE_BODIES], n;
  uint64_t generations[MAX_WHILE_BODIES];
  char source[2048];
  unsigned k;
  assert(bodies >= 4u && bodies <= MAX_WHILE_BODIES);
  int written = snprintf(source, sizeof(source),
    "local jit=require('jit'); jit.off(true,true); jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=1024','maxside=1000','maxrecord=10000','maxirconst=2000','maxsnap=4000'); "
    "local parts={'return function(mode,n) local s,i=%s,1 while i<=n do '}; "
    "for m=1,%u do parts[#parts+1]=(m==1 and 'if ' or 'elseif ').."
    "'mode=='..m..' then s=s+i*'..tostring(m/%u)..' ' end; "
    "parts[#parts+1]='else s=s-i*%s end i=i+1 end return s end'; "
    "kernel=assert(loadstring(table.concat(parts)))(); "
    "oracle=assert(loadstring(string.dump(kernel))); jit.off(oracle,true); "
    "for mode=1,%u do for k=1,%u do "
    "assert(kernel(mode,48)==oracle(mode,48)) end end",
    fractional ? ".5" : "0", bodies-1u, fractional ? 128u : 1u,
    fractional ? "1.25" : "5", bodies, bodies > 81u ? 3u : 80u);
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
  assert(root != NULL && trace_nchild_acq(root) == bodies-1u);
  if (bodies > 81u) {
    int sparse_parent = 0;
    for (n = 0; n < trace_nsnap_acq(root); n++) {
      SnapShot *snap = &trace_snap_acq(root)[n];
      if (snap_topslot_acq(snap) > snap_nslots_acq(snap)) sparse_parent = 1;
    }
    assert(sparse_parent);  /* Frame reservation exceeds this live map. */
  }
  ids[0] = trace_traceno_acq(root);
  chain[0] = root;
  /* The flattened list is newest-first; each IR_BASE names its immediate
  ** parent, independently of that list order. Require every real edge. */
  ids[bodies-1u] = trace_nextside_acq(root);
  for (k = bodies-1u; k != 0; k--) {
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
  for (k = 0; k < bodies; k++) {
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
    uint32_t contract = lj_trace_test_arm64_side_scalar_copy_contract(
      J, ids[bodies-1u]);
    const uint32_t required = 0xf8ff0003u;
    if ((contract & required) != required)
      fprintf(stderr, "while copy contract expected=%08x actual=%08x\n",
              required, contract);
    assert((contract & required) == required);
  }
  assert(lj_trace_test_arm64_side_graph_summary(J, ids[0]) ==
         (((bodies-1u) << 16) | (bodies-1u)));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  written = snprintf(source, sizeof(source),
    "local jit=require('jit'); jit.off(true,true); "
    "for k=1,64 do assert(kernel(%u,48)==oracle(%u,48)) end", bodies,bodies);
  assert(written > 0 && (size_t)written < sizeof(source));
  run(L, source);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_root_entry_publishes() ==
           lj_trace_test_root_entry_successes() &&
         lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_first_exit_parent() == ids[bodies-1u] &&
         lj_trace_test_exit_calls() >= 64u &&
         lj_trace_test_exit_calls() <= 128u);
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (k = 0; k < bodies; k++) {
    GCtrace *T = traceref_safe(J, ids[k]);
    assert(T == chain[k] && trace_runnable_acq(T, ids[k]) &&
           trace_mcode_acq(T) == addresses[k] &&
           trace_szmcode_acq(T) == sizes[k] &&
           la_load64_acq(&T->arm64_generation) == generations[k] &&
           memcmp(code[k], addresses[k], sizes[k]) == 0);
    free(code[k]);
  }
  lj_gc2_smr_read_leave(J2G(J));
  if (bodies > 65u) {
    /* Retire a deep leaf and then a still-linked ancestor while both graphs
    ** exceed the inline index. Descendants retire before their parent; the
    ** preserved prefix keeps its exact code and generations. */
    assert(lj_trace_flushscope(J, ids[bodies-1u]) == 1u);
    assert(lj_trace_test_arm64_side_graph_summary(J, ids[0]) ==
           (((bodies-2u) << 16) | (bodies-2u)));
    assert(lj_trace_flushscope(J, ids[66]) == 1u);
    assert(lj_trace_test_arm64_side_graph_summary(J, ids[0]) ==
           ((65u << 16) | 65u));
    assert(lj_gc2_smr_read_try(J2G(J)));
    for (k = 0; k < bodies; k++) {
      GCtrace *T = traceref_safe(J, ids[k]);
      if (k <= 65u) {
        assert(T == chain[k] && trace_runnable_acq(T, ids[k]) &&
               trace_mcode_acq(T) == addresses[k] &&
               la_load64_acq(&T->arm64_generation) == generations[k]);
      } else {
        assert(!trace_runnable_acq(T, ids[k]));
      }
    }
    lj_gc2_smr_read_leave(J2G(J));
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    run(L, "for k=1,32 do assert(kernel(66,48)==oracle(66,48)) end");
    assert(lj_trace_test_root_entry_successes() > 0 &&
           lj_trace_test_first_exit_parent() == ids[65]);
  }
  run(L, "jit.flush()");
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (k = 0; k < bodies; k++)
    assert(!trace_runnable_acq(traceref_safe(J, ids[k]), ids[k]));
  lj_gc2_smr_read_leave(J2G(J));
  lua_gc(L, LUA_GCCOLLECT, 0);
  run(L, source);
  root = NULL;
  assert(lj_gc2_smr_read_try(J2G(J)));
  for (n = 1; n < trace_sizetrace_acq(J); n++) {
    GCtrace *T = traceref_safe(J, (TraceNo)n);
    if (trace_runnable_acq(T, (TraceNo)n) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_LOOP) {
      assert(root == NULL);
      root = T;
    }
  }
  assert(root && la_load64_acq(&root->arm64_generation) != generations[0]);
  ids[0] = trace_traceno_acq(root);
  lj_gc2_smr_read_leave(J2G(J));
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  run(L, source);
  assert(lj_trace_test_root_entry_successes() > 0 &&
         lj_trace_test_first_exit_parent() == ids[0]);
  printf("nested while %s: depth=%u, copied ancestry negatives, GC/native reentry, unchanged code, full flush/reentry\n",
         fractional ? "fractional" : "integer", bodies-1u);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  run(L, "local t=require('threading'); ready,stop=t.channel(1),t.channel(1); "
         "peer=assert(t.spawn(function(r,s) assert(r:send(true,5)); "
         "assert(select(2,s:recv(600))); return true end,ready,stop)); "
         "assert(select(2,ready:recv(5)))");
  check_case(L, 0, 16u);
  check_case(L, 1, 65u);
  assert(L2J(L)->arm64_side_ancestry == NULL);
  check_case(L, 0, 70u);
  {
    struct LJArm64SideAncestor *storage = L2J(L)->arm64_side_ancestry;
    MSize capacity = L2J(L)->arm64_side_ancestry_size;
    assert(storage && capacity >= 68u);
    check_case(L, 1, 81u);
    assert(L2J(L)->arm64_side_ancestry == storage &&
           L2J(L)->arm64_side_ancestry_size == capacity);
  }
  {
    MSize previous = L2J(L)->arm64_side_ancestry_size;
    check_case(L, 1, 133u);
    assert(L2J(L)->arm64_side_ancestry_size > previous);
    check_case(L, 1, 261u);
    assert(L2J(L)->arm64_side_ancestry_size >= 259u);
  }
  run(L, "assert(stop:send(true,5)); assert(peer:join())");
  lua_close(L);
  puts("nested while side adaptation OK");
  return 0;
}
#else
int main(void) { puts("nested while side adaptation SKIP: ARM64 required"); return 0; }
#endif
