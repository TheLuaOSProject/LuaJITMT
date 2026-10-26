/* Independent NUM fusion rejection across the assembler loop boundary. */
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_trace.h"
#include "lj_target.h"
#include "lib/arm64_trace_fixture.h"

static int is_num_addsub(const IRIns *ir)
{
  return irt_isnum(ir->t) && (ir->o == IR_ADD || ir->o == IR_SUB);
}

static int uses(IRIns ir, IRRef ref)
{
  return ir.op1 == ref || ir.op2 == ref;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCtrace *T = NULL;
  TraceNo no;
  Arm64TraceFixture f;
  IRRef loop = 0, premul = 0, preuse = 0, bodymul = 0, bodyuse = 0;
  int mutated_sem, mutated_post;
  assert(L);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','fma'); "
    "local f=function(n,a,b) local i,x=0,.75 "
      "while i<n do x=x*a+b; i=i+1 end return x end; "
    "_G.fma_boundary_original=f; "
    "_G.fma_boundary_clone=assert(loadstring(string.dump(f))); "
    "local x=f(20,.5,.25); assert(x==.5+2^-22); assert(f(20,.5,.25)==x)" ) == 0);
  J = L2J(L);
  for (no = 1; no < trace_sizetrace_acq(J); no++) {
    GCtrace *candidate = traceref_safe(J, no);
    if (candidate && trace_runnable_acq(candidate, no) &&
        bc_op(trace_startins_acq(candidate)) == BC_LOOP) {
      T = candidate;
      break;
    }
  }
  assert(T && "expected one runnable numeric LOOP trace");
  arm64_fixture_init(&f, L, "fma_boundary_clone");
  assert(lj_asm_arm64_ir_admit(f.J, &f.semantic, NULL));
  assert(lj_asm_arm64_postra_admit(&f.view, NULL));

  for (IRRef r = REF_FIRST; r < f.nins; r++) {
    IRIns ir = f.good[r];
    if (ir.o == IR_LOOP) { loop = r; break; }
  }
  assert(loop);
  /* Require an unlocated pre-loop MULNUM with an existing pre-loop add/sub
  ** consumer. Do not synthesize this candidate or silently skip the probe. */
  for (IRRef r = REF_FIRST; r < loop && !premul; r++) {
    IRIns mul = f.good[r];
    if (mul.o != IR_MUL || mul.t.irt != IRT_NUM ||
        !ra_noreg(mul.r) || ra_hasspill(mul.s)) continue;
    for (IRRef u = r + 1; u < loop; u++) {
      if (is_num_addsub(&f.good[u]) && uses(f.good[u], r)) {
        premul = r; preuse = u; break;
      }
    }
  }
  /* Find a body MULNUM consumed by a body add/sub; this preserves all IR
  ** types and allocation fields when only the operand edge is redirected. */
  for (IRRef u = loop + 1; u < f.nins && !bodyuse; u++) {
    IRIns add = f.good[u];
    if (!is_num_addsub(&add)) continue;
    for (IRRef m = loop + 1; m < u; m++) {
      if (f.good[m].o == IR_MUL && f.good[m].t.irt == IRT_NUM &&
          ra_noreg(f.good[m].r) && !ra_hasspill(f.good[m].s) && uses(add, m)) {
        bodymul = m; bodyuse = u; break;
      }
    }
  }
  assert(premul && preuse && bodymul && bodyuse &&
         "authentic trace lacks required preloop/body fusion geometry");
  assert(preuse < loop && loop < bodymul && bodymul < bodyuse);
  assert(!ra_hasreg(f.good[premul].r) && !ra_hasspill(f.good[premul].s));
  assert(f.good[bodyuse].op1 == bodymul || f.good[bodyuse].op2 == bodymul);
  printf("candidate loop=%u pre-MUL=%u pre-use=%u body-MUL=%u body-use=%u\n",
         loop, premul, preuse, bodymul, bodyuse);

  {
    const MCode *mc = trace_mcode_acq(f.owner);
    unsigned fused = 0;
    for (MSize i = 0; i < f.owner->szmcode / sizeof(MCode); i++)
      if ((mc[i] & 0xffe08000u) == A64I_FMADDd) fused++;
    assert(fused >= 2);
  }

  /* Redirect exactly one body consumer to the absent-location preloop MUL. */
  if (f.ir[bodyuse].op1 == bodymul) f.ir[bodyuse].op1 = premul;
  else { assert(f.ir[bodyuse].op2 == bodymul); f.ir[bodyuse].op2 = premul; }
  if (f.postra[bodyuse].op1 == bodymul) f.postra[bodyuse].op1 = premul;
  else { assert(f.postra[bodyuse].op2 == bodymul); f.postra[bodyuse].op2 = premul; }
  mutated_sem = lj_asm_arm64_ir_admit(f.J, &f.semantic, NULL);
  mutated_post = lj_asm_arm64_postra_admit(&f.view, NULL);
  printf("mutated clone: semantic=%s postRA=%s\n",
         mutated_sem ? "accept" : "reject",
         mutated_post ? "accept" : "reject");

  assert(mutated_sem && !mutated_post);

  arm64_fixture_reset(&f);
  assert(lj_asm_arm64_ir_admit(f.J, &f.semantic, NULL));
  assert(lj_asm_arm64_postra_admit(&f.view, NULL));
  arm64_fixture_free(&f);
  lua_close(L);
  puts("t-arm64-jit-fusion-boundary OK: cross-loop NUM fusion rejected; original trace preserved");
  return 0;
}

#else
#include <stdio.h>
int main(void)
{
  puts("t-arm64-jit-fusion-boundary SKIP: requires experimental Apple ARM64 JIT");
  return 0;
}
#endif
