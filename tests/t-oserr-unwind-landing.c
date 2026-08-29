/*
** OS error-state preservation across an external-unwind final landing.
*/

#include <errno.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

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

int main(void)
{
  lua_State *L = luaL_newstate();
  int status, observed;
  if (L == NULL)
    return 2;
  lua_pushcfunction(L, throw_with_errno);
  errno = 0;
  status = lua_pcall(L, 0, 0, 0);
  observed = errno;
  lua_close(L);
  if (status != LUA_ERRRUN || observed != EDOM) {
    fprintf(stderr, "external unwind: status=%d errno=%d expected=%d\n",
	    status, observed, EDOM);
    return 1;
  }
  puts("t-oserr-unwind-landing OK: final landing restored errno");
  return 0;
}
