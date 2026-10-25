#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_tab.h"
#include "lj_target.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || !defined(LJ_TAB_TEST_HELPERS)
#error "TNEW admission requires experimental native ARM64 and table helpers"
#endif

static lua_State *test_L;
static unsigned native_constructors;
static unsigned capacity;

static void constructor(lua_State *L, GCtab *t)
{
  TValue *array;
  Node *node;
  MSize size, mask, i;
  GCArena *arena;
  if (L != test_L || !lj_tg_load_jit_base(L2TG(L))) return;
  native_constructors++;
  arena = lj_arena_of(t);
  assert(!lj_arena_ishuge(arena));
  assert(lj_arena_ready_get(arena, lj_arena_cellof(t)));
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) != 0);
  size = lj_tab_array_snapshot_acq(t, &array);
  node = lj_tab_node_snapshot_acq(t, &mask);
  assert(size == (capacity & 0x7ffu));
  assert(mask == ((capacity >> 11) ? (1u << (capacity >> 11))-1u : 0u));
  for (i = 0; i < size; i++) assert(lj_tv_isnil_acq(&array[i]));
  if (mask) {
    for (i = 0; i <= mask; i++) {
      assert(lj_tv_isnil_acq(&node[i].key));
      assert(lj_tv_isnil_acq(&node[i].val));
    }
  }
}

/* Change only a private, uncompiled constructor's capacity hint. Its Lua
** result is still an empty table. This exercises both native helper branches
** and nonempty array/hash storage without admitting raw initialization stores. */
static int set_capacity(lua_State *L)
{
  GCproto *pt;
  BCIns *bc;
  MSize i;
  unsigned seen = 0;
  assert(lua_isfunction(L, 1) && isluafunc(funcV(L->base)));
  pt = funcproto(funcV(L->base));
  bc = proto_bc(pt);
  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((uint32_t *)&bc[i]);
    if (bc_op(ins) == BC_TNEW) {
      assert(bc_d(ins) == 0);
      setbc_d(&ins, capacity);
      la_store32_rel((uint32_t *)&bc[i], ins);
      seen++;
    }
  }
  assert(seen);
  return 0;
}

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject why;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &why);
}
static int allocated(Arm64TraceFixture *f)
{
  IRRef end;
  return lj_asm_arm64_postra_admit(&f->view, &end);
}
int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef r, alloc = 0, phi = 0;
  uint32_t anchors;
  assert(L && argc == 3);
  capacity = (unsigned)strtoul(argv[2], NULL, 0);
  assert(capacity == 0 || capacity == 4 || capacity == (65u|(4u<<11)));
  luaL_openlibs(L);
  test_L = L;
  anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  lua_pushboolean(L, 1); lua_setglobal(L, "tnew_admission");
  lua_pushcfunction(L, set_capacity); lua_setglobal(L, "tnew_set_capacity");
  lj_tab_test_set_constructor_prepublish_hook(constructor);
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "%s\n", lua_tostring(L,-1)); abort();
  }
  lj_tab_test_set_constructor_prepublish_hook(NULL);
  assert(native_constructors >= 640u);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  arm64_fixture_init(&f, L, "tnew_trace_clone");
  assert(semantic(&f) && allocated(&f));
  for (r = REF_FIRST; r < f.semantic_nins; r++) {
    if (f.ir[r].o == IR_TNEW && !alloc) alloc = r;
    if (f.ir[r].o == IR_PHI && irt_istab(f.ir[r].t)) phi = r;
  }
  assert(alloc && phi);
#define BOTH(r,field,value) f.ir[r].field=(value); f.postra[r].field=(value)
#define REJECT(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; \
  if (semantic(&f) || allocated(&f)) { \
    fprintf(stderr,"TNEW mutation accepted at %d\n",__LINE__); abort(); \
  } \
} while (0)
  REJECT(BOTH(alloc,op2,LJ_MAX_HBITS+1));
  REJECT(BOTH(alloc,t.irt,IRT_TAB|IRT_ISPHI));
  REJECT(BOTH(alloc,t.irt,IRT_NUM|IRT_GUARD|IRT_ISPHI));
  REJECT(BOTH(alloc,o,IR_TDUP));
  REJECT(BOTH(alloc,o,IR_ASTORE));
  REJECT(BOTH(phi,op2,phi));
  REJECT(BOTH(phi,op2,REF_NIL));
  arm64_fixture_reset(&f);
  f.semantic.sinktags=1;
  assert(!semantic(&f) && allocated(&f));
  f.semantic.sinktags=0;
  arm64_fixture_reset(&f);
  f.postra[alloc].r=RID_D0;
  assert(semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  f.postra[alloc].r=RID_SINK;
  assert(semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  f.postra[alloc].s=SPS_FIRST+1; f.view.spadjust=16;
  assert(semantic(&f) && !allocated(&f));
  f.view.spadjust=f.owner->spadjust;
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  lua_close(L);
  printf("TNEW admission/READY constructor OK: capacity=%u native=%u\n",
         capacity,native_constructors);
  return 0;
}
