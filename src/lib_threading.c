/*
** Threading library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdint.h>

#define lib_threading_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_lib.h"
#include "lj_thr.h"

#define LJLIB_MODULE_threading

LJLIB_CF(threading_cpucount)
{
  setintV(L->top++, (int32_t)lj_thr_cpucount());
  return 1;
}

LJLIB_CF(threading_fence)
{
  lj_thr_fence();
  return 0;
}

LJLIB_CF(threading_sleep)
{
  lua_Number sec = L->base < L->top ? lj_lib_checknum(L, 1) : 0;
  int64_t ns = 0;
  if (sec > 0) {
    lua_Number nsec = sec * 1000000000.0;
    ns = nsec > (lua_Number)INT64_MAX ? INT64_MAX : (int64_t)nsec;
  }
  (void)lj_thr_sleep_ns(L, ns);
  return 0;
}

#include "lj_libdef.h"

LUALIB_API int luaopen_threading(lua_State *L)
{
  LJ_LIB_REG(L, LUA_THREADINGLIBNAME, threading);
  return 1;
}
