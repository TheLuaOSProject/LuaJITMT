/*
** Focused recorder-admission safepoint regression.
*/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_TRACE_TEST_HELPERS
#error "t-jit-recorder-safepoint requires LJ_TRACE_TEST_HELPERS"
#endif

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void reset_jit(lua_State *L)
{
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n");
  assert(lj_trace_state_load(L2J(L)) == LJ_TRACE_IDLE);
  assert(jit_token_acq(G(L)) == 0);
  assert(jit_owner_l_acq(L2J(L)) == NULL);
}

static int run_hot_loop(lua_State *L)
{
  int status = luaL_loadstring(L,
    "local s, i = 0, 0\n"
    "while i < 128 do i = i + 1; s = s + i end\n"
    "return s\n");
  assert(status == LUA_OK);
  status = lua_pcall(L, 0, 1, 0);
  if (status == LUA_OK) {
    assert(lua_tointeger(L, -1) == 8256);
    lua_pop(L, 1);
  }
  return status;
}

#if !LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED
static int run_hot_loop_with_errno(lua_State *L, int errnum)
{
  int status = luaL_loadstring(L,
    "local s, i = 0, 0\n"
    "while i < 128 do i = i + 1; s = s + i end\n"
    "return s\n");
  assert(status == LUA_OK);
  errno = errnum;
  status = lua_pcall(L, 0, 1, 0);
  if (status == LUA_OK) {
    assert(lua_tointeger(L, -1) == 8256);
    lua_pop(L, 1);
  }
  return status;
}
#endif

static void assert_owner_clean(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(J->cur.traceno == 0);
}

#if !LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED && \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
typedef struct SideHandshakePublisher {
  global_State *g;
  uint32_t wait_for_observer;
  uint32_t result;
  uint32_t done;
} SideHandshakePublisher;

static void *side_handshake_publisher(void *arg)
{
  SideHandshakePublisher *publisher = (SideHandshakePublisher *)arg;
  while (publisher->wait_for_observer &&
	 !lj_trace_test_admission_observer_waiting())
    la_cpu_pause();
  publisher->result = lj_safepoint_handshake(
	publisher->g, LJ_GC2_HS_REDISPATCH);
  la_store32_rel(&publisher->done, 1);
  return NULL;
}

static void prepare_side_root(lua_State *L)
{
  reset_jit(L);
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "function lj_m6_admission_side(flag, n)\n"
    "  local s, i = 0, 1\n"
    "  while i <= n do\n"
    "    if flag then s = s + i * 3 else s = s + i end\n"
    "    i = i + 1\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local r = 1\n"
    "while r <= 32 do\n"
    "  assert(lj_m6_admission_side(false, 80) == 3240)\n"
    "  r = r + 1\n"
    "end\n"
    "assert(util.traceinfo(1), 'expected side-admission parent trace')\n");
  assert_owner_clean(L);
}

static int run_single_side_exit(lua_State *L)
{
  int status = luaL_loadstring(L,
    "return lj_m6_admission_side(true, 2)");
  assert(status == LUA_OK);
  status = lua_pcall(L, 0, 1, 0);
  if (status == LUA_OK) {
    assert(lua_tointeger(L, -1) == 9);
    lua_pop(L, 1);
  }
  return status;
}

static uint32_t recorded_side_snapshot_count(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TraceNo parent = lj_trace_test_admission_side_parent();
  ExitNo exitno = lj_trace_test_admission_side_exitno();
  GCtrace *T;
  uint32_t count;
  assert(parent != 0);
  assert(lj_gc2_smr_read_try(g));
  T = traceref_safe(J, parent);
  assert(trace_runnable_acq(T, parent));
  assert(exitno < trace_nsnap_acq(T));
  count = (uint32_t)snap_count_acq(&trace_snap_acq(T)[exitno]);
  lj_gc2_smr_read_leave(g);
  return count;
}
#endif

static void test_counted_entry_before_hotcount(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch;
  uint32_t slot, before;
  int status;

  reset_jit(L);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_ENTRY,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_STOPREQ);
  status = run_hot_loop(L);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);

  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  slot = lj_trace_test_admission_hotcount_index();
  before = lj_trace_test_admission_hotcount_before();
  assert(slot < HOTCOUNT_SIZE);
  /* Checked STOPREQ threw before lj_trace_hot() could reset this slot. */
  assert((uint32_t)tg->hotcount[slot] == before);
  assert_owner_clean(L);
  clear_stopreq(tg);
}

#if LJ_PROFILE_TGLOCAL
static void test_profile_entry_and_hotcall(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch;

  reset_jit(L);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_ENTRY,
	LJ_TRACE_TEST_REQUEST_PROFILE, 0);
  assert(run_hot_loop(L) == LUA_OK);
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(gc2_hs_epoch_acq(g) == epoch);  /* Profile requests are uncounted. */
  assert(gc2_hs_pending_acq(g) == 0);
  assert_owner_clean(L);

  /* Exercise the exact helper used after a vm_hotcall marker. */
  lj_tg_profile_request_rel(tg, 1);
  lj_dispatch_test_hotcall_poll(L);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(gc2_hs_epoch_acq(g) == epoch);
}
#endif

#if !LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED
static void test_late_after_token(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch;

  reset_jit(L);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_AFTER_TOKEN,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_REDISPATCH);
  assert(run_hot_loop(L) == LUA_OK);
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_clean_releases() == 1);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert_owner_clean(L);
}

static void test_late_after_root_token_stopreq(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch;
  int status;

  reset_jit(L);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_AFTER_TOKEN,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_STOPREQ);
  status = run_hot_loop(L);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);

  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_clean_releases() == 1);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert_owner_clean(L);
  clear_stopreq(tg);
}

#if !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED && LJ_PROFILE_TGLOCAL
static void test_profile_side_pre_admission(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch;

  prepare_side_root(L);
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_SIDE_ENTRY,
	LJ_TRACE_TEST_REQUEST_PROFILE, 0);
  assert(run_single_side_exit(L) == LUA_OK);
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_side_gate_blocks() == 1);
  assert(lj_trace_test_admission_side_clean_releases() == 0);
  assert(recorded_side_snapshot_count(L) ==
	 lj_trace_test_admission_side_snapshot_before());
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert_owner_clean(L);
}
#endif

#if !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
static void test_real_counted_before_poll_side_admission(lua_State *L,
						 uint32_t stage,
						 uint32_t gate_blocks,
						 uint32_t clean_releases)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  SideHandshakePublisher publisher;
  pthread_t thread;
  uint64_t epoch;

  prepare_side_root(L);
  epoch = gc2_hs_epoch_acq(g);
  memset(&publisher, 0, sizeof(publisher));
  publisher.g = g;
  publisher.wait_for_observer = 1;
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(stage, LJ_TRACE_TEST_REQUEST_OBSERVE, 0);
  lj_safepoint_test_signal_pause_arm(tg);
  assert(pthread_create(&thread, NULL, side_handshake_publisher,
		&publisher) == 0);
  assert(run_single_side_exit(L) == LUA_OK);
  /* The observer exposed real parent/exit metadata, then let the publisher
  ** enter the serialized leader and pause after reqmask but before poll. The
  ** production side gate rearmed and acknowledged that exact request. */
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_observer_waiting() == 0);
  assert(lj_trace_test_admission_side_gate_blocks() == gate_blocks);
  assert(lj_trace_test_admission_side_clean_releases() == clean_releases);
  assert(recorded_side_snapshot_count(L) ==
	 lj_trace_test_admission_side_snapshot_before());
  /* The owner rearmed the missing signal. vm_exit_interp cleared jit_base,
  ** acknowledged the real request, and left only the paused leader sentinel. */
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u);
  assert(gc2_hs_pending_acq(g) == 1);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_safepoint_test_signal_paused());
  assert(la_load32_acq(&publisher.done) == 0);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert_owner_clean(L);

  /* Resume the original publisher. Its real ordered store recreates poll=1
  ** after the owner ACK; the leader-final consumed-poll pass must remove it
  ** before releasing leadership. */
  lj_safepoint_test_signal_pause_release();
  assert(pthread_join(thread, NULL) == 0);
  assert(publisher.result >= 1u);
  assert(la_load32_acq(&publisher.done) == 1);
  assert(lj_safepoint_test_signal_resumed_poll_stores() == 1);
  assert(lj_safepoint_test_signal_consumed_clears() == 1);
  assert(lj_safepoint_test_signal_clean_before_leaves() == 1);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  lj_safepoint_test_signal_pause_reset();
}
#endif

static void test_protected_trace_state_stopreq(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  ASMFunction ordinary;
  uint64_t epoch;
  int status;

  reset_jit(L);
  ordinary = tg->dispatch[BC_ADDVN];
  epoch = gc2_hs_epoch_acq(g);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_TRACE_STATE,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_STOPREQ);
  lj_trace_test_admission_clobber_cleanup_errno(ERANGE);
  status = run_hot_loop_with_errno(L, EDOM);
  /* The protected landing restored EDOM before lj_trace_ins() cleanup. The
  ** test-only cleanup tail replaced it with ERANGE, so only the explicit
  ** save/restore around lj_trace_abort_owner() can preserve this value. */
  assert(errno == EDOM);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);

  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_protected_polls() == 1);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert_owner_clean(L);
  assert(tg->dispatch[BC_ADDVN] == ordinary);
  assert(J->postproc == LJ_POST_NONE);
  clear_stopreq(tg);
}
#endif

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg = L2TG(L);
  assert(tg != NULL && lj_tg_owns_state_acq(tg, L));

  test_counted_entry_before_hotcount(L);
#if LJ_PROFILE_TGLOCAL
  test_profile_entry_and_hotcall(L);
#endif
#if !LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED
  test_late_after_token(L);
  test_late_after_root_token_stopreq(L);
#if !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
#if LJ_PROFILE_TGLOCAL
  test_profile_side_pre_admission(L);
#endif
  test_real_counted_before_poll_side_admission(
    L, LJ_TRACE_TEST_ADMISSION_SIDE_ENTRY, 1, 0);
  test_real_counted_before_poll_side_admission(
    L, LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN, 0, 1);
#endif
  test_protected_trace_state_stopreq(L);
#else
  /* A root-closed target services pre-admission work without publishing a
  ** trace or entering protected recorder state. */
  assert(G2J(G(L))->freetrace == 0);
#endif

  lua_close(L);
  puts("t-jit-recorder-safepoint OK: gated root/side admission and cleanup verified");
  return 0;
}
