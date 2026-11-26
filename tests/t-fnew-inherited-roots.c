/*
** Check inherited upvalue transfer while a sibling redirects the parent.
*/

#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_atomic.h"
#include "lj_func.h"
#include "lj_safepoint.h"
#include "lj_thr.h"

#include "lib/lua_fixture_helpers.h"

static GCfuncL *factory;
static uint32_t armed, paused, released;

static int arm(lua_State *L)
{
  assert(tvisfunc(L->base) && isluafunc(funcV(L->base)));
  factory = &funcV(L->base)->l;
  la_store32_rel(&armed, 1);
  return 0;
}

static void pause_after_capture(lua_State *L, GCfuncL *parent, uint32_t uv)
{
  LJNativeFrame frame;
  if (!la_load32_acq(&armed) || parent != factory || uv != 0)
    return;
  la_store32_rel(&armed, 0);
  lj_native_enter_l(L, &frame);
  la_store32_rel(&paused, 1);
  while (!la_load32_acq(&released))
    sched_yield();
  (void)lj_native_leave_l(L, &frame);
}

static int wait_capture(lua_State *L)
{
  while (!la_load32_acq(&paused))
    (void)lj_thr_retry_yield(L);
  return 0;
}

static int release_capture(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&released, 1);
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  alarm(30);
  lua_register(L, "arm_capture", arm);
  lua_register(L, "wait_capture", wait_capture);
  lua_register(L, "release_capture", release_capture);
  lj_func_test_set_captured_hook(pause_after_capture);
  ljt_lua_dostring(L,
    "local th = require('threading')\n"
    "jit.off(); jit.flush()\n"
    "local weak = setmetatable({}, {__mode='v'})\n"
    "local function build()\n"
    "  local first, second, replacement = {label='old'}, 17, {label='new'}\n"
    "  weak[1] = first\n"
    "  return function() return function() return first, second end end,\n"
    "    function() return replacement end\n"
    "end\n"
    "local factory, donor = build()\n"
    "local function check()\n"
    "  arm_capture(factory)\n"
    "  local task = th.spawn(factory)\n"
    "  wait_capture()\n"
    "  debug.upvaluejoin(factory, 1, donor, 1)\n"
    "  local function drop() weak[2] = {} end\n"
    "  drop(); collectgarbage()\n"
    "  assert(weak[2] == nil and weak[1].label == 'old')\n"
    "  release_capture()\n"
    "  local ok, child = task:join(10); assert(ok, child)\n"
    "  local value, second = child()\n"
    "  assert(value == weak[1] and second == 17)\n"
    "  assert(factory()() == donor())\n"
    "end\n"
    "check(); collectgarbage(); collectgarbage()\n"
    "assert(weak[1] == nil, 'constructor retained the replaced cell')\n");
  lj_func_test_set_captured_hook(NULL);
  lua_close(L);
  alarm(0);
  puts("t-fnew-inherited-roots OK");
  return 0;
}
