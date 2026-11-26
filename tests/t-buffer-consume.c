/* Hold one get() at string interning while another actor consumes or resets. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

enum { IDLE, ARMED, COPYING, RELEASED };
static uint32_t stage;
static lua_State *main_L;

GCstr *__real_lj_str_new(lua_State *L, const char *p, size_t n);
GCstr *__wrap_lj_str_new(lua_State *L, const char *p, size_t n)
{
  if (L != main_L && la_load32_acq(&stage) == ARMED &&
      n == 4 && memcmp(p, "old!", 4) == 0) {
    LJNativeFrame frame;
    lj_native_enter_l(L, &frame);
    la_store32_rel(&stage, COPYING);
    while (la_load32_acq(&stage) == COPYING)
      sched_yield();
    (void)lj_native_leave_l(L, &frame);
  }
  return __real_lj_str_new(L, p, n);
}

static int arm(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&stage, ARMED);
  return 0;
}

static int wait_copy(lua_State *L)
{
  while (la_load32_acq(&stage) != COPYING)
    (void)lj_thr_retry_yield(L);
  return 0;
}

static int release_copy(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&stage, RELEASED);
  return 0;
}

int main(void)
{
  lua_State *L = main_L = ljt_lua_newstate_openlibs();
  alarm(30);
  lua_register(L, "arm", arm);
  lua_register(L, "wait_copy", wait_copy);
  lua_register(L, "release_copy", release_copy);
  ljt_lua_dostring(L,
    "local th, buf = require('threading'), require('string.buffer')\n"
    "local function read(b) return b:get(4) end\n"
    "for _, op in ipairs({'get', 'skip', 'reset', 'set'}) do\n"
    "  local b = buf.new(64)\n"
    "  if op == 'skip' then b:set('old!keep') else b:put('old!keep') end\n"
    "  arm()\n"
    "  local peer = th.spawn(read, b)\n"
    "  wait_copy()\n"
    "  if op == 'get' then assert(b:get(4) == 'old!')\n"
    "  elseif op == 'skip' then b:skip(4)\n"
    "  elseif op == 'reset' then b:reset():put('new!next')\n"
    "  else b:set('new!next') end\n"
    "  release_copy()\n"
    "  local ok, s = peer:join(10); assert(ok, s)\n"
    "  if op == 'get' or op == 'skip' then\n"
    "    assert(s == 'keep' and #b == 0)\n"
    "  else assert(s == 'new!' and b:get() == 'next') end\n"
    "  b:put('ABCD')\n"
    "  local a,z,c = b:get(1, 0, nil)\n"
    "  assert(a == 'A' and z == '' and c == 'BCD')\n"
    "end\n");
  lua_close(L);
  alarm(0);
  puts("t-buffer-consume OK");
  return 0;
}
