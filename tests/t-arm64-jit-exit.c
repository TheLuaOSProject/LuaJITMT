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
#include <string.h>
#include <time.h>

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
#include "lj_thr.h"
#include "lj_asm.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-exit requires open LOOP/JFUNCF entry and closed stitch entry"
#endif

#define EXIT_RACE_ROUNDS 128u
#define EXIT_SLOT_RACE_ROUNDS 256u
#define EXIT_STUB_TRACE 0x1234u
#define EXIT_STUB_NEXITS 4u
#define EXIT_WAIT_TIMEOUT_NS U64x(00000006,fc23ac00)  /* 30 seconds. */

typedef struct ExitLeaseRace {
  global_State *g;
  uint32_t phase;
  uint32_t active_result;
  uint32_t idle_result;
} ExitLeaseRace;

typedef struct ExitSlotRace {
  GCtrace trace;
  global_State *g;
  _Alignas(8) MCode *slots[1];
  _Alignas(8) MCode target[2][2];
  uint32_t payload;
  uint32_t acknowledged;
} ExitSlotRace;

static uint64_t monotonic_ns(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void wait_word(const uint32_t *word, uint32_t value)
{
  uint64_t deadline = monotonic_ns() + EXIT_WAIT_TIMEOUT_NS;
  for (;;) {
    if (la_load32_acq(word) >= value)
      return;
    if (monotonic_ns() >= deadline)
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 synthetic exit race timed out");
}

static void wait_phase(const ExitLeaseRace *race, uint32_t phase)
{
  wait_word(&race->phase, phase);
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

static void *exit_slot_reader(void *arg)
{
  ExitSlotRace *race = (ExitSlotRace *)arg;
  uint32_t round;
  for (round = 0; round < EXIT_SLOT_RACE_ROUNDS; round++) {
    MCode *expected = race->target[round & 1u];
    uint64_t deadline = monotonic_ns() + EXIT_WAIT_TIMEOUT_NS;
    for (;;) {
      if (trace_exittarget_arm64_acq(&race->trace, 0) == expected)
	break;
      assert(monotonic_ns() < deadline);
      (void)lj_thr_retry_yield(NULL);
    }
    assert(la_load32_rlx(&race->payload) == round + 1u);
    la_store32_rel(&race->acknowledged, round + 1u);
  }
  return NULL;
}

static void test_exit_slot_release_acquire(lua_State *L)
{
  ExitSlotRace race;
  pthread_t reader;
  uint32_t round;

  memset(&race, 0, sizeof(race));
  race.g = G(L);
  race.trace.nsnap = 1;
  race.trace.exittab = race.slots;
  race.target[0][0] = race.target[1][0] = A64I_BTI_J;
  race.target[0][1] = race.target[1][1] = A64I_NOP;
  trace_exittarget_arm64_rel(race.g, &race.trace, 0, race.target[1]);
  assert(pthread_create(&reader, NULL, exit_slot_reader, &race) == 0);
  for (round = 0; round < EXIT_SLOT_RACE_ROUNDS; round++) {
    la_store32_rlx(&race.payload, round + 1u);
    trace_exittarget_arm64_rel(race.g, &race.trace, 0,
			       race.target[round & 1u]);
    wait_word(&race.acknowledged, round + 1u);
  }
  assert(pthread_join(reader, NULL) == 0);
  assert(la_load32_acq(&race.acknowledged) == EXIT_SLOT_RACE_ROUNDS);
}

static void test_exit_stub_words(jit_State *J, const char *path)
{
  _Alignas(8) MCode words[64] = { 0 };
  _Alignas(8) MCode *slots[EXIT_STUB_NEXITS] = { NULL };
  GCtrace T;
  MCode *fallback = words;
  MCode *gates = words + ARM64_EXIT_FALLBACK_WORDS;
  MSize nwords;
  intptr_t k64ofs = (intptr_t)((char *)&J->k64[LJ_K64_VM_EXIT_HANDLER] -
			       (char *)&J2GG(J)->g);
  MCode movtrace = A64I_MOVZw | A64F_D(RID_X0) |
		   A64F_U16(EXIT_STUB_TRACE);
  FILE *fp;
  ExitNo i;

  assert(sizeof(((GCtrace *)0)->traceno) == sizeof(uint16_t));
  assert(sizeof(((GCtrace *)0)->nsnap) == sizeof(uint16_t));
  assert(offsetof(ExitState, gpr) == 256u);
  assert(offsetof(ExitState, spill) == 512u);
  assert(k64ofs >= 0 && (k64ofs & 7) == 0 && (k64ofs >> 3) < 4096);

  nwords = lj_asm_arm64_exitstub_test(J, words, 64, EXIT_STUB_TRACE,
				      EXIT_STUB_NEXITS, slots);
  assert(nwords == ARM64_EXIT_FALLBACK_WORDS +
	 ARM64_EXIT_GATE_WORDS * EXIT_STUB_NEXITS);
  assert(((uintptr_t)(void *)fallback & 7u) == 0);
  assert(((uintptr_t)(void *)gates & 7u) == 0);

  assert(fallback[0] == A64I_BTI_J);
  assert(fallback[1] ==
	 (A64I_LDRx | A64F_D(RID_LR) | A64F_N(RID_GL) |
	  A64F_U12((uint32_t)(k64ofs >> 3))));
  assert(fallback[2] == (A64I_BLR_AUTH | A64F_N(RID_LR)));
  assert(fallback[3] == movtrace);

  memset(&T, 0, sizeof(T));
  T.nsnap = EXIT_STUB_NEXITS;
  T.exittab = slots;
  T.exitstub = gates;

  for (i = 0; i < EXIT_STUB_NEXITS; i++) {
    MCode *gate = gates + ARM64_EXIT_GATE_WORDS * i;
    uint64_t literal = 0;
    memcpy(&literal, &gate[6], sizeof(literal));
    assert(((uintptr_t)(void *)&gate[6] & 7u) == 0);
    assert(gate[0] ==
	   (A64I_MOVZw | A64F_D(RID_LR) | A64F_U16(i)));
    assert(gate[1] ==
	   (A64I_STRx | A64F_D(RID_LR) | A64F_N(RID_SP)));
    assert(gate[2] ==
	   (A64I_LDRLx | A64F_D(RID_LR) | A64F_S19(4)));
    assert(gate[3] ==
	   (A64I_LDARx | A64F_D(RID_LR) | A64F_N(RID_LR)));
    assert(gate[4] == (A64I_BR_G_AUTH | A64F_N(RID_LR)));
    assert(gate[5] == A64I_NOP);
    assert(literal == (uint64_t)(uintptr_t)(void *)&slots[i]);
    assert(exitstub_trace_addr_((MCode *)gates, i) == gate);
    assert(exitstub_trace_fallback_addr_((MCode *)gates) == fallback);
    assert(trace_exittarget_arm64_acq(&T, i) == fallback);
    assert(trace_exittarget_arm64_acq(&T, i) != gate);
  }

  fp = fopen(path, "wb");
  assert(fp != NULL);
  assert(fwrite(words, sizeof(MCode), nwords, fp) == nwords);
  assert(fclose(fp) == 0);
}

static void test_exit_stub_layout_limit(void)
{
  _Alignas(8) MCode aligned[2];
  MSize need = 0;

  assert(lj_asm_arm64_exitstub_layout_test(
	(uintptr_t)(void *)&aligned[0], 8191, &need));
  assert(need == 65532u);
  assert(lj_asm_arm64_exitstub_layout_test(
	(uintptr_t)(void *)&aligned[1], 8191, &need));
  assert(need == 65533u);
  assert(!lj_asm_arm64_exitstub_layout_test(
	(uintptr_t)(void *)&aligned[0], 8192, &need));
  assert(need == 65540u);
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
  test_exit_stub_layout_limit();
  test_exit_stub_words(L2J(L), argv[1]);
  test_exit_slot_release_acquire(L);
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
