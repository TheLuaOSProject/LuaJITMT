/* Independent admission mutations for scalar results using the
** generic rooted getter with a GC identity key. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_ircall.h"
#include "lj_target.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "rooted scalar-key admission requires the experimental ARM64 profile"
#endif

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &reject);
}

static int allocated(Arm64TraceFixture *f)
{
  IRRef nins = 0;
  return lj_asm_arm64_postra_admit(&f->view, &nins);
}

static IRRef find_call(const IRIns *ir, IRRef n, IRCallID id)
{
  IRRef r;
  for (r = REF_FIRST; r < n; r++)
    if (ir[r].o == IR_CALLS && ir[r].op2 == id) return r;
  assert(!"missing rooted scalar-key helper call");
  return 0;
}

static SnapNo find_snapshot(const Arm64TraceFixture *f, IRRef ref)
{
  SnapNo s;
  for (s = 0; s < f->semantic.nsnap; s++)
    if (f->snaps[s].ref == ref) return s;
  assert(!"missing rooted scalar-key result snapshot");
  return (SnapNo)~0u;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef read, result, head, tail, table, key, guardconst;
  SnapNo result_snap;
  assert(L && argc == 2);
  luaL_openlibs(L);
  lua_pushboolean(L, 1); lua_setglobal(L, "rooted_scalar_keys_admission");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "rooted scalar-key admission fixture: %s\n", lua_tostring(L, -1));
    abort();
  }

  arm64_fixture_init(&f, L, "rooted_scalar_key_trace_clone");
  assert(semantic(&f) && allocated(&f));
  read = find_call(f.ir, f.semantic_nins,
                   IRCALL_lj_tab_gettv_rooted_hit_try);
  result = read + 2;
  assert(f.ir[result].o == IR_VLOAD && irt_type(f.ir[result].t) == IRT_NUM);
  tail = f.ir[read].op1;
  assert(f.ir[tail].o == IR_CARG);
  head = f.ir[tail].op1;
  assert(f.ir[head].o == IR_CARG);
  table = f.ir[head].op1;
  key = f.ir[head].op2;
  assert(f.ir[table].o == IR_TMPREF &&
         f.ir[table].op2 == (IRTMPREF_IN1|IRTMPREF_OUT1));
  assert(f.ir[key].o == IR_TMPREF && f.ir[key].op2 == IRTMPREF_IN2);
  assert(irt_type(f.ir[f.ir[key].op1].t) == IRT_TAB);
  result_snap = find_snapshot(&f, result);

  guardconst = f.ir[read+1].op1 == read ? f.ir[read+1].op2 : f.ir[read+1].op1;
  assert(irref_isk(guardconst) && f.ir[guardconst].o == IR_KINT &&
         f.ir[guardconst].i == 0);

#define RESET() do { arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); } while (0)
#define SEM_REJECT(change) do { RESET(); change; assert(!semantic(&f)); assert(allocated(&f)); } while (0)
#define POST_REJECT(change) do { RESET(); change; assert(semantic(&f)); assert(!allocated(&f)); } while (0)

  /* This wrong helper must not certify a GC identity key. */
  SEM_REJECT(f.ir[read].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  POST_REJECT(f.postra[read].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  SEM_REJECT(f.ir[table].op2 = IRTMPREF_IN2);
  POST_REJECT(f.postra[table].op2 = IRTMPREF_IN2);
  SEM_REJECT(f.ir[key].op2 = IRTMPREF_IN1);
  POST_REJECT(f.postra[key].op2 = IRTMPREF_IN1);
  SEM_REJECT(f.ir[head].op1 = key);
  POST_REJECT(f.postra[head].op1 = key);
  SEM_REJECT(f.ir[head].op2 = table);
  POST_REJECT(f.postra[head].op2 = table);
  SEM_REJECT(f.ir[read+1].o = IR_EQ);
  POST_REJECT(f.postra[read+1].o = IR_EQ);
  SEM_REJECT(f.ir[guardconst].i = 1);
  POST_REJECT(f.postra[guardconst].i = 1);
  SEM_REJECT(f.ir[result].t.irt = IRT_TAB|IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_TAB|IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt = IRT_LIGHTUD|IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_LIGHTUD|IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt = IRT_PTR|IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_PTR|IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt = IRT_PROTO|IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_PROTO|IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt &= (IRType)~IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt &= (IRType)~IRT_GUARD);

  /* Semantic and post-RA validators share snapshot storage in this fixture;
  ** mutate and run each pass independently. */
  RESET(); f.snaps[result_snap].ref = REF_DROP;
  assert(!semantic(&f));
  RESET(); f.snaps[result_snap].ref = REF_DROP;
  assert(!allocated(&f));
  RESET();
  assert(semantic(&f) && allocated(&f));
#undef POST_REJECT
#undef SEM_REJECT
#undef RESET

  printf("rooted scalar-key admission: read=%u result=%u snapshot=%u\n",
         (unsigned)read, (unsigned)result, (unsigned)result_snap);
  arm64_fixture_free(&f);
  lua_close(L);
  puts("t-arm64-jit-rooted-scalar-keys-admission OK");
  return 0;
}
