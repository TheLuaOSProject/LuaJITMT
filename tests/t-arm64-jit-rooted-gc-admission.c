/* Independent semantic and allocated-layout certificates for GC-valued
** rooted reads/stores, including a live GC spill across repeated helpers. */
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
#error "rooted GC admission requires the experimental ARM64 profile"
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
  assert(!"missing rooted GC helper call");
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef read, store, value, readhead, readtail, table, key;
  IRRef storehead, storetail, storetable, storekey, storevalue;
  IRRef r, spilled = 0;
  SnapNo result_snap = (SnapNo)~0u, s;
  unsigned spills = 0, stores = 0;
  assert(L && argc == 2);
  luaL_openlibs(L);
  lua_pushboolean(L, 1); lua_setglobal(L, "rooted_gc_admission");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "rooted GC admission fixture: %s\n", lua_tostring(L, -1));
    abort();
  }

  arm64_fixture_init(&f, L, "rooted_gc_trace_clone");
  assert(semantic(&f) && allocated(&f));
  read = find_call(f.ir, f.semantic_nins,
                   IRCALL_lj_tab_gettv_rooted_hit_try);
  store = find_call(f.ir, f.semantic_nins, IRCALL_lj_tab_storegc_rooted);
  value = read + 2;
  assert(f.ir[value].o == IR_VLOAD &&
         (irt_type(f.ir[value].t) == IRT_TAB ||
          irt_type(f.ir[value].t) == IRT_STR));

  readtail = f.ir[read].op1;
  assert(f.ir[readtail].o == IR_CARG);
  readhead = f.ir[readtail].op1;
  assert(f.ir[readhead].o == IR_CARG);
  table = f.ir[readhead].op1;
  key = f.ir[readhead].op2;

  storetail = f.ir[store].op1;
  assert(f.ir[storetail].o == IR_CARG);
  storehead = f.ir[storetail].op1;
  assert(f.ir[storehead].o == IR_CARG);
  storetable = f.ir[storehead].op1;
  storekey = f.ir[storehead].op2;
  storevalue = f.ir[storetail].op2;
  assert(f.ir[storevalue].o == IR_TMPREF &&
         f.ir[storevalue].op2 == IRTMPREF_IN3);
  for (r = REF_FIRST; r < f.semantic_nins; r++)
    if (f.ir[r].o == IR_CALLS &&
        f.ir[r].op2 == IRCALL_lj_tab_storegc_rooted)
      stores++;
  assert(stores > 0);
  for (s = 0; s < f.semantic.nsnap; s++)
    if (f.snaps[s].ref == value) result_snap = s;
  assert(result_snap != (SnapNo)~0u);

  /* The source value remains live across the scratch-clobbering scalar read
  ** and repeated GC stores. Require a real allocated full-width GC spill. */
  for (r = f.owner->nk; r < f.nins; r++) {
    IRIns p = f.postra[r];
    IRType type = irt_type(p.t);
    if ((p.o == IR_SLOAD || p.o == IR_VLOAD || p.o == IR_PHI) &&
        (type == IRT_TAB || type == IRT_STR || type == IRT_FUNC ||
         type == IRT_THREAD || type == IRT_UDATA || type == IRT_CDATA) &&
        ra_hasspill(p.s)) {
      spills++;
      if (!spilled) spilled = r;
    }
  }
  assert(spills > 0 && spilled);

#define RESET() do { arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); } while (0)
#define SEM_REJECT(change) do { \
  RESET(); change; \
  assert(!semantic(&f)); assert(allocated(&f)); \
} while (0)
#define POST_REJECT(change) do { \
  RESET(); change; \
  assert(semantic(&f)); assert(!allocated(&f)); \
} while (0)

  SEM_REJECT(f.ir[read].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  POST_REJECT(f.postra[read].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  SEM_REJECT(f.ir[table].op2 = IRTMPREF_IN2);
  POST_REJECT(f.postra[table].op2 = IRTMPREF_IN2);
  SEM_REJECT(f.ir[key].op2 = IRTMPREF_IN1);
  POST_REJECT(f.postra[key].op2 = IRTMPREF_IN1);
  SEM_REJECT(f.ir[storehead].op1 = storekey);
  POST_REJECT(f.postra[storehead].op1 = storekey);
  SEM_REJECT(f.ir[storehead].op2 = storetable);
  POST_REJECT(f.postra[storehead].op2 = storetable);
  SEM_REJECT(f.ir[storetable].op2 = IRTMPREF_IN3);
  POST_REJECT(f.postra[storetable].op2 = IRTMPREF_IN3);
  SEM_REJECT(f.ir[storekey].op2 = IRTMPREF_IN3);
  POST_REJECT(f.postra[storekey].op2 = IRTMPREF_IN3);
  SEM_REJECT(f.ir[store].op2 = IRCALL_lj_tab_storenum_rooted);
  POST_REJECT(f.postra[store].op2 = IRCALL_lj_tab_storenum_rooted);
  SEM_REJECT(f.ir[storevalue].op2 = IRTMPREF_IN2);
  POST_REJECT(f.postra[storevalue].op2 = IRTMPREF_IN2);
  SEM_REJECT(f.ir[storevalue].op2 = IRTMPREF_IN1);
  POST_REJECT(f.postra[storevalue].op2 = IRTMPREF_IN1);
  SEM_REJECT(f.ir[storevalue].op2 = IRTMPREF_IN3|IRTMPREF_OUT1);
  POST_REJECT(f.postra[storevalue].op2 = IRTMPREF_IN3|IRTMPREF_OUT1);
  SEM_REJECT(f.ir[storevalue].op1 = storevalue);
  POST_REJECT(f.postra[storevalue].op1 = storevalue);
  SEM_REJECT(f.ir[store].op1 = store);
  POST_REJECT(f.postra[store].op1 = store);
  SEM_REJECT(f.ir[read+1].o = IR_EQ);
  POST_REJECT(f.postra[read+1].o = IR_EQ);
  SEM_REJECT(f.ir[store+1].o = IR_EQ);
  POST_REJECT(f.postra[store+1].o = IR_EQ);
  SEM_REJECT(f.ir[store].t.irt = IRT_INT);
  POST_REJECT(f.postra[store].t.irt = IRT_INT);
  SEM_REJECT(f.ir[value].t.irt = IRT_NUM|IRT_GUARD);
  POST_REJECT(f.postra[value].t.irt = IRT_NUM|IRT_GUARD);
  SEM_REJECT(f.ir[value].t.irt = IRT_PTR|IRT_GUARD);
  POST_REJECT(f.postra[value].t.irt = IRT_PTR|IRT_GUARD);
  SEM_REJECT(f.ir[value].t.irt = IRT_PROTO|IRT_GUARD);
  POST_REJECT(f.postra[value].t.irt = IRT_PROTO|IRT_GUARD);
  SEM_REJECT(f.ir[value].t.irt &= (IRType)~IRT_GUARD);
  POST_REJECT(f.postra[value].t.irt &= (IRType)~IRT_GUARD);
  /* The semantic and post-RA views share snapshot storage. Run each pass
  ** independently on the same malformed result publication. */
  RESET(); f.snaps[result_snap].ref = REF_DROP;
  assert(!semantic(&f));
  RESET(); f.snaps[result_snap].ref = REF_DROP;
  assert(!allocated(&f));

  /* A GC carrier occupies its complete aligned spill span. */
  POST_REJECT(f.postra[spilled].s = f.postra[spilled].s + 1);
  /* A properly aligned 32-bit status cannot own the high half of that span. */
  POST_REJECT(f.postra[read].s = f.postra[spilled].s + 1);
  POST_REJECT(f.postra[storevalue].r = f.postra[storetable].r);
  POST_REJECT(f.postra[storevalue].r = f.postra[storekey].r);
  POST_REJECT(f.postra[storevalue].r = RID_D0);
  RESET();
  assert(semantic(&f) && allocated(&f));
#undef POST_REJECT
#undef SEM_REJECT
#undef RESET

  printf("rooted GC admission: read=%u stores=%u GC spills=%u\n",
         (unsigned)read, stores, spills);
  arm64_fixture_free(&f);
  lua_close(L);
  puts("t-arm64-jit-rooted-gc-admission OK");
  return 0;
}
