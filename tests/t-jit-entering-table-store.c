/*
** Focused regression fixture for JIT table-store routing while mt_entering
** is nonzero.
*/

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

static const char jit_entering_store_script[] =
  "package.path = 'src/?.lua;src/jit/?.lua;' .. package.path\n"
  "local util = require('jit.util')\n"
  "jit.on()\n"
  "jit.flush()\n"
  "jit.opt.start('hotloop=1', 'hotexit=1')\n"
  "local a = { 0 }\n"
  "for i = 1, 80 do\n"
  "  a[1] = i + 0.5\n"
  "end\n"
  "assert(a[1] == 80.5)\n"
  "assert(util.traceinfo(1), 'entering array store did not trace')\n"
  "jit.flush()\n"
  "jit.opt.start('hotloop=1', 'hotexit=1')\n"
  "local h = { stable = 0 }\n"
  "for i = 1, 80 do\n"
  "  h.stable = i + 0.5\n"
  "end\n"
  "assert(h.stable == 80.5)\n"
  "assert(util.traceinfo(1), 'entering hash store did not trace')\n";

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(mt_active_acq(g) == 0);
  assert(mt_entering_acq(g) == 0);

  assert(mt_entering_add_rlx(g, 1) == 0);
  if (luaL_dostring(L, jit_entering_store_script) != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lua error");
    assert(0);
  }
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);

  lua_close(L);
  return 0;
}
