/*
** Focused regression test for os.setlocale() mutation during mt_entering.
*/

#include <assert.h>
#include <limits.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"

#include "lib/lua_fixture_helpers.h"

static void call_setlocale(lua_State *L, const char *locale,
			   int expect_success)
{
  int status;
  lua_getglobal(L, "os");
  lua_getfield(L, -1, "setlocale");
  if (locale)
    lua_pushstring(L, locale);
  else
    lua_pushnil(L);
  lua_pushliteral(L, "all");
  status = lua_pcall(L, 2, 1, 0);
  if (expect_success) {
    ljt_lua_assert_ok(L, status, "os.setlocale");
    assert(lua_isnil(L, -1) || lua_isstring(L, -1));
  } else {
    const char *err;
    assert(status != LUA_OK);
    err = lua_tostring(L, -1);
    assert(err != NULL);
    assert(strstr(err, "os.setlocale mutation disabled") != NULL);
  }
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);

  call_setlocale(L, "C", 1);
  call_setlocale(L, NULL, 1);

  assert(mt_live_acq(g) == 0);
  assert(mt_entering_add_rlx(g, 1) == 0);
  call_setlocale(L, NULL, 1);
  call_setlocale(L, "C", 0);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);

  call_setlocale(L, "C", 1);
  lua_close(L);
  return 0;
}
