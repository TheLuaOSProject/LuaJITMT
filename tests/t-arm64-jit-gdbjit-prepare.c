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
#include "lj_func.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_gdbjit.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED || \
    !defined(LUAJIT_USE_GDBJIT) || !defined(LJ_GDBJIT_TEST_HELPERS)
#error "fixture requires the closed-side experimental ARM64 GDBJIT build"
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

int main(void)
{
  run_case(GDBJIT_CASE_NORMAL);
  run_case(GDBJIT_CASE_MEDIUM_NAME);
  run_case(GDBJIT_CASE_ALLOC_OMIT);
  run_case(GDBJIT_CASE_LOCK_OMIT);
  run_case(GDBJIT_CASE_LONG_NAME);
  puts("t-arm64-jit-gdbjit-prepare OK");
  return 0;
}
