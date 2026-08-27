/*
** Synthetic contract for the constrained ARM64 trace-exit substrate.
**
** Both ordinary arm64 and arm64e admit the strict native LOOP entry. This
** fixture checks the exit-stub writer independently under both ABIs and
** repeats the TG lifetime-lease/reclaimer ordering implemented by the VM exit
** path.
** Linked-image disassembly is checked separately.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_ARM64_EXIT_TEST_HELPERS) && defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_safepoint.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_asm.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-exit requires open LOOP/JFUNCF entry and closed stitch entry"
#endif

#define EXIT_RACE_ROUNDS 128u
#define EXIT_STUB_TRACE 0x1234u
#define EXIT_STUB_NEXITS 4u

typedef struct ExitLeaseRace {
  global_State *g;
  uint32_t phase;
  uint32_t active_result;
  uint32_t idle_result;
} ExitLeaseRace;

static void wait_phase(const ExitLeaseRace *race, uint32_t phase)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (la_load32_acq(&race->phase) >= phase)
      return;
    la_cpu_pause();
  }
  assert(!"ARM64 synthetic exit race timed out");
}

static void *exit_lease_reclaimer(void *arg)
{
  ExitLeaseRace *race = (ExitLeaseRace *)arg;
  uint32_t round;
  for (round = 0; round < EXIT_RACE_ROUNDS; round++) {
    uint32_t active = round * 4u + 1u;
    wait_phase(race, active);
    race->active_result =
      (uint32_t)lj_gc2_test_idle_reclaim_enter(race->g);
    if (race->active_result)
      lj_gc2_test_idle_reclaim_leave(race->g);
    la_store32_rel(&race->phase, active + 1u);
    wait_phase(race, active + 2u);
    race->idle_result =
      (uint32_t)lj_gc2_test_idle_reclaim_enter(race->g);
    if (race->idle_result)
      lj_gc2_test_idle_reclaim_leave(race->g);
    la_store32_rel(&race->phase, active + 3u);
  }
  return NULL;
}

static void test_exit_stub_words(jit_State *J, const char *path)
{
  MCode direct[32] = { 0 };
  MCode indirect[32] = { 0 };
  MSize ndirect, nindirect;
  intptr_t k64ofs = (intptr_t)((char *)&J->k64[LJ_K64_VM_EXIT_HANDLER] -
			       (char *)&J2GG(J)->g);
  MCode strlr = A64I_STRx | A64F_D(RID_LR) | A64F_N(RID_SP);
  MCode movtrace = A64I_MOVZw | A64F_U16(EXIT_STUB_TRACE);
  FILE *fp;
  ExitNo i;

  assert(sizeof(((GCtrace *)0)->traceno) == sizeof(uint16_t));
  assert(sizeof(((GCtrace *)0)->nsnap) == sizeof(uint16_t));
  assert(offsetof(ExitState, gpr) == 256u);
  assert(offsetof(ExitState, spill) == 512u);
  assert(k64ofs >= 0 && (k64ofs & 7) == 0 && (k64ofs >> 3) < 4096);

  ndirect = lj_asm_arm64_exitstub_test(J, direct, 32, EXIT_STUB_TRACE,
				       EXIT_STUB_NEXITS, 0);
  nindirect = lj_asm_arm64_exitstub_test(J, indirect, 32, EXIT_STUB_TRACE,
					 EXIT_STUB_NEXITS, 1);
  assert(ndirect == EXIT_STUB_NEXITS + 3u);
  assert(nindirect == EXIT_STUB_NEXITS + 4u);

  assert(direct[0] == strlr);
  assert(direct[1] == (A64I_BL | A64F_S26((int32_t)ndirect - 1)));
  assert(direct[2] == movtrace);
  assert(indirect[0] == strlr);
  assert(indirect[1] ==
	 (A64I_LDRx | A64F_D(RID_LR) | A64F_N(RID_GL) |
	  A64F_U12((uint32_t)(k64ofs >> 3))));
  assert(indirect[2] == (A64I_BLR_AUTH | A64F_N(RID_LR)));
  assert(indirect[3] == movtrace);

  for (i = 0; i < EXIT_STUB_NEXITS; i++) {
    uintptr_t saved_direct = (uintptr_t)&direct[3u + i + 1u];
    uintptr_t handler_direct = (uintptr_t)&direct[2];
    uintptr_t saved_indirect = (uintptr_t)&indirect[4u + i + 1u];
    uintptr_t handler_indirect = (uintptr_t)&indirect[3];
    assert(direct[3u+i] == (A64I_BL | A64F_S26(-3-(int32_t)i)));
    assert(indirect[4u+i] == (A64I_BL | A64F_S26(-4-(int32_t)i)));
    assert(exitstub_trace_addr_(direct, i) == &direct[3u+i]);
    assert(exitstub_trace_addr_(indirect, i) == &indirect[4u+i]);
    assert(((saved_direct-handler_direct) >> 2) - 2u == i);
    assert(((saved_indirect-handler_indirect) >> 2) - 2u == i);
  }

  fp = fopen(path, "wb");
  assert(fp != NULL);
  assert(fwrite(direct, sizeof(MCode), ndirect, fp) == ndirect);
  assert(fwrite(indirect, sizeof(MCode), nindirect, fp) == nindirect);
  assert(fclose(fp) == 0);
}

static void require_idle_reclaim(global_State *g)
{
  assert(lj_gc2_test_idle_reclaim_enter(g));
  lj_gc2_test_idle_reclaim_leave(g);
  assert(gc2_jit_phase_gate_acq(g) != 0);
}

static void test_exit_lease_race(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  ExitLeaseRace race = { g, 0, 0, 0 };
  pthread_t worker;
  int32_t oldstate = lj_tg_vmstate_load_acq(tg);
  uint32_t round;

  assert(tg != NULL && tg == L->tg_hint && tg->gl == g);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_jit_exitcode_acq(tg) == 0);
  require_idle_reclaim(g);
  assert(pthread_create(&worker, NULL, exit_lease_reclaimer, &race) == 0);

  for (round = 0; round < EXIT_RACE_ROUNDS; round++) {
    uint32_t active = round * 4u + 1u;
    int error_case = (round & 1u) != 0;
    race.active_result = race.idle_result = 0;

    /* Model handler entry: EXIT is observable, but the TG-local base lease
    ** remains published until restore/event work is complete. */
    lj_tg_store_jit_base(tg, L->base);
    lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_EXIT);
    if (error_case)
      lj_tg_jit_exitcode_rel(tg, LUA_ERRRUN);
    assert(lj_tg_jit_active_acq(tg));
    la_store32_rel(&race.phase, active);
    wait_phase(&race, active + 1u);
    assert(race.active_result == 0u);
    assert(lj_tg_load_jit_base(tg) == L->base);
    if (error_case) {
      assert(lj_tg_jit_exitcode_acq(tg) == LUA_ERRRUN);
      lj_tg_jit_exitcode_rel(tg, 0);
    } else {
      assert(lj_tg_jit_exitcode_acq(tg) == 0);
    }

    /* Model both normal and error quiescence: base is already authoritative,
    ** then the lease clears before INTERP becomes observable. */
    assert(L->base != NULL);
    lj_tg_store_jit_base(tg, NULL);
    lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
    assert(!lj_tg_jit_active_acq(tg));
    la_store32_rel(&race.phase, active + 2u);
    wait_phase(&race, active + 3u);
    assert(race.idle_result == 1u);
    assert(gc2_jit_phase_gate_acq(g) != 0);
  }
  assert(pthread_join(worker, NULL) == 0);
  lj_tg_vmstate_store_rel(tg, oldstate);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_jit_exitcode_acq(tg) == 0);
}

static void test_profile_ack_after_quiescence(lua_State *L)
{
  TGState *tg = G2TG(G(L));
  assert(lj_tg_load_jit_base(tg) == NULL);
  lj_tg_profile_request_rel(tg, 1);
  assert(lj_tg_profile_request_acq(tg) == 1);
  (void)lj_safepoint_ack_check(L);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

int main(int argc, char **argv)
{
  lua_State *L;
  assert(argc == 2);
  L = luaL_newstate();
  assert(L != NULL && L2J(L) != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  test_exit_stub_words(L2J(L), argv[1]);
  test_exit_lease_race(L);
  test_profile_ack_after_quiescence(L);
  lua_close(L);
#if LJ_ABI_PAUTH
  puts("t-arm64-jit-exit OK: arm64e exit stubs under open LOOP/JFUNCF policy and lease races verified");
#else
  puts("t-arm64-jit-exit OK: arm64 exit stubs under open LOOP/JFUNCF policy and lease races verified");
#endif
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-exit SKIP");
  return 0;
}

#endif
