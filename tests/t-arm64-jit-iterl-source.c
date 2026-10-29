/* Source-only iterator tuple proof. Native ITERL admission remains closed. */
#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"
#if LJ_TARGET_ARM64 && defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#include "lj_jit.h"
#include "lj_asm.h"
#include "lj_bc.h"

static void check_source(lua_State *L, const char *text)
{
  GCproto *pt;
  BCIns *bc, original, patched;
  MSize loop = 0, rejections = 0;
  LJArm64IteratorSource source;
  assert(luaL_dostring(L, text) == 0);
  assert(tvisfunc(L->top-1));
  pt = funcproto(funcV(L->top-1));
  bc = proto_bc(pt);
  for (MSize i = 1; i < pt->sizebc; i++)
    if (bc_op(bc[i]) == BC_ITERL) { assert(!loop); loop = i; }
  assert(loop);
  original = bc[loop];
  assert(lj_asm_arm64_iterl_tuple(pt, &bc[loop], original, original, 1, &source));
  assert(source.call_pc+1 == loop && source.setup_pc+1 == source.body_pc);
  assert(source.call_pc >= source.body_pc && source.base == bc_a(original));
  /* The private state has no peer or executing function during these writes. */
  for (unsigned mutation = 0; mutation < 9; mutation++) {
    BCIns saved, bad;
    MSize at;
    if (mutation < 4) {
      at = source.call_pc; saved = bc[at]; bad = saved;
      if (mutation == 0) setbc_op(&bad, BC_CALL);
      if (mutation == 1) setbc_a(&bad, (BCReg)(bc_a(bad)+1));
      if (mutation == 2) setbc_b(&bad, 3);
      if (mutation == 3) setbc_c(&bad, 2);
    } else if (mutation < 7) {
      at = source.setup_pc; saved = bc[at]; bad = saved;
      if (mutation == 4) setbc_op(&bad, BC_ISNEXT);
      if (mutation == 5) setbc_a(&bad, (BCReg)(bc_a(bad)-1));
      if (mutation == 6) setbc_j(&bad, bc_j(bad)+1);
    } else {
      at = loop; saved = bc[at]; bad = saved;
      if (mutation == 7) setbc_a(&bad, (BCReg)(bc_a(bad)+1));
      if (mutation == 8) setbc_j(&bad, bc_j(bad)+1);
    }
    bc_publish(&bc[at], bad);
    assert(!lj_asm_arm64_iterl_tuple(pt, &bc[loop], original, original, 1, &source));
    rejections++;
    bc_publish(&bc[at], saved);
  }
  /* Coherent changed generations must also fail structural validation, even
  ** when the expected word is changed with the live source word. */
  for (unsigned mutation = 0; mutation < 6; mutation++) {
    BCIns bad = original;
    if (mutation == 0) setbc_a(&bad, 2);
    if (mutation == 1) setbc_a(&bad, pt->framesize);
    if (mutation == 2) setbc_j(&bad, 0);
    if (mutation == 3) setbc_j(&bad, -1);
    if (mutation == 4) setbc_j(&bad, -(int)loop);
    if (mutation == 5) setbc_op(&bad, BC_FORL);
    bc_publish(&bc[loop], bad);
    assert(!lj_asm_arm64_iterl_tuple(pt, &bc[loop], bad, bad, 1, &source));
    bc_publish(&bc[loop], original);
    rejections++;
  }
  patched = BCINS_AD(BC_JITERL, bc_a(original), 7);
  bc_publish(&bc[loop], patched);
  assert(lj_asm_arm64_iterl_tuple(pt, &bc[loop], original, patched, 0, &source));
  assert(!lj_asm_arm64_iterl_tuple(pt, &bc[loop], original, original, 1, &source));
  assert(!lj_asm_arm64_iterl_tuple(pt, &bc[loop], original,
    BCINS_AD(BC_JITERL, bc_a(original)+1, 7), 0, &source));
  assert(!lj_asm_arm64_iterl_tuple(pt, &bc[loop], original,
    BCINS_AD(BC_JITERL, bc_a(original), 8), 0, &source));
  bc_publish(&bc[loop], original);
  assert(lj_asm_arm64_iterl_tuple(pt, &bc[loop], original, original, 1, &source));
  assert(!lj_asm_arm64_iterl_tuple(pt, bc, original, original, 1, &source));
  assert(!lj_asm_arm64_iterl_tuple(pt, bc+pt->sizebc, original, original, 1, &source));
  assert(!lj_asm_arm64_iterl_tuple(NULL, &bc[loop], original, original, 1, &source));
  assert(rejections == 15);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L); luaL_openlibs(L);
  assert(luaL_dostring(L, "jit.off()") == 0);
  check_source(L, "local function iter(n,k) return k+1 end; "
    "return function(n) local s=.5; for k in iter,n,0 do "
    "s=s+k; if k>=n then break end end return s end");
  check_source(L, "local function iter(n,k) if k<n then return k+1 end end; "
    "return function(n) local s=.5; for k in iter,n,0 do s=s+k end return s end");
  lua_close(L);
  puts("ARM64 ITERL tuple: two source shapes and generation mutations PASS (source proof only)");
  return 0;
}
#else
int main(void) { return 0; }
#endif
