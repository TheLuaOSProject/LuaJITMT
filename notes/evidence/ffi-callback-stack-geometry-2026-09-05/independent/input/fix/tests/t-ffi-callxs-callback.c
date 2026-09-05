/* Authentic generated CALLXS callback suspension and unwind regression. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_ctype.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_XSAVE_TEST_HELPERS
#error "t-ffi-callxs-callback requires LJ_XSAVE_TEST_HELPERS"
#endif
#define NATIVE_FRAME_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

typedef int32_t (*AuthCallback)(int32_t);

static TGState *test_tg;
static int callbacks_enabled;
static int throw_on_generated;
static int current_outer_generated;
static uint32_t outer_effects;
static uint32_t generated_outer_entries;
static uint32_t generated_outer_returns;
static uint32_t bool_outer_effects;
static uint32_t generated_bool_entries;
static uint32_t generated_bool_returns;
static uint32_t generated_bool_callback_returns;
static uint32_t suspended_observations;
static uint32_t nested_generated_entries;
static uint32_t nested_interpreted_entries;
static uint32_t nested_interpreted_returns;
static uint32_t nested_callback_observations;
static uint32_t callback_markers;
static GCcdata *current_outer_result_root;

static int64_t callback_outer(AuthCallback cb, int32_t value);
static _Bool callback_bool_outer(AuthCallback cb, int32_t value);
static int64_t nested_leaf(int32_t value);
static int32_t interpreted_callback_call(AuthCallback cb, int32_t value);

static uint32_t frame_state(const LJFFINativeFrame *frame)
{
  return lj_ffi_native_frame_flags_acq(frame) & NATIVE_FRAME_STATE;
}

static GCcdata *assert_active_top(const LJFFINativeFrameSnapshot *snapshot,
				  void *func, int callback_seen,
				  int boxed_result)
{
  const LJFFINativeFrame *frame;
  GCcdata *result_root;
  lua_State *L;
  TValue *stack, *jitbase;
  uint64_t offset;
  uint32_t flags;

  assert(snapshot != NULL && snapshot->depth != 0);
  frame = &snapshot->frame[snapshot->depth - 1u];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert(frame_state(frame) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(((flags & LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN) != 0) ==
	 callback_seen);
  assert(lj_ffi_native_frame_func_acq(frame) == func);
  assert(lj_ffi_native_frame_trace_acq(frame) != NULL);
  assert(lj_ffi_native_frame_trace_no_acq(frame) != 0);
  result_root = lj_ffi_native_frame_result_root_acq(frame);
  assert((result_root != NULL) == boxed_result);
  L = lj_ffi_native_frame_L_acq(frame);
  assert(L != NULL && L == lj_tg_load_cur_L(test_tg));
  assert(lj_tg_in_native_acq(test_tg) == 1u);
  assert(lj_tg_vmstate_load_acq(test_tg) ==
	 (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  stack = mref_acq(L->stack, TValue);
  offset = lj_ffi_native_frame_jit_base_offset_acq(frame);
  assert(stack != NULL && (offset % sizeof(TValue)) == 0);
  jitbase = (TValue *)(void *)((char *)stack + (uintptr_t)offset);
  assert(lj_tg_load_jit_base(test_tg) == jitbase);
  return result_root;
}

static int snapshot_outer_suspended(LJFFINativeFrameSnapshot *snapshot)
{
  const LJFFINativeFrame *frame;
  uint32_t flags;
  LJFFINativeFrameSnapshotResult result =
    lj_ffi_native_frame_snapshot(test_tg, snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return 0;
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot->depth == 1u);
  frame = &snapshot->frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert(frame_state(frame) == LJ_FFI_NATIVE_FRAME_F_SUSPENDED);
  assert((flags & (LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
		   LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN)) ==
	 (LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
	  LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN));
  assert(lj_ffi_native_frame_func_acq(frame) == (void *)callback_outer);
  assert(lj_ffi_native_frame_result_root_acq(frame) != NULL);
  if (current_outer_result_root)
    assert(lj_ffi_native_frame_result_root_acq(frame) ==
	   current_outer_result_root);
  assert(lj_ffi_native_frame_L_acq(frame) ==
	 lj_tg_load_cur_L(test_tg));
  assert(lj_tg_load_jit_base(test_tg) == NULL);
  return 1;
}

static int callback_mark(lua_State *L)
{
  LJFFINativeFrameSnapshot snapshot;
  MSize depth;
  assert(L2TG(L) == test_tg);
  callback_markers++;
  if (current_outer_generated) {
    assert(snapshot_outer_suspended(&snapshot));
    depth = ccallback_depth_acq(&test_tg->cb);
    assert(depth != 0);
    assert(test_tg->cb.frame[depth - 1u].native_frame_depth == 1u);
    suspended_observations++;
    assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
    assert(snapshot_outer_suspended(&snapshot));
  }
  return 0;
}

static int inner_callback_mark(lua_State *L)
{
  LJFFINativeFrameSnapshot snapshot;
  MSize depth;
  assert(L2TG(L) == test_tg);
  if (current_outer_generated) {
    assert(snapshot_outer_suspended(&snapshot));
    depth = ccallback_depth_acq(&test_tg->cb);
    assert(depth >= 2u);
    assert(test_tg->cb.frame[depth - 2u].native_frame_depth == 1u);
    assert(test_tg->cb.frame[depth - 1u].native_frame_depth == 0u);
    nested_callback_observations++;
  }
  return 0;
}

static int callback_should_throw(lua_State *L)
{
  int should_throw = throw_on_generated && current_outer_generated;
  if (should_throw)
    throw_on_generated = 0;
  lua_pushboolean(L, should_throw);
  return 1;
}

static int64_t nested_leaf(int32_t value)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE) {
    const LJFFINativeFrame *top = &snapshot.frame[snapshot.depth - 1u];
    if (snapshot.depth == 2u) {
      const LJFFINativeFrame *outer = &snapshot.frame[0];
      assert(frame_state(outer) == LJ_FFI_NATIVE_FRAME_F_SUSPENDED);
      assert((lj_ffi_native_frame_flags_acq(outer) &
	      LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN) != 0);
      assert(lj_ffi_native_frame_func_acq(outer) ==
	     (void *)callback_outer);
      assert(lj_ffi_native_frame_result_root_acq(outer) ==
	     current_outer_result_root);
      assert(assert_active_top(&snapshot, (void *)nested_leaf, 0, 1) !=
	     current_outer_result_root);
      nested_generated_entries++;
    } else if (snapshot.depth == 1u &&
	       frame_state(top) == LJ_FFI_NATIVE_FRAME_F_ACTIVE) {
      (void)assert_active_top(&snapshot, (void *)nested_leaf, 0, 1);
    } else {
      /* An interpreter fallback inside the outer callback legitimately sees
      ** only its older SUSPENDED continuation. */
      assert(snapshot.depth == 1u);
      assert(frame_state(top) == LJ_FFI_NATIVE_FRAME_F_SUSPENDED);
      assert(lj_ffi_native_frame_func_acq(top) == (void *)callback_outer);
    }
  } else {
    assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  }
  return value + 7;
}

static int32_t interpreted_callback_call(AuthCallback cb, int32_t value)
{
  LJFFINativeFrameSnapshot snapshot;
  int suspended = snapshot_outer_suspended(&snapshot);
  int32_t result;
  if (suspended) {
    assert(lj_tg_in_native_acq(test_tg) != 0);
    nested_interpreted_entries++;
  }
  assert(cb != NULL);
  result = cb(value);
  if (suspended) {
    assert(snapshot_outer_suspended(&snapshot));
    assert(lj_tg_in_native_acq(test_tg) != 0);
    nested_interpreted_returns++;
  }
  return result + 30;
}

static int64_t callback_outer(AuthCallback cb, int32_t value)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult snapshot_result;
  int generated = 0;
  int32_t call_result;

  snapshot_result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (snapshot_result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE) {
    assert(snapshot.depth == 1u);
    current_outer_result_root = assert_active_top(
	&snapshot, (void *)callback_outer, 0, 1);
    generated = 1;
    generated_outer_entries++;
  } else {
    assert(snapshot_result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  }

  outer_effects++;
  assert(cb != NULL);
  if (callbacks_enabled) {
    current_outer_generated = generated;
    call_result = cb(value);
    current_outer_generated = 0;
  } else {
    call_result = value + 10;
  }

  if (generated) {
    assert(lj_ffi_native_frame_snapshot(test_tg, &snapshot) ==
	   LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
    assert(snapshot.depth == 1u);
    assert(assert_active_top(&snapshot, (void *)callback_outer,
		      callbacks_enabled != 0, 1) ==
	   current_outer_result_root);
    generated_outer_returns++;
  } else {
    assert(lj_ffi_native_frame_snapshot(test_tg, &snapshot) ==
	   LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  }
  if (generated)
    current_outer_result_root = NULL;
  return (int64_t)call_result + 100;
}

static _Bool callback_bool_outer(AuthCallback cb, int32_t value)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult snapshot_result;
  int generated = 0;
  int32_t call_result;

  snapshot_result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (snapshot_result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE) {
    assert(snapshot.depth == 1u);
    assert(assert_active_top(&snapshot, (void *)callback_bool_outer, 0, 0) ==
	   NULL);
    generated = 1;
    generated_bool_entries++;
  } else {
    assert(snapshot_result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  }

  bool_outer_effects++;
  assert(cb != NULL);
  call_result = callbacks_enabled ? cb(value) : value;

  if (generated) {
    assert(lj_ffi_native_frame_snapshot(test_tg, &snapshot) ==
	   LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
    assert(snapshot.depth == 1u);
    assert(assert_active_top(&snapshot, (void *)callback_bool_outer,
			      callbacks_enabled != 0, 0) == NULL);
    generated_bool_returns++;
    if (callbacks_enabled)
      generated_bool_callback_returns++;
  } else {
    assert(lj_ffi_native_frame_snapshot(test_tg, &snapshot) ==
	   LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  }
  return call_result != 0;
}

static void assert_callback_frames_empty(TGState *tg)
{
  MSize i;
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == 0);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  for (i = 0; i < CCALLBACK_MAX_NEST; i++) {
    assert(tg->cb.frame[i].L == NULL);
    assert(tg->cb.frame[i].cont == 0);
    assert(tg->cb.frame[i].native_depth == 0);
    assert(tg->cb.frame[i].native_frame_depth == 0);
    assert(tg->cb.frame[i].auto_detach == 0);
    assert(tg->cb.frame[i].errphase == CCALLBACK_ERR_SETUP);
    assert(tg->cb.frame[i].errnum == 0);
    assert(tg->cb.frame[i].winerr == 0);
  }
}

static void assert_no_trace_pins(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (T != NULL)
      assert(trace_native_pins_acq(T) == 0);
  }
}

static void assert_runtime_clean(lua_State *L, void *old_func,
				 MSize old_slot, uint8_t old_stopreq)
{
  TGState *tg = L2TG(L);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_slot_acq(&tg->cb) == old_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == old_stopreq);
  assert(la_loadptr_acq((void *const *)&tg->ffi_xsave_root) == NULL);
  assert(la_load32_acq(&tg->ffi_xsave_baseslot) == 0);
  assert(la_load32_acq(&tg->ffi_xsave_nslots) == 0);
  assert_callback_frames_empty(tg);
  assert_no_trace_pins(G2J(G(L)));
}

static lua_Integer call_callback_run(lua_State *L, int n, int seed,
				     int expect_ok)
{
  int status;
  lua_Integer result = 0;
  lua_getglobal(L, "__callxs_callback_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, seed);
  status = lua_pcall(L, 2, 1, 0);
  if (expect_ok) {
    assert(status == LUA_OK);
    assert(lua_isnumber(L, -1));
    result = lua_tointeger(L, -1);
  } else {
    assert(status != LUA_OK);
    assert(lua_tostring(L, -1) != NULL);
    assert(strstr(lua_tostring(L, -1),
		  "authentic generated callback error") != NULL);
  }
  lua_pop(L, 1);
  return result;
}

static int call_callback_bool_run(lua_State *L, int n, int value)
{
  int status, result;
  lua_getglobal(L, "__callxs_callback_bool_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, value);
  status = lua_pcall(L, 2, 1, 0);
  assert(status == LUA_OK);
  assert(lua_isboolean(L, -1));
  result = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return result;
}

int main(void)
{
#if !LJ_TARGET_X64
  printf("t-ffi-callxs-callback SKIP: x64-only lowering\n");
  return 0;
#else
  lua_State *L = ljt_lua_newstate_openlibs();
  TGState *tg = L2TG(L);
  void *old_func = lj_tg_ffi_call_func_acq(tg);
  MSize old_slot = ccallback_slot_acq(&tg->cb);
  uint8_t old_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  uint32_t effects0, markers0, entries0, returns0;
  uint32_t suspended0, nested0, interp0, interp_ret0, inner_cb0;
  uint32_t bool_effects0, bool_entries0, bool_returns0, bool_callbacks0;
  lua_Integer got, expected;
  const int n = 16, seed = 9;

  test_tg = tg;
  assert(tg != NULL && lj_ffi_native_frame_depth_acq(tg) == 0);
  lua_pushlightuserdata(L, (void *)callback_outer);
  lua_setglobal(L, "lj_callxs_callback_outer_ptr");
  lua_pushlightuserdata(L, (void *)callback_bool_outer);
  lua_setglobal(L, "lj_callxs_callback_bool_outer_ptr");
  lua_pushlightuserdata(L, (void *)nested_leaf);
  lua_setglobal(L, "lj_callxs_nested_leaf_ptr");
  lua_pushlightuserdata(L, (void *)interpreted_callback_call);
  lua_setglobal(L, "lj_callxs_interpreted_callback_ptr");
  lua_pushcfunction(L, callback_mark);
  lua_setglobal(L, "lj_callxs_callback_mark");
  lua_pushcfunction(L, inner_callback_mark);
  lua_setglobal(L, "lj_callxs_inner_callback_mark");
  lua_pushcfunction(L, callback_should_throw);
  lua_setglobal(L, "lj_callxs_callback_should_throw");

  /* The outer function is deliberately recorded without invoking its callback.
  ** Callback entry aborts an in-progress recording, so this warm-up is the
  ** exact way to exercise callback reentry from already-published CALLXS. */
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int32_t (*lj_callxs_callback_t)(int32_t);\n"
    "typedef int64_t (*lj_callxs_outer_t)(lj_callxs_callback_t, int32_t);\n"
    "typedef _Bool (*lj_callxs_bool_outer_t)(lj_callxs_callback_t, int32_t);\n"
    "typedef int64_t (*lj_callxs_leaf_t)(int32_t);\n"
    "typedef int32_t (*lj_callxs_inner_t)(lj_callxs_callback_t, int32_t);\n"
    "]]\n"
    "local outer = ffi.cast('lj_callxs_outer_t',\n"
    "                       lj_callxs_callback_outer_ptr)\n"
    "local bool_outer = ffi.cast('lj_callxs_bool_outer_t',\n"
    "                            lj_callxs_callback_bool_outer_ptr)\n"
    "local leaf = ffi.cast('lj_callxs_leaf_t', lj_callxs_nested_leaf_ptr)\n"
    "local inner = ffi.cast('lj_callxs_inner_t',\n"
    "                       lj_callxs_interpreted_callback_ptr)\n"
    "local function nested_run(x)\n"
    "  local s = 0\n"
    "  for i = 1, 6 do s = s + tonumber(leaf(x + i)) end\n"
    "  return s\n"
    "end\n"
    "local function cb2_body(x)\n"
    "  lj_callxs_inner_callback_mark()\n"
    "  return x + 2\n"
    "end\n"
    "jit.off(cb2_body)\n"
    "local cb2 = ffi.cast('lj_callxs_callback_t', cb2_body)\n"
    "local function bool_cb_body(x) return x end\n"
    "jit.off(bool_cb_body)\n"
    "local bool_cb = ffi.cast('lj_callxs_callback_t', bool_cb_body)\n"
    "local function cb1_body(x)\n"
    "  local churn = { x, tostring(x) }\n"
    "  assert(churn[1] == x and #churn[2] > 0)\n"
    "  lj_callxs_callback_mark()\n"
    "  if lj_callxs_callback_should_throw() then\n"
    "    error('authentic generated callback error')\n"
    "  end\n"
    "  return nested_run(x) + inner(cb2, x)\n"
    "end\n"
    "jit.off(cb1_body)\n"
    "local cb1 = ffi.cast('lj_callxs_callback_t', cb1_body)\n"
    "function __callxs_callback_run(n, seed)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + tonumber(outer(cb1, seed + i))\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "function __callxs_callback_bool_run(n, value)\n"
    "  local result\n"
    "  for _ = 1, n do result = bool_outer(bool_cb, value) end\n"
    "  assert(type(result) == 'boolean')\n"
    "  return result\n"
    "end\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "for x = 1, 80 do assert(nested_run(x) == 6*x + 63) end\n"
    "for seed = 1, 40 do\n"
    "  local n = 80\n"
    "  assert(__callxs_callback_run(n, seed) ==\n"
    "         n*seed + n*(n+1)/2 + 110*n)\n"
    "end\n"
    "assert(__callxs_callback_bool_run(200, 1) == true)\n"
    "__callxs_callback_keep = { outer, bool_outer, leaf, inner, nested_run }\n"
    "__callxs_callback_cb1 = cb1\n"
    "__callxs_callback_cb2 = cb2\n"
    "__callxs_callback_bool_cb = bool_cb\n");

  assert(generated_outer_entries != 0);
  assert(generated_outer_entries == generated_outer_returns);
  assert(generated_bool_entries != 0);
  assert(generated_bool_entries == generated_bool_returns);
  assert_runtime_clean(L, old_func, old_slot, old_stopreq);

  callbacks_enabled = 1;
  /* The trace specialized the result as true during callback-free warm-up.
  ** Reenter Lua and return false now: callback_seen forces native leave before
  ** the value guard, whose DONE snapshot must restore a real Lua false. */
  bool_effects0 = bool_outer_effects;
  bool_entries0 = generated_bool_entries;
  bool_returns0 = generated_bool_returns;
  bool_callbacks0 = generated_bool_callback_returns;
  assert(call_callback_bool_run(L, n, 0) == 0);
  assert(bool_outer_effects - bool_effects0 == (uint32_t)n);
  assert(generated_bool_entries - bool_entries0 != 0);
  assert(generated_bool_entries - bool_entries0 ==
	 generated_bool_returns - bool_returns0);
  assert(generated_bool_callback_returns - bool_callbacks0 ==
	 generated_bool_returns - bool_returns0);
  assert_runtime_clean(L, old_func, old_slot, old_stopreq);

  effects0 = outer_effects;
  markers0 = callback_markers;
  entries0 = generated_outer_entries;
  returns0 = generated_outer_returns;
  suspended0 = suspended_observations;
  nested0 = nested_generated_entries;
  interp0 = nested_interpreted_entries;
  interp_ret0 = nested_interpreted_returns;
  inner_cb0 = nested_callback_observations;
  got = call_callback_run(L, n, seed, 1);
  expected = 7 * ((lua_Integer)n * seed +
		  (lua_Integer)n * (n + 1) / 2) + 195 * n;
  assert(got == expected);
  assert(outer_effects - effects0 == (uint32_t)n);
  assert(callback_markers - markers0 == (uint32_t)n);
  assert(generated_outer_entries - entries0 != 0);
  assert(generated_outer_entries - entries0 ==
	 generated_outer_returns - returns0);
  assert(suspended_observations - suspended0 != 0);
  assert(nested_generated_entries - nested0 != 0);
  assert(nested_interpreted_entries - interp0 != 0);
  assert(nested_interpreted_entries - interp0 ==
	 nested_interpreted_returns - interp_ret0);
  assert(nested_callback_observations - inner_cb0 != 0);
  assert_runtime_clean(L, old_func, old_slot, old_stopreq);

  /* Initial interpreter iterations may precede the loop root. Arm the error
  ** only for the first callback actually entered by generated outer CALLXS.
  ** Exactly one generated entry and no generated return proves the foreign
  ** effect was neither replayed nor allowed to run past the unwind edge. */
  effects0 = outer_effects;
  markers0 = callback_markers;
  entries0 = generated_outer_entries;
  returns0 = generated_outer_returns;
  throw_on_generated = 1;
  (void)call_callback_run(L, 64, 100, 0);
  assert(throw_on_generated == 0);
  assert(current_outer_generated == 1);
  assert(outer_effects - effects0 == callback_markers - markers0);
  assert(outer_effects - effects0 != 0);
  assert(generated_outer_entries - entries0 == 1u);
  assert(generated_outer_returns - returns0 == 0u);
  current_outer_generated = 0;
  current_outer_result_root = NULL;
  assert_runtime_clean(L, old_func, old_slot, old_stopreq);

  ljt_lua_dostring(L,
    "__callxs_callback_cb1:free()\n"
    "__callxs_callback_cb2:free()\n"
    "__callxs_callback_bool_cb:free()\n"
    "__callxs_callback_cb1 = nil\n"
    "__callxs_callback_cb2 = nil\n"
    "__callxs_callback_bool_cb = nil\n"
    "__callxs_callback_keep = nil\n"
    "__callxs_callback_run = nil\n"
    "__callxs_callback_bool_run = nil\n");
  lua_close(L);
  printf("t-ffi-callxs-callback OK: generated callbacks suspend, nest and unwind exactly\n");
  return 0;
#endif
}
