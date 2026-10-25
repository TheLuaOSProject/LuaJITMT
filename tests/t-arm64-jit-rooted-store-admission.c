#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_ircall.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "rooted store admission requires the native ARM64 experimental profile"
#endif

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject why;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &why);
}
static int allocated(Arm64TraceFixture *f)
{
  IRRef nins = 0;
  return lj_asm_arm64_postra_admit(&f->view, &nins);
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef r, call = 0, outer, inner, table, key, status, guard, later;
  SnapNo s, after = 0;
  assert(L && argc == 2);
  luaL_openlibs(L);
  lua_pushboolean(L, 1); lua_setglobal(L, "rooted_store_admission");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    abort();
  }
  arm64_fixture_init(&f, L, "rooted_store_trace_clone");
  assert(semantic(&f) && allocated(&f));
  for (r = REF_FIRST; r < f.semantic_nins; r++) {
    if (f.ir[r].o == IR_CALLS && f.ir[r].op2 == IRCALL_lj_tab_storenum_rooted) {
      call = r; break;
    }
  }
  assert(call);
  outer = f.ir[call].op1; inner = f.ir[outer].op1;
  table = f.ir[inner].op1; key = f.ir[inner].op2;
  guard = call+1;
  status = f.ir[guard].op1 == call ? f.ir[guard].op2 : f.ir[guard].op1;
  later = call+2;
  for (s = 0; s < f.semantic.nsnap; s++) {
    if (f.snaps[s].ref > call &&
        f.snaps[s].ref < f.semantic_nins) { after = s; break; }
  }
  assert(after);
#define REJECT(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; \
  if (semantic(&f) || allocated(&f)) { \
    fprintf(stderr, "store mutation accepted at line %d\n", __LINE__); abort(); \
  } \
} while (0)
#define BOTH(ref, field, value) \
  f.ir[(ref)].field = (value); f.postra[(ref)].field = (value)
  REJECT(BOTH(table, op2, IRTMPREF_IN2));
  REJECT(BOTH(key, op2, IRTMPREF_IN1));
  REJECT(BOTH(inner, op2, table));
  REJECT(BOTH(inner, op1, key));
  REJECT(BOTH(outer, op2, table));
  REJECT(BOTH(outer, op2, REF_NIL));
  REJECT(BOTH(outer, op2, outer));
  REJECT(BOTH(outer, op1, f.nins));
  REJECT(BOTH(call, op2, IRCALL_lj_tab_storetv_existing_forjit));
  REJECT(BOTH(call, op2, IRCALL_lj_tab_storebool_rooted));
  REJECT(BOTH(call, op1, call));
  REJECT(BOTH(guard, o, IR_EQ));
  REJECT(BOTH(status, i, 1));
  REJECT(BOTH(later, o, IR_HSTORE));
  REJECT(f.snaps[after].ref = call);
#undef BOTH
#undef REJECT
  arm64_fixture_reset(&f);
  f.postra[key].r = f.postra[table].r;
  assert(semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  f.postra[table].r = RID_D0;
  assert(semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  lua_close(L);
  puts("rooted store independent semantic/postRA corruption checks OK");
  return 0;
}
