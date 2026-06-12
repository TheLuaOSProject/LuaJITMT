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
#include "lj_chan.h"
#include "lj_err.h"
#include "lj_gc.h"
#include "lj_lib.h"
#include "lj_thr.h"
#include "lj_udata.h"

/* -- Channel methods ----------------------------------------------------- */

static LJChan *threading_tochan(lua_State *L)
{
  if (!(L->base < L->top && tvisudata(L->base) &&
	udataV(L->base)->udtype == UDTYPE_CHANNEL))
    lj_err_argtype(L, 1, "threading.channel");
  return (LJChan *)uddata(udataV(L->base));
}

static void threading_push_recv(lua_State *L, int rc, TValue *out)
{
  if (rc == LJ_CHAN_OK) {
    copyTV(L, L->top++, out);
    setboolV(L->top++, 1);
  } else if (rc == LJ_CHAN_CLOSED) {
    setnilV(L->top++);
    setboolV(L->top++, 0);
  } else {
    setnilV(L->top++);
    lua_pushliteral(L, "empty");
  }
}

#define LJLIB_MODULE_threading_channel

LJLIB_CF(threading_channel_send)
{
  GCudata *ud;
  LJChan *ch = threading_tochan(L);
  cTValue *tv = lj_lib_checkany(L, 2);
  ud = udataV(L->base);
  lj_gc_barrier(L, ud, tv);  /* 09 section 9.5: publish Lua refs to channel. */
  if (lj_chan_send(L, ch, tv) == LJ_CHAN_CLOSED)
    lj_err_callermsg(L, "closed channel");
  setboolV(L->top++, 1);
  return 1;
}

LJLIB_CF(threading_channel_recv)
{
  TValue out;
  LJChan *ch = threading_tochan(L);
  int rc;
  setnilV(&out);
  rc = lj_chan_recv(L, ch, &out);
  threading_push_recv(L, rc, &out);
  return 2;
}

LJLIB_CF(threading_channel_peek)
{
  TValue out;
  LJChan *ch = threading_tochan(L);
  int rc;
  setnilV(&out);
  rc = lj_chan_peek(ch, &out);
  threading_push_recv(L, rc, &out);
  return 2;
}

LJLIB_CF(threading_channel_close)
{
  lj_chan_close(threading_tochan(L));
  return 0;
}

LJLIB_CF(threading_channel___gc)
{
  if (L->base < L->top && tvisudata(L->base) &&
      udataV(L->base)->udtype == UDTYPE_CHANNEL)
    lj_chan_close((LJChan *)uddata(udataV(L->base)));
  return 0;
}

LJLIB_CF(threading_channel___tostring)
{
  (void)threading_tochan(L);
  lua_pushliteral(L, "threading.channel");
  return 1;
}

LJLIB_PUSH("threading.channel") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

#define LJLIB_MODULE_threading

LJLIB_PUSH(top-2) LJLIB_SET(!)  /* Set environment to channel methods. */

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

LJLIB_CF(threading_channel)
{
  int32_t n = L->base < L->top ? lj_lib_checkint(L, 1) : 0;
  uint32_t cap;
  uint32_t rcap;
  uint64_t bytes;
  GCtab *env;
  GCudata *ud;
  if (n < 0)
    lj_err_arg(L, 1, LJ_ERR_NUMRNG);
  cap = (uint32_t)n;
  rcap = lj_chan_round_capacity(cap);
  bytes = sizeof(LJChan) + ((uint64_t)rcap - 1u) * sizeof(LJChanSlot);
  if (bytes > LJ_MAX_UDATA)
    lj_err_arg(L, 1, LJ_ERR_NUMRNG);
  env = tabref(curr_func(L)->c.env);
  ud = lj_udata_new(L, lj_chan_memsize(cap), env);
  ud->udtype = UDTYPE_CHANNEL;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcref(ud->metatable, obj2gco(env));
  lj_chan_init((LJChan *)uddata(ud), cap);
  setudataV(L, L->top++, ud);
  lj_gc_check(L);
  return 1;
}

#include "lj_libdef.h"

LUALIB_API int luaopen_threading(lua_State *L)
{
  LJ_LIB_REG(L, NULL, threading_channel);
  LJ_LIB_REG(L, LUA_THREADINGLIBNAME, threading);
  return 1;
}
