/* A first generated callback must start above the complete XSAVE frame. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

typedef int32_t (*StackCallback)(int32_t);

static TGState *test_tg;
static int enabled, stress_mode;
static uint32_t entries, returns, callbacks, suspended;
static uint32_t stale_geometry, relocations, throws;

static int32_t invoke(StackCallback callback, int32_t value);

static int active_generated(LJFFINativeFrameSnapshot *snapshot)
{
  LJFFINativeFrameSnapshotResult result =
    lj_ffi_native_frame_snapshot(test_tg, snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return 0;
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot->depth == 1);
  assert(lj_ffi_native_frame_func_acq(&snapshot->frame[0]) == (void *)invoke);
  assert((lj_ffi_native_frame_flags_acq(&snapshot->frame[0]) &
          LJ_FFI_NATIVE_FRAME_F_ACTIVE) != 0);
  assert(lj_tg_in_native_acq(test_tg) == 1);
  return 1;
}

static int frame_geometry_current(const LJFFINativeFrame *frame)
{
  lua_State *L = lj_tg_load_cur_L(test_tg);
  assert(L == lj_ffi_native_frame_L_acq(frame));
  return L->base == restorestack(L,
           lj_ffi_native_frame_base_offset_acq(frame)) &&
         L->top == restorestack(L,
           lj_ffi_native_frame_top_offset_acq(frame));
}

static int32_t invoke(StackCallback callback, int32_t value)
{
  LJFFINativeFrameSnapshot snapshot;
  int generated = active_generated(&snapshot);
  int32_t result;
  if (generated) {
    entries++;
    if (!frame_geometry_current(&snapshot.frame[0]))
      stale_geometry++;
  }
  if (enabled) {
    if (generated) callbacks++;
    result = callback(value);
  } else {
    result = value + 1;
  }
  if (generated) {
    assert(active_generated(&snapshot));
    if (!frame_geometry_current(&snapshot.frame[0]))
      stale_geometry++;
    returns++;
  }
  return result;
}

static int enable_callbacks(lua_State *L)
{
  UNUSED(L);
  enabled = 1;
  entries = returns = callbacks = suspended = 0;
  return 0;
}

static int callback_mark(lua_State *L)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result =
    lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return 0;
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1);
  assert(lj_ffi_native_frame_func_acq(&snapshot.frame[0]) == (void *)invoke);
  assert((lj_ffi_native_frame_flags_acq(&snapshot.frame[0]) &
          LJ_FFI_NATIVE_FRAME_F_SUSPENDED) != 0);
  assert(lj_tg_load_jit_base(test_tg) == NULL);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  suspended++;
  if (stress_mode == 1 && suspended == 1) {
    TValue *oldstack = tvref(L->stack);
    assert(lua_checkstack(L, 8192));
    relocations += oldstack != tvref(L->stack);
    assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  } else if (stress_mode == 2) {
    throws++;
    return luaL_error(L, "generated callback stack unwind");
  }
  return 0;
}

static void assert_clean(lua_State *L)
{
  jit_State *J = G2J(G(L));
  TraceNo no;
  assert(lj_ffi_native_frame_depth_acq(test_tg) == 0);
  assert((lj_ffi_native_frame_sequence_acq(test_tg) & 1u) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_tg_load_jit_base(test_tg) == NULL);
  assert(lj_tg_ffi_call_func_acq(test_tg) == NULL);
  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  assert(ccallback_slot_acq(&test_tg->cb) == 0);
  for (no = 1; no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T) assert(trace_native_pins_acq(T) == 0);
  }
}

static void run_case(int mode)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  test_tg = L2TG(L);
  enabled = 0;
  stress_mode = mode;
  entries = returns = callbacks = suspended = 0;
  stale_geometry = relocations = throws = 0;
  lua_pushlightuserdata(L, (void *)invoke);
  lua_setglobal(L, "stack_invoke_ptr");
  lua_pushcfunction(L, enable_callbacks);
  lua_setglobal(L, "stack_enable_callbacks");
  lua_pushcfunction(L, callback_mark);
  lua_setglobal(L, "stack_callback_mark");
  lua_pushboolean(L, mode == 2);
  lua_setglobal(L, "stack_expect_error");
  /* Starting at zero skips the first body before the hot loop records. Thus
  ** no interpreted FFI call happens to extend L->top over the numeric-for
  ** control slots. The constant step is legitimately absent from the DONE
  ** post-call snapshot; callback setup must preserve its original stack slot. */
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local p = ffi.new('struct { double x; double y; }', 0, 1)\n"
    "local fn = ffi.cast('int(*)(int(*)(int), int)', stack_invoke_ptr)\n"
    "local function body(i) stack_callback_mark(); return i+1 end\n"
    "jit.off(body)\n"
    "local cb = ffi.cast('int(*)(int)', body)\n"
    "local function run(p, n, fn, cb)\n"
    "  local s = 0\n"
    "  for i = 0, n do\n"
    "    if i > 0 then p.x=i; s=s+p.x+p.y+fn(cb,i) end\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local exits = 0\n"
    "local function onexit() exits=exits+1 end\n"
    "jit.off(onexit)\n"
    "local function check()\n"
    "  jit.opt.start('hotloop=1', 'hotexit=1000')\n"
    "  jit.attach(onexit, 'texit')\n"
    "  assert(run(p,80,fn,cb)==6640 and exits>0)\n"
    "  stack_enable_callbacks(); exits=0\n"
    "  if stack_expect_error then\n"
    "    local ok, err=pcall(run,p,80,fn,cb)\n"
    "    assert(not ok and err:find('generated callback stack unwind',1,true))\n"
    "  else\n"
    "    local got=run(p,80,fn,cb)\n"
    "    assert(got==6640, 'callback changed the live numeric-for step: '..got)\n"
    "    assert(exits>0, 'callback run did not leave native code')\n"
    "  end\n"
    "  jit.attach(onexit); cb:free()\n"
    "end\n"
    "jit.off(check); check()\n");
  assert(entries != 0 && callbacks != 0 && suspended != 0);
  assert(stale_geometry == 0);
  if (mode == 2) {
    assert(entries == 1 && returns == 0 && throws == 1);
  } else {
    assert(entries == returns && callbacks == suspended && throws == 0);
  }
  if (mode == 1) assert(relocations != 0);
  assert_clean(L);
  printf("callback stack mode=%d generated=%u callbacks=%u relocated=%u throws=%u\n",
         mode, entries, callbacks, relocations, throws);
  lua_close(L);
}

int main(void)
{
  int mode;
  for (mode = 0; mode < 3; mode++) run_case(mode);
  return 0;
}
