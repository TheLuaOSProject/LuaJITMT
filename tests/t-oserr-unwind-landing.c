/*
** OS error-state preservation across an external-unwind final landing.
*/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_err.h"

#ifndef LJ_OSERR_TEST_UNWIND_CLOBBER
#error "t-oserr-unwind-landing requires LJ_OSERR_TEST_UNWIND_CLOBBER"
#endif

#if !LJ_UNWIND_EXT
#error "t-oserr-unwind-landing requires external unwinding"
#endif

static int throw_with_errno(lua_State *L)
{
  lua_pushliteral(L, "probe");
  errno = EDOM;
  lj_err_throw(L, LUA_ERRRUN);
  return 0;
}

static int test_c_landing(lua_State *L)
{
  int status, observed;
  lua_settop(L, 0);
  lua_pushcfunction(L, throw_with_errno);
  errno = 0;
  status = lua_pcall(L, 0, 0, 0);
  observed = errno;
  if (status != LUA_ERRRUN || observed != EDOM) {
    fprintf(stderr, "external C unwind: status=%d errno=%d expected=%d\n",
	    status, observed, EDOM);
    return 1;
  }
  return 0;
}

static int test_fast_function_landing(lua_State *L, int pass)
{
  const char *message;
  int status, observed;
  lua_settop(L, 0);
  if (luaL_loadstring(L, "return pcall(lj_oserr_throw)") != LUA_OK) {
    fprintf(stderr, "external fast-function unwind: load failed: %s\n",
	    lua_tostring(L, -1));
    return 1;
  }
  errno = 0;
  status = lua_pcall(L, 0, 2, 0);
  observed = errno;
  message = status == LUA_OK && lua_gettop(L) == 2 ?
    lua_tostring(L, -1) : NULL;
  if (status != LUA_OK || lua_gettop(L) != 2 ||
      !lua_isboolean(L, -2) || lua_toboolean(L, -2) ||
      message == NULL || strcmp(message, "probe") != 0 || observed != EDOM) {
    fprintf(stderr,
	"external fast-function unwind pass %d: status=%d results=%d "
	"errno=%d expected=%d\n",
	pass, status, lua_gettop(L), observed, EDOM);
    return 1;
  }
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  int failed;
  if (L == NULL)
    return 2;
  luaL_openlibs(L);
  failed = test_c_landing(L);
  lua_settop(L, 0);
  lua_pushcfunction(L, throw_with_errno);
  lua_setglobal(L, "lj_oserr_throw");
  if (!failed)
    failed = test_fast_function_landing(L, 1);
  if (!failed)
    failed = test_fast_function_landing(L, 2);
  lua_close(L);
  if (failed)
    return 1;
  puts("t-oserr-unwind-landing OK: C and fast-function landings "
       "restored errno");
  return 0;
}
