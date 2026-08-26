/*
** Focused ARM64 TG-local JIT emitter word contract.
**
** The real emitter remains unreachable from recording while
** recorder admission is fail-closed. This fixture invokes test-only wrappers,
** checks every emitted word and writes those exact words for Mach-O
** disassembly by the companion CI gate.
*/

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_ARM64_EMIT_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_dispatch.h"
#include "lj_target.h"
#include "lj_jit.h"
#include "lj_asm.h"

#if !LJ_HASJIT || !LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED
#error "t-arm64-jit-emitter requires fail-closed ARM64 recorder admission"
#endif

#define ENC_ADDx_IMM(rd, rn, imm) \
  (0x91000000u | ((uint32_t)(imm) << 10) | \
   ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define ENC_LDARx(rt, rn) \
  (0xc8dffc00u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define ENC_LDARw(rt, rn) \
  (0x88dffc00u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define ENC_STLRx(rt, rn) \
  (0xc89ffc00u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define ENC_MOVZw(rd, imm) \
  (0x52800000u | ((uint32_t)(imm) << 5) | (uint32_t)(rd))
#define ENC_MOVNw(rd, imm) \
  (0x12800000u | ((uint32_t)(imm) << 5) | (uint32_t)(rd))
#define ENC_STRw(rt, rn, ofs) \
  (0xb9000000u | (((uint32_t)(ofs) >> 2) << 10) | \
   ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define ENC_ADDx_IMM_SHIFT12(rd, rn, imm12) \
  (0x91400000u | ((uint32_t)(imm12) << 10) | \
   ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define ENC_MOVx(rd, rm) \
  (0xaa0003e0u | ((uint32_t)(rm) << 16) | (uint32_t)(rd))

static size_t append_words(MCode *all, size_t nall, const MCode *words,
			   MSize n)
{
  MSize i;
  for (i = 0; i < n; i++)
    all[nall++] = words[i];
  return nall;
}

static MSize check_emit(jit_State *J, LJArm64EmitTestOp op, int32_t state,
			const MCode *expected, MSize nexpected,
			MCode *all, size_t *nall)
{
  MCode words[8];
  MSize i, n = lj_asm_arm64_emit_test(J, words, 8, op, state);
  assert(n == nexpected);
  for (i = 0; i < n; i++)
    assert(words[i] == expected[i]);
  *nall = append_words(all, *nall, words, n);
  return n;
}

int main(int argc, char **argv)
{
  static const int32_t positive = 0x1234;
  static const int32_t negative = (int32_t)~LJ_VMST_C;
  const MCode get_cur_L[] = {
    ENC_ADDx_IMM(RID_TMP, RID_DISPATCH, DISPATCH_TG(cur_L)),
    ENC_LDARx(RID_X0, RID_TMP)
  };
  const MCode get_jit_base[] = {
    ENC_ADDx_IMM(RID_TMP, RID_DISPATCH, DISPATCH_TG(jit_base)),
    ENC_LDARx(RID_X1, RID_TMP)
  };
  const MCode set_jit_base[] = {
    ENC_ADDx_IMM(RID_TMP, RID_DISPATCH, DISPATCH_TG(jit_base)),
    ENC_STLRx(RID_X2, RID_TMP)
  };
  const MCode setvm_positive[] = {
    ENC_MOVZw(RID_TMP, positive),
    0xd5033bbfu,
    ENC_STRw(RID_TMP, RID_DISPATCH, DISPATCH_TG(vmstate))
  };
  const MCode setvm_negative[] = {
    ENC_MOVNw(RID_TMP, LJ_VMST_C),
    0xd5033bbfu,
    ENC_STRw(RID_TMP, RID_DISPATCH, DISPATCH_TG(vmstate))
  };
  const MCode get_poll[] = {
    ENC_ADDx_IMM(RID_TMP, RID_DISPATCH, DISPATCH_TG(poll)),
    ENC_LDARw(RID_X3, RID_TMP)
  };
  const MCode get_profile_request[] = {
    ENC_ADDx_IMM(RID_TMP, RID_DISPATCH, DISPATCH_TG(profile_request)),
    ENC_LDARw(RID_X4, RID_TMP)
  };
  MCode get_jit_gate[3];
  MSize n_get_jit_gate = 0;
  uint32_t gate_ofs = (uint32_t)offsetof(global_State, gc2.jit_phase_gate);
  uint32_t gate_hi = gate_ofs & 0xfff000u;
  uint32_t gate_lo = gate_ofs & 0xfffu;
  MCode all[64];
  size_t nall = 0;
  lua_State *L;
  FILE *fp;

  assert(argc == 2);
  assert(RID_DISPATCH == RID_X25 && RID_TMP == RID_LR);
  assert(!rset_test(RSET_GPR, RID_DISPATCH));
  assert(!rset_test(RSET_GPR, RID_TMP));
  assert(rset_test(RSET_GPR, RID_BASE));
  assert(gate_ofs <= 0xffffffu && (gate_ofs & 3u) == 0);
  if (gate_hi != 0) {
    get_jit_gate[n_get_jit_gate++] =
      ENC_ADDx_IMM_SHIFT12(RID_TMP, RID_GL, gate_hi >> 12);
    if (gate_lo != 0)
      get_jit_gate[n_get_jit_gate++] =
        ENC_ADDx_IMM(RID_TMP, RID_TMP, gate_lo);
  } else if (gate_lo != 0) {
    get_jit_gate[n_get_jit_gate++] =
      ENC_ADDx_IMM(RID_TMP, RID_GL, gate_lo);
  } else {
    get_jit_gate[n_get_jit_gate++] = ENC_MOVx(RID_TMP, RID_GL);
  }
  get_jit_gate[n_get_jit_gate++] = ENC_LDARw(RID_X5, RID_TMP);
  L = luaL_newstate();
  assert(L != NULL && L2J(L) != NULL);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_GET_CUR_L, 0,
	     get_cur_L, 2, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_GET_JIT_BASE, 0,
	     get_jit_base, 2, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_SET_JIT_BASE, 0,
	     set_jit_base, 2, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_SETVMSTATE, positive,
	     setvm_positive, 3, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_SETVMSTATE, negative,
	     setvm_negative, 3, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_SETVMSTATE_ROOT, positive,
	     setvm_positive, 3, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_SETVMSTATE_ROOT, negative,
	     setvm_negative, 3, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_GET_POLL, 0,
	     get_poll, 2, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_GET_PROFILE_REQUEST, 0,
	     get_profile_request, 2, all, &nall);
  check_emit(L2J(L), LJ_ARM64_EMIT_TEST_GET_JIT_GATE, 0,
	     get_jit_gate, n_get_jit_gate, all, &nall);
  lua_close(L);

  fp = fopen(argv[1], "wb");
  assert(fp != NULL);
  assert(fwrite(all, sizeof(MCode), nall, fp) == nall);
  assert(fclose(fp) == 0);
  puts("t-arm64-jit-emitter OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-emitter SKIP");
  return 0;
}

#endif
