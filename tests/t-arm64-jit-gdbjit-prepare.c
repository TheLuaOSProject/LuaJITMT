/*
** Focused ARM64 regression test for optional GDB JIT metadata preparation.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_gdbjit.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED || \
    !defined(LUAJIT_USE_GDBJIT) || !defined(LJ_TRACE_TEST_HELPERS) || \
    !defined(LJ_GDBJIT_TEST_HELPERS)
#error "fixture requires the exact-first-side experimental ARM64 GDBJIT build"
#endif

typedef enum GDBJITCase {
  GDBJIT_CASE_NORMAL,
  GDBJIT_CASE_MEDIUM_NAME,
  GDBJIT_CASE_ALLOC_OMIT,
  GDBJIT_CASE_LOCK_OMIT,
  GDBJIT_CASE_LONG_NAME
} GDBJITCase;

static const char fixture_lua[] =
  "function __gdbjit_prepare_loop(n)\n"
  "  local i,s=0,0\n"
  "  while i<n do i=i+1 s=s+i end\n"
  "  return s\n"
  "end\n"
  "function __gdbjit_first_side(n, bias)\n"
  "  local i=0\n"
  "  while i<n do\n"
  "    i=i+1\n"
  "    if bias~=0 then i=i+1 end\n"
  "  end\n"
  "  return i\n"
  "end\n"
  "function __gdbjit_unsupported_side(n, bias)\n"
  "  local i=0\n"
  "  while i<n do i=(i~=0 and i or i)+3 end\n"
  "  return i\n"
  "end\n";

static char *chunkname_with_payload(size_t payload)
{
  const size_t len = payload+1u;
  char *name = (char *)malloc(len+1u);
  assert(name != NULL);
  name[0] = '@';
  memset(name+1, 'g', payload);
  name[len] = '\0';
  return name;
}

static void call_integer_loop(lua_State *L)
{
  int status;
  lua_getglobal(L, "__gdbjit_prepare_loop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 20);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "GDBJIT loop call failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_tointeger(L, -1) == 210);
  lua_pop(L, 1);
}

static GCtrace *published_root(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCfunc *fn;
  GCproto *pt;
  GCtrace *T;
  TraceNo tr;

  lua_getglobal(L, "__gdbjit_prepare_loop");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);

  tr = proto_trace_acq(pt);
  assert(tr != 0);
  T = traceref_safe(J, tr);
  assert(T != NULL);
  assert(trace_runnable_acq(T, tr));
  assert(trace_root_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_mcode_acq(T) != NULL);
  return T;
}

static void assert_stats(GDBJITCase which, const LJGDBJITTestStats *stats)
{
  assert(stats->prepare_attempts == 1);
  assert(stats->aborts_after_token == 0);
  switch (which) {
  case GDBJIT_CASE_NORMAL:
  case GDBJIT_CASE_MEDIUM_NAME:
    assert(stats->prepare_successes == 1);
    assert(stats->prepare_bounds_omits == 0);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 1);
    assert(stats->commit_successes == 1);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 0);
    assert(stats->aborts_after_token == 0);
    break;
  case GDBJIT_CASE_ALLOC_OMIT:
    assert(stats->prepare_successes == 0);
    assert(stats->prepare_bounds_omits == 0);
    assert(stats->prepare_alloc_omits == 1);
    assert(stats->commit_attempts == 0);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 0);
    break;
  case GDBJIT_CASE_LOCK_OMIT:
    assert(stats->prepare_successes == 1);
    assert(stats->prepare_bounds_omits == 0);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 1);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 1);
    assert(stats->aborts == 1);
    break;
  case GDBJIT_CASE_LONG_NAME:
    assert(stats->prepare_successes == 0);
    assert(stats->prepare_bounds_omits == 1);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 0);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 0);
    assert(stats->aborts_after_token == 0);
    break;
  }
  if (which == GDBJIT_CASE_NORMAL || which == GDBJIT_CASE_MEDIUM_NAME) {
    assert(stats->register_callbacks == 1);
    assert(stats->register_callbacks_ready == 1);
  } else {
    assert(stats->register_callbacks == 0);
    assert(stats->register_callbacks_ready == 0);
  }
}

/* Exercise the future pre-trace_save source selection without publishing the
** synthetic destination. The copied BASE names the live root as its parent so
** the exact-generation accept and pointer-mismatch reject paths are both real
** slot lookups under the bounded SMR reader. */
static void exercise_private_prepare(lua_State *L, GCtrace *parent)
{
  jit_State *J = G2J(G(L));
  GCtrace saved_cur;
  GCtrace target;
  GCtrace other;
  GCtrace impostor;
  GCtrace *saved_curfinal = J->curfinal;
  lua_State *saved_owner = jit_owner_l_acq(J);
  IRIns *private_ir = (IRIns *)calloc(REF_BASE+1u, sizeof(IRIns));
  GDBJITPrepared *prep;
  LJGDBJITTestStats stats;

  assert(private_ir != NULL);
  assert(saved_curfinal == NULL);
  assert(saved_owner == NULL);
  memcpy(&saved_cur, &J->cur, sizeof(saved_cur));
  memcpy(&J->cur, parent, sizeof(J->cur));
  memset(&target, 0, sizeof(target));
  memset(&other, 0, sizeof(other));
  memcpy(&impostor, parent, sizeof(impostor));
  assert(trace_traceno_acq(&impostor) == trace_traceno_acq(parent));
  private_ir[REF_BASE] = ir_load_acq(&trace_ir_acq(parent)[REF_BASE]);
  private_ir[REF_BASE].op1 = trace_traceno_acq(parent);
  J->cur.ir = private_ir;
  J->curfinal = &target;
  jit_owner_l_rel(J, L);

  lj_gdbjit_test_reset();
  prep = lj_gdbjit_preparetrace(J, &target, parent);
  assert(prep != NULL);
  lj_gdbjit_aborttrace(G(L), prep);
  prep = lj_gdbjit_preparetrace(J, &target, &impostor);
  assert(prep == NULL);
  lj_gdbjit_test_stats(&stats);
  assert(stats.prepare_attempts == 2);
  assert(stats.prepare_successes == 1);
  assert(stats.prepare_bounds_omits == 0);
  assert(stats.prepare_alloc_omits == 0);
  assert(stats.commit_attempts == 0);
  assert(stats.commit_successes == 0);
  assert(stats.commit_lock_omits == 0);
  assert(stats.aborts == 1);
  assert(stats.register_callbacks == 0);
  assert(stats.register_callbacks_ready == 0);

  /* A wrong destination does not consume the preparation. The first valid
  ** destination attempt does, even when the descriptor try-lock is omitted;
  ** replay cannot acquire the lock or mutate the target and abort owns the
  ** allocation exactly once. */
  lj_gdbjit_test_reset();
  prep = lj_gdbjit_preparetrace(J, &target, parent);
  assert(prep != NULL);
  assert(lj_gdbjit_committrace(&other, prep) == 0);
  assert(lj_gdbjit_test_descriptor_lock_acquire() == 1);
  assert(lj_gdbjit_committrace(&target, prep) == 0);
  lj_gdbjit_test_descriptor_lock_release();
  assert(lj_gdbjit_committrace(&target, prep) == 0);
  assert(trace_gdbjit_entry_acq(&target) == NULL);
  lj_gdbjit_aborttrace(G(L), prep);
  lj_gdbjit_test_stats(&stats);
  assert(stats.prepare_attempts == 1);
  assert(stats.prepare_successes == 1);
  assert(stats.prepare_bounds_omits == 0);
  assert(stats.prepare_alloc_omits == 0);
  assert(stats.commit_attempts == 1);
  assert(stats.commit_successes == 0);
  assert(stats.commit_lock_omits == 1);
  assert(stats.aborts == 1);
  assert(stats.register_callbacks == 0);
  assert(stats.register_callbacks_ready == 0);

  jit_owner_l_rel(J, saved_owner);
  J->curfinal = saved_curfinal;
  memcpy(&J->cur, &saved_cur, sizeof(J->cur));
  free(private_ir);
}

static GDBJITPrepared *prepare_filename_payload(lua_State *L, jit_State *J,
						 GCtrace *T, GCproto *pt,
						 char *name, size_t payload)
{
  GCstr *chunkname;
  name[0] = '@';
  memset(name+1, 'b', payload);
  name[payload+1u] = '\0';
  chunkname = lj_str_new(L, name, payload+1u);
  assert(chunkname != NULL);
  setgcrefrel(pt->chunkname, obj2gco(chunkname));
  return lj_gdbjit_preparetrace(J, T, NULL);
}

/* Discover the real conservative boundary, then repeat only its adjacent
** accept/reject pair under fresh counters. The accepted descriptor must expose
** exactly the copied object extent and finish close to the fixed capacity. */
static void exercise_filename_boundary(lua_State *L, GCtrace *T)
{
  jit_State *J = G2J(G(L));
  GCproto *pt = trace_startpt_acq(T);
  GCstr *saved_chunkname = proto_chunkname_acq(pt);
  lua_State *saved_owner = jit_owner_l_acq(J);
  char *name = (char *)malloc(4098u);
  size_t lo = 0, hi = 4096u;
  size_t symfile_size, object_size, capacity;
  int was_running = lua_gc(L, LUA_GCISRUNNING, 0);
  GDBJITPrepared *prep;
  LJGDBJITTestStats stats;

  assert(pt != NULL && saved_chunkname != NULL && name != NULL);
  assert(saved_owner == NULL);
  jit_owner_l_rel(J, L);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  prep = prepare_filename_payload(L, J, T, pt, name, lo);
  assert(prep != NULL);
  lj_gdbjit_aborttrace(G(L), prep);
  prep = prepare_filename_payload(L, J, T, pt, name, hi);
  assert(prep == NULL);

  while (hi-lo > 1u) {
    size_t mid = lo+(hi-lo)/2u;
    prep = prepare_filename_payload(L, J, T, pt, name, mid);
    if (prep != NULL) {
      lo = mid;
      lj_gdbjit_aborttrace(G(L), prep);
    } else {
      hi = mid;
    }
  }
  assert(hi == lo+1u);

  lj_gdbjit_test_reset();
  prep = prepare_filename_payload(L, J, T, pt, name, lo);
  assert(prep != NULL);
  symfile_size = lj_gdbjit_test_prepared_symfile_size(prep);
  object_size = lj_gdbjit_test_prepared_object_size(prep);
  capacity = lj_gdbjit_test_object_capacity();
  assert(symfile_size == object_size);
  assert(object_size < capacity);
  assert(capacity-object_size <= 3u*sizeof(uintptr_t));
  lj_gdbjit_aborttrace(G(L), prep);
  prep = prepare_filename_payload(L, J, T, pt, name, hi);
  assert(prep == NULL);
  lj_gdbjit_test_stats(&stats);
  assert(stats.prepare_attempts == 2);
  assert(stats.prepare_successes == 1);
  assert(stats.prepare_bounds_omits == 1);
  assert(stats.prepare_alloc_omits == 0);
  assert(stats.commit_attempts == 0);
  assert(stats.commit_successes == 0);
  assert(stats.commit_lock_omits == 0);
  assert(stats.aborts == 1);
  assert(stats.register_callbacks == 0);
  assert(stats.register_callbacks_ready == 0);

  setgcrefrel(pt->chunkname, obj2gco(saved_chunkname));
  jit_owner_l_rel(J, saved_owner);
  if (was_running)
    (void)lua_gc(L, LUA_GCRESTART, 0);
  free(name);
}

static void run_case(GDBJITCase which)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  const char *chunkname = "@gdbjit-prepare.lua";
  char *dynamic_name = NULL;
  LJGDBJITTestStats stats;
  GCtrace *T;
  uint32_t calls;
  int descriptor_held = 0;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2')\n");
  if (which == GDBJIT_CASE_MEDIUM_NAME) {
    dynamic_name = chunkname_with_payload(1200u);
    chunkname = dynamic_name;
  } else if (which == GDBJIT_CASE_LONG_NAME) {
    dynamic_name = chunkname_with_payload(6000u);
    chunkname = dynamic_name;
  }
  ljt_lua_assert_ok(L,
    luaL_loadbuffer(L, fixture_lua, sizeof(fixture_lua)-1u, chunkname),
    "load GDBJIT fixture");
  ljt_lua_pcall(L, 0, 0, "install GDBJIT fixture");

  lj_gdbjit_test_reset();
  if (which == GDBJIT_CASE_ALLOC_OMIT)
    lj_gdbjit_test_force_prepare_alloc_omit();
  else if (which == GDBJIT_CASE_LOCK_OMIT) {
    assert(lj_gdbjit_test_descriptor_lock_acquire() == 1);
    descriptor_held = 1;
  }

  for (calls = 0; calls < 40; calls++)
    call_integer_loop(L);
  if (descriptor_held)
    lj_gdbjit_test_descriptor_lock_release();
  T = published_root(L);
  assert((trace_gdbjit_entry_acq(T) != NULL) ==
         (which == GDBJIT_CASE_NORMAL ||
          which == GDBJIT_CASE_MEDIUM_NAME));
  lj_gdbjit_test_stats(&stats);
  assert_stats(which, &stats);
  if (which == GDBJIT_CASE_NORMAL) {
    exercise_private_prepare(L, T);
    exercise_filename_boundary(L, T);
  }

  lua_close(L);
  free(dynamic_name);
}

typedef enum GDBJITSideCase {
  GDBJIT_SIDE_SUCCESS_SCOPED,
  GDBJIT_SIDE_ALLOC_OMIT,
  GDBJIT_SIDE_LOCK_OMIT,
  GDBJIT_SIDE_POST_PREPARE_ROLLBACK,
  GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR,
  GDBJIT_SIDE_SUCCESS_FULL_FLUSH,
  GDBJIT_SIDE_RETIRE_LOCK_OMIT
} GDBJITSideCase;

static lua_Integer call_named_pair(lua_State *L, const char *name,
	lua_Integer n, lua_Integer bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "GDBJIT first-side call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static void call_first_side_expect_external_error(lua_State *L)
{
  int status;
  lua_getglobal(L, "__gdbjit_first_side");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 3);
  lua_pushinteger(L, 1);
  status = lua_pcall(L, 2, 1, 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isboolean(L, -1) && lua_toboolean(L, -1));
  lua_pop(L, 1);
}

static GCproto *named_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

#define call_first_side_pair(L, n, bias) \
  call_named_pair((L), "__gdbjit_first_side", (n), (bias))

static GCtrace *record_first_side_root(lua_State *L, jit_State *J,
	TraceNo *rootnop)
{
  GCproto *pt = named_proto(L, "__gdbjit_first_side");
  GCtrace *root = NULL;
  unsigned attempt;
  for (attempt = 0; attempt < 64; attempt++) {
    TraceNo rootno;
    assert(call_first_side_pair(L, 3, 0) == 3);
    rootno = proto_trace_acq(pt);
    if (rootno != 0) {
      root = traceref_safe(J, rootno);
      if (trace_runnable_acq(root, rootno)) {
	*rootnop = rootno;
	break;
      }
    }
  }
  assert(root != NULL && *rootnop != 0);
  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == *rootnop);
  assert(trace_linktype_acq(root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_gdbjit_entry_acq(root) != NULL);
  return root;
}

static GCtrace *record_first_side_child(lua_State *L, jit_State *J,
	GCtrace *root, TraceNo *childnop)
{
  GCtrace *child = NULL;
  unsigned attempt;
  for (attempt = 0; attempt < 8; attempt++) {
    assert(call_first_side_pair(L, 3, 1) == 4);
    *childnop = trace_nextside_acq(root);
    if (*childnop != 0) {
      child = traceref_safe(J, *childnop);
      if (trace_runnable_acq(child, *childnop))
	break;
    }
  }
  assert(child != NULL && *childnop != 0);
  return child;
}

static void assert_published_child(lua_State *L, jit_State *J,
	GCtrace *root, TraceNo rootno, GCtrace *child, TraceNo childno,
	int registered)
{
  int token;
  assert(trace_root_acq(child) == rootno);
  assert(trace_link_acq(child) == rootno);
  assert(trace_linktype_acq(child) == LJ_TRLINK_ROOT);
  assert(trace_nchild_acq(root) == 1);
  assert(trace_nextside_acq(root) == childno);
  assert((trace_gdbjit_entry_acq(child) != NULL) == registered);
  if (registered)
    assert(trace_gdbjit_entry_acq(child) != trace_gdbjit_entry_acq(root));
  token = lj_jit_token_acquire_wait(J);
  assert(token == 1);
  assert(lj_trace_test_arm64_gdbjit_callback_ready(
	J, root, 2, child));
  lj_jit_token_release(J);

  lj_trace_test_reset_exit_stats();
  assert(call_first_side_pair(L, 3, 1) == 4);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == childno);
  assert(lj_trace_test_first_exitno() == 3);
  assert(lj_trace_test_last_exit_parent() == childno);
  assert(lj_trace_test_last_exitno() == 3);
}

static uint32_t reclaim_trace_at(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  uint32_t reclaimed;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(lj_jit_token_try(J));
  reclaimed = lj_trace_reclaim_retired(g, epoch);
  lj_jit_token_release(J);
  lj_gc2_test_idle_reclaim_leave(g);
  return reclaimed;
}

static void assert_side_stats(GDBJITSideCase which,
	const LJGDBJITTestStats *stats)
{
  assert(stats->prepare_attempts == 1);
  assert(stats->prepare_bounds_omits == 0);
  assert(stats->aborts_after_token ==
	 (which == GDBJIT_SIDE_LOCK_OMIT ||
	  which == GDBJIT_SIDE_POST_PREPARE_ROLLBACK ||
	  which == GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR));
  if (which == GDBJIT_SIDE_ALLOC_OMIT) {
    assert(stats->prepare_successes == 0);
    assert(stats->prepare_alloc_omits == 1);
    assert(stats->commit_attempts == 0);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 0);
    assert(stats->register_callbacks == 0);
    assert(stats->register_callbacks_ready == 0);
  } else if (which == GDBJIT_SIDE_LOCK_OMIT) {
    assert(stats->prepare_successes == 1);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 1);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 1);
    assert(stats->aborts == 1);
    assert(stats->register_callbacks == 0);
    assert(stats->register_callbacks_ready == 0);
  } else if (which == GDBJIT_SIDE_POST_PREPARE_ROLLBACK ||
	     which == GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR) {
    assert(stats->prepare_successes == 1);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 0);
    assert(stats->commit_successes == 0);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 1);
    assert(stats->register_callbacks == 0);
    assert(stats->register_callbacks_ready == 0);
  } else {
    assert(stats->prepare_successes == 1);
    assert(stats->prepare_alloc_omits == 0);
    assert(stats->commit_attempts == 1);
    assert(stats->commit_successes == 1);
    assert(stats->commit_lock_omits == 0);
    assert(stats->aborts == 0);
    assert(stats->register_callbacks == 1);
    assert(stats->register_callbacks_ready == 1);
  }
}

static void assert_side_quiescent(lua_State *L, jit_State *J)
{
  assert(J->curfinal == NULL);
  assert(J->gdbjit_pending_abort == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(G(L)) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(gc2_smr_readers_acq(G(L)) == 0);
}

static void run_side_case(GDBJITSideCase which)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  GCtrace *root;
  GCtrace *child;
  SnapShot *rootsnap;
  TraceNo rootno = 0, childno = 0;
  TraceNo rollback_slot = 0;
  MSize rollback_count = 0;
  void *root_entry;
  LJGDBJITTestStats stats;
  int descriptor_held = 0;
  int closed = 0;
  int rollback_case = which == GDBJIT_SIDE_POST_PREPARE_ROLLBACK ||
	which == GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n");
  ljt_lua_assert_ok(L,
    luaL_loadbuffer(L, fixture_lua, sizeof(fixture_lua)-1u,
	"@gdbjit-first-side.lua"),
    "load GDBJIT first-side fixture");
  ljt_lua_pcall(L, 0, 0, "install GDBJIT first-side fixture");

  root = record_first_side_root(L, J, &rootno);
  root_entry = trace_gdbjit_entry_acq(root);
  assert(root_entry != NULL);
  rootsnap = trace_snap_acq(root);
  assert(rootsnap != NULL && trace_nsnap_acq(root) > 2);
  if (rollback_case) {
    rollback_count = snap_count_acq(&rootsnap[2]);
    assert(rollback_count < SNAPCOUNT_DONE);
  }
  lj_gdbjit_test_reset();

  if (which == GDBJIT_SIDE_ALLOC_OMIT) {
    lj_gdbjit_test_force_prepare_alloc_omit();
  } else if (which == GDBJIT_SIDE_LOCK_OMIT) {
    assert(lj_gdbjit_test_descriptor_lock_acquire() == 1);
    descriptor_held = 1;
  } else if (which == GDBJIT_SIDE_POST_PREPARE_ROLLBACK) {
    lj_trace_test_arm64_gdbjit_force_post_prepare_rollback();
  } else if (which == GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR) {
    lj_trace_test_arm64_gdbjit_force_post_prepare_external_error();
  }

  if (rollback_case) {
    if (which == GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR)
      call_first_side_expect_external_error(L);
    else
      assert(call_first_side_pair(L, 3, 1) == 4);
    assert(trace_nextside_acq(root) == 0);
    assert(trace_nchild_acq(root) == 0);
    assert(trace_runnable_acq(root, rootno));
    assert(trace_gdbjit_entry_acq(root) == root_entry);
    assert(snap_count_acq(&rootsnap[2]) >= rollback_count);
    assert(snap_count_acq(&rootsnap[2]) < SNAPCOUNT_DONE);
    rollback_slot = lj_trace_test_arm64_gdbjit_post_prepare_traceno();
    assert(rollback_slot != 0);
    assert(traceref_safe(J, rollback_slot) == NULL);
  } else {
    int registered = which == GDBJIT_SIDE_SUCCESS_SCOPED ||
	which == GDBJIT_SIDE_SUCCESS_FULL_FLUSH ||
	which == GDBJIT_SIDE_RETIRE_LOCK_OMIT;
    child = record_first_side_child(L, J, root, &childno);
    assert_published_child(
	L, J, root, rootno, child, childno, registered);
    assert(trace_gdbjit_entry_acq(root) == root_entry);
  }

  if (descriptor_held)
    lj_gdbjit_test_descriptor_lock_release();
  assert_side_quiescent(L, J);
  lj_gdbjit_test_stats(&stats);
  assert_side_stats(which, &stats);

  if (rollback_case) {
    lj_gdbjit_test_reset();
    child = record_first_side_child(L, J, root, &childno);
    assert(childno == rollback_slot);
    assert_published_child(L, J, root, rootno, child, childno, 1);
    lj_gdbjit_test_stats(&stats);
    assert_side_stats(GDBJIT_SIDE_SUCCESS_SCOPED, &stats);
  }

  if (which == GDBJIT_SIDE_SUCCESS_SCOPED) {
    assert(lj_trace_flushscope(J, childno) == 1u);
    assert(trace_gdbjit_entry_acq(child) == NULL);
    assert(trace_gdbjit_entry_acq(root) == root_entry);
    assert(trace_nchild_acq(root) == 0);
    assert(trace_nextside_acq(root) == 0);
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 2);
    assert(stats.register_callbacks_ready == 1);
    assert(call_first_side_pair(L, 3, 0) == 3);
  } else if (which == GDBJIT_SIDE_SUCCESS_FULL_FLUSH) {
    assert(lj_trace_flushall_gc(L) == 0);
    assert(trace_gdbjit_entry_acq(child) == NULL);
    assert(trace_gdbjit_entry_acq(root) == NULL);
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 3);
    assert(stats.register_callbacks_ready == 1);
  } else if (which == GDBJIT_SIDE_RETIRE_LOCK_OMIT) {
    uint64_t mature_epoch, retire_stamp;
    assert(lj_gdbjit_test_descriptor_lock_acquire() == 1);
    assert(lj_trace_flushscope(J, childno) == 1u);
    assert(trace_gdbjit_entry_acq(child) != NULL);
    assert(trace_retired_link_listed_acq(child));
    retire_stamp = la_load64_acq(&child->retire_epoch);
    assert(retire_stamp != 0 && retire_stamp != UINT64_MAX);
    mature_epoch = retire_stamp-1u+LJ_FLUSH_EPOCHS;
    assert(trace_nchild_acq(root) == 0);
    assert(trace_nextside_acq(root) == 0);
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 1);
    assert(reclaim_trace_at(g, mature_epoch) == 0);
    assert(trace_gdbjit_entry_acq(child) != NULL);
    assert(trace_retired_link_listed_acq(child));
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 1);
    lj_gdbjit_test_descriptor_lock_release();
    assert(reclaim_trace_at(g, mature_epoch) >= 1u);
    assert(traceref_safe(J, childno) == NULL);
    assert(trace_retired_head_acq(J) == NULL);
    assert(trace_gdbjit_entry_acq(root) == root_entry);
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 2);
    assert(stats.register_callbacks_ready == 1);
    lua_close(L);
    closed = 1;
    lj_gdbjit_test_stats(&stats);
    assert(stats.register_callbacks == 3);
    assert(stats.register_callbacks_ready == 1);
  }

  if (!closed)
    lua_close(L);
}

static void run_unsupported_side_case(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  GCproto *pt;
  GCtrace *root = NULL;
  TraceNo rootno = 0;
  LJGDBJITTestStats stats;
  unsigned attempt;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n");
  ljt_lua_assert_ok(L,
    luaL_loadbuffer(L, fixture_lua, sizeof(fixture_lua)-1u,
	"@gdbjit-unsupported-side.lua"),
    "load GDBJIT unsupported-side fixture");
  ljt_lua_pcall(L, 0, 0, "install GDBJIT unsupported-side fixture");
  pt = named_proto(L, "__gdbjit_unsupported_side");
  for (attempt = 0; attempt < 64; attempt++) {
    assert(call_named_pair(L, "__gdbjit_unsupported_side", 4, 0) == 6);
    rootno = proto_trace_acq(pt);
    if (rootno != 0) {
      root = traceref_safe(J, rootno);
      if (trace_runnable_acq(root, rootno))
	break;
    }
  }
  assert(root != NULL && trace_gdbjit_entry_acq(root) != NULL);
  lj_gdbjit_test_reset();
  for (attempt = 0; attempt < 8; attempt++)
    assert(call_named_pair(L, "__gdbjit_unsupported_side", 7, 0) == 9);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_nchild_acq(root) == 0);
  assert_side_quiescent(L, J);
  lj_gdbjit_test_stats(&stats);
  assert(stats.prepare_attempts == 0);
  assert(stats.prepare_successes == 0);
  assert(stats.prepare_bounds_omits == 0);
  assert(stats.prepare_alloc_omits == 0);
  assert(stats.commit_attempts == 0);
  assert(stats.commit_successes == 0);
  assert(stats.commit_lock_omits == 0);
  assert(stats.aborts == 0);
  assert(stats.aborts_after_token == 0);
  assert(stats.register_callbacks == 0);
  assert(stats.register_callbacks_ready == 0);
  lua_close(L);
}

int main(void)
{
  run_case(GDBJIT_CASE_NORMAL);
  run_case(GDBJIT_CASE_MEDIUM_NAME);
  run_case(GDBJIT_CASE_ALLOC_OMIT);
  run_case(GDBJIT_CASE_LOCK_OMIT);
  run_case(GDBJIT_CASE_LONG_NAME);
  run_side_case(GDBJIT_SIDE_SUCCESS_SCOPED);
  run_side_case(GDBJIT_SIDE_ALLOC_OMIT);
  run_side_case(GDBJIT_SIDE_LOCK_OMIT);
  run_side_case(GDBJIT_SIDE_POST_PREPARE_ROLLBACK);
  run_side_case(GDBJIT_SIDE_POST_PREPARE_EXTERNAL_ERROR);
  run_side_case(GDBJIT_SIDE_SUCCESS_FULL_FLUSH);
  run_side_case(GDBJIT_SIDE_RETIRE_LOCK_OMIT);
  run_unsupported_side_case();
  puts("t-arm64-jit-gdbjit-prepare OK");
  return 0;
}
