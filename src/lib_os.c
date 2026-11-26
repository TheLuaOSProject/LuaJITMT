/*
** OS library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define lib_os_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_lib.h"
#include "lj_locale.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

#if LJ_TARGET_POSIX
#include <unistd.h>
#endif

#if !LJ_TARGET_PSVITA
#include <locale.h>
#endif

/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_os

/* -- Native-state wrappers ---------------------------------------------- */

static int os_had_pending_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && (lj_tg_reqmask_acq(tg) & LJ_GC2_HS_STOPREQ);
}

static void os_checkstop_fresh(lua_State *L, uint32_t actions, int had_stopreq,
			       int had_pending_stopreq)
{
  if (had_pending_stopreq && !(actions & LJ_GC2_HS_STOPREQ))
    actions |= lj_safepoint_poll(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
}

static int os_native_remove_action(lua_State *L, const char *filename,
				   uint32_t *actionsp)
{
  int ok;
  lj_native_enter(L2TG(L));
  ok = remove(filename);
  *actionsp = lj_native_leave(L);
  return ok;
}

static int os_native_remove(lua_State *L, const char *filename)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  int had_pending_stopreq = os_had_pending_stopreq(L);
  int ok = os_native_remove_action(L, filename, &actions);
  os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq);
  return ok;
}

static int os_native_rename(lua_State *L, const char *fromname,
			    const char *toname)
{
  int had_stopreq = lj_safepoint_had_stopreq(L);
  int had_pending_stopreq = os_had_pending_stopreq(L);
  uint32_t actions;
  int ok;
  lj_native_enter(L2TG(L));
  ok = rename(fromname, toname);
  actions = lj_native_leave(L);
  os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq);
  return ok;
}

#if !(LJ_TARGET_PS3 || LJ_TARGET_PS4 || LJ_TARGET_PS5 || LJ_TARGET_PSVITA || LJ_TARGET_NX)
#if LJ_TARGET_POSIX
static int os_native_mkstemp(lua_State *L, char *buf)
{
  TGState *tg = L2TG(L);
  int had_stopreq = lj_safepoint_had_stopreq(L);
  int had_pending_stopreq = os_had_pending_stopreq(L);
  uint32_t actions;
  int fd;
  lj_native_enter(tg);
  fd = mkstemp(buf);
  if (fd != -1)
    close(fd);
  actions = lj_native_leave(L);
  if (fd != -1 &&
      lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t remove_actions;
    (void)os_native_remove_action(L, buf, &remove_actions);
    actions |= remove_actions;
  }
  os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq);
  return fd;
}
#else
static char *os_native_tmpnam(lua_State *L, char *buf)
{
  int had_stopreq = lj_safepoint_had_stopreq(L);
  int had_pending_stopreq = os_had_pending_stopreq(L);
  uint32_t actions;
  char *p;
  lj_native_enter(L2TG(L));
  p = tmpnam(buf);
  actions = lj_native_leave(L);
  os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq);
  return p;
}
#endif
#endif

LJLIB_CF(os_execute)
{
#if LJ_NO_SYSTEM
#if LJ_52
  errno = ENOSYS;
  return luaL_fileresult(L, 0, NULL);
#else
  lua_pushinteger(L, -1);
  return 1;
#endif
#else
  const char *cmd = luaL_optstring(L, 1, NULL);
  TGState *tg = L2TG(L);
  int had_stopreq = lj_safepoint_had_stopreq(L);
  int had_pending_stopreq = os_had_pending_stopreq(L);
  uint32_t actions;
  int stat;
  lj_native_enter(tg);
  stat = system(cmd);
  actions = lj_native_leave(L);
  os_checkstop_fresh(L, actions, had_stopreq, had_pending_stopreq);
#if LJ_52
  if (cmd)
    return luaL_execresult(L, stat);
  setboolV(L->top++, 1);
#else
  setintV(L->top++, stat);
#endif
  return 1;
#endif
}

LJLIB_CF(os_remove)
{
  const char *filename = luaL_checkstring(L, 1);
  return luaL_fileresult(L, os_native_remove(L, filename) == 0, filename);
}

LJLIB_CF(os_rename)
{
  const char *fromname = luaL_checkstring(L, 1);
  const char *toname = luaL_checkstring(L, 2);
  return luaL_fileresult(L, os_native_rename(L, fromname, toname) == 0,
			 fromname);
}

LJLIB_CF(os_tmpname)
{
#if LJ_TARGET_PS3 || LJ_TARGET_PS4 || LJ_TARGET_PS5 || LJ_TARGET_PSVITA || LJ_TARGET_NX
  lj_err_caller(L, LJ_ERR_OSUNIQF);
  return 0;
#else
#if LJ_TARGET_POSIX
  char buf[15+1];
  int fp;
  strcpy(buf, "/tmp/lua_XXXXXX");
  fp = os_native_mkstemp(L, buf);
  if (fp == -1)
    lj_err_caller(L, LJ_ERR_OSUNIQF);
#else
  char buf[L_tmpnam];
  if (os_native_tmpnam(L, buf) == NULL)
    lj_err_caller(L, LJ_ERR_OSUNIQF);
#endif
  lua_pushstring(L, buf);
  return 1;
#endif
}

LJLIB_CF(os_getenv)
{
#if LJ_TARGET_CONSOLE
  lua_pushnil(L);
#else
  lua_pushstring(L, getenv(luaL_checkstring(L, 1)));  /* if NULL push nil */
#endif
  return 1;
}

LJLIB_CF(os_exit)
{
  int status;
  if (L->base < L->top && tvisbool(L->base))
    status = boolV(L->base) ? EXIT_SUCCESS : EXIT_FAILURE;
  else
    status = lj_lib_optint(L, 1, EXIT_SUCCESS);
  if (L->base+1 < L->top && tvistruecond(L->base+1))
    lua_close(L);
  exit(status);
  return 0;  /* Unreachable. */
}

LJLIB_CF(os_clock)
{
  setnumV(L->top++, ((lua_Number)clock())*(1.0/(lua_Number)CLOCKS_PER_SEC));
  return 1;
}

/* ------------------------------------------------------------------------ */

static void setfield(lua_State *L, const char *key, int value)
{
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}

static void setboolfield(lua_State *L, const char *key, int value)
{
  if (value < 0)  /* undefined? */
    return;  /* does not set field */
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}

static int getboolfield(lua_State *L, const char *key)
{
  int res;
  lua_getfield(L, -1, key);
  res = lua_isnil(L, -1) ? -1 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}

static int getfield(lua_State *L, const char *key, int d)
{
  int res;
  lua_getfield(L, -1, key);
  if (lua_isnumber(L, -1)) {
    res = (int)lua_tointeger(L, -1);
  } else {
    if (d < 0)
      lj_err_callerv(L, LJ_ERR_OSDATEF, key);
    res = d;
  }
  lua_pop(L, 1);
  return res;
}

static struct tm *os_date_tm(time_t *t, int utc, struct tm *rtm)
{
#if LJ_TARGET_POSIX
  return utc ? gmtime_r(t, rtm) : localtime_r(t, rtm);
#elif LJ_TARGET_WINDOWS
  return (utc ? gmtime_s(rtm, t) : localtime_s(rtm, t)) == 0 ? rtm : NULL;
#else
  UNUSED(rtm);
  return utc ? gmtime(t) : localtime(t);
#endif
}

LJLIB_CF(os_date)
{
  const char *s = luaL_optstring(L, 1, "%c");
  time_t t = lua_isnoneornil(L, 2) ? time(NULL) :
	     lj_num2int_type(luaL_checknumber(L, 2), time_t);
  struct tm *stm;
  struct tm rtm;
  int utc = 0;
  if (*s == '!') {  /* UTC? */
    s++;  /* Skip '!' */
    utc = 1;
  }
  stm = os_date_tm(&t, utc, &rtm);
  if (stm == NULL) {  /* Invalid date? */
    setnilV(L->top++);
  } else if (strcmp(s, "*t") == 0) {
    lua_createtable(L, 0, 9);  /* 9 = number of fields */
    setfield(L, "sec", stm->tm_sec);
    setfield(L, "min", stm->tm_min);
    setfield(L, "hour", stm->tm_hour);
    setfield(L, "day", stm->tm_mday);
    setfield(L, "month", stm->tm_mon+1);
    setfield(L, "year", stm->tm_year+1900);
    setfield(L, "wday", stm->tm_wday+1);
    setfield(L, "yday", stm->tm_yday+1);
    setboolfield(L, "isdst", stm->tm_isdst);
  } else if (*s) {
    SBuf *sb = lj_buf_tmp_(L);
    MSize sz = 0, retry = 4;
    const char *q;
    for (q = s; *q; q++)
      sz += (*q == '%') ? 30 : 1;  /* Overflow doesn't matter. */
    while (retry--) {  /* Limit growth for invalid format or empty result. */
      char *buf = lj_buf_need(sb, sz);
      size_t len = strftime(buf, sbufsz(sb), s, stm);
      if (len) {
	setstrV(L, L->top++, lj_str_new(L, buf, len));
	lj_gc_check(L);
	break;
      }
      sz += (sz|1);
    }
  } else {
    setstrV(L, L->top++, &G(L)->strempty);
  }
  return 1;
}

LJLIB_CF(os_time)
{
  time_t t;
  if (lua_isnoneornil(L, 1)) {  /* called without args? */
    t = time(NULL);  /* get current time */
  } else {
    struct tm ts;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);  /* make sure table is at the top */
    ts.tm_sec = getfield(L, "sec", 0);
    ts.tm_min = getfield(L, "min", 0);
    ts.tm_hour = getfield(L, "hour", 12);
    ts.tm_mday = getfield(L, "day", -1);
    ts.tm_mon = (int)((unsigned int)getfield(L, "month", -1) - 1u);
    ts.tm_year = (int)((unsigned int)getfield(L, "year", -1) - 1900u);
    ts.tm_isdst = getboolfield(L, "isdst");
    t = mktime(&ts);
  }
  if (t == (time_t)(-1))
    lua_pushnil(L);
  else
    lua_pushnumber(L, (lua_Number)t);
  return 1;
}

LJLIB_CF(os_difftime)
{
  lua_pushnumber(L,
    difftime(lj_num2int_type(luaL_checknumber(L, 1), time_t),
	     lj_num2int_type(luaL_optnumber(L, 2, (lua_Number)0), time_t)));
  return 1;
}

/* ------------------------------------------------------------------------ */

#if !LJ_TARGET_PSVITA
/* Lua heaps share libc's locale names, including their lifetime. */
static uint32_t os_locale_lock;
static uint32_t os_locale_writer;
static global_State *os_locale_heaps;

static void os_locale_lock_acquire(void)
{
  for (;;) {
    uint32_t expect = 0;
    if (la_cas32(&os_locale_lock, &expect, 1, __ATOMIC_ACQUIRE,
		 __ATOMIC_RELAXED))
      return;
    (void)la_futex_wait(&os_locale_lock, 1, 1000000);
  }
}

static void os_locale_lock_release(void)
{
  la_store32_rel(&os_locale_lock, 0);
  la_futex_wake(&os_locale_lock, 1);
}

void lj_locale_heap_register(global_State *g)
{
  os_locale_lock_acquire();
  g->locale_next = os_locale_heaps;
  os_locale_heaps = g;
  os_locale_lock_release();
}

void lj_locale_heap_unregister(global_State *g)
{
  global_State **link;
  os_locale_lock_acquire();
  for (link = &os_locale_heaps; *link && *link != g;
       link = &(*link)->locale_next) {}
  if (!*link)
    abort();  /* A live heap must have exactly one registration. */
  *link = g->locale_next;
  os_locale_lock_release();
}

void lj_locale_wait_attach(void)
{
  while (la_load32_acq(&os_locale_writer) != 0)
    (void)la_futex_wait(&os_locale_writer, 1, 1000000);
}

/* The locale lock stabilizes the heap list. The writer flag excludes entry.
** Heaps on the calling actor can have no other executing main state. Load
** entering before live so a reservation transferring between them cannot
** be missed. A later entrant must pass through lj_locale_wait_attach(). */
static int os_locale_quiescent(void)
{
  global_State *g;
  uint32_t actor = lj_thr_actor_current();
  if (actor == 0)
    return 0;
  for (g = os_locale_heaps; g; g = g->locale_next) {
    if (lj_tg_actor_acq(g->main_tg) != actor ||
	mt_entering_acq(g) != 0 || mt_live_acq(g) != 0)
      return 0;
  }
  return 1;
}

static TValue *os_locale_push_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  UNUSED(dummy);
  lua_pushstring(L, (const char *)ud);
  return NULL;
}
#endif

LJLIB_CF(os_setlocale)
{
#if LJ_TARGET_PSVITA
  lua_pushliteral(L, "C");
#else
  GCstr *s = lj_lib_optstr(L, 1);
  const char *str = s ? strdata(s) : NULL;
  const char *res;
  char buf[256], *copy = NULL;
  int oom = 0, mutation = 0;
  int opt = lj_lib_checkopt(L, 2, 6,
    "\5ctype\7numeric\4time\7collate\10monetary\1\377\3all");
  if (opt == 0) opt = LC_CTYPE;
  else if (opt == 1) opt = LC_NUMERIC;
  else if (opt == 2) opt = LC_TIME;
  else if (opt == 3) opt = LC_COLLATE;
  else if (opt == 4) opt = LC_MONETARY;
  else if (opt == 6) opt = LC_ALL;
  os_locale_lock_acquire();
  if (str) {
    /* Pair with the entrant's fence after publishing mt_entering. Either
    ** we see its reservation or it sees the pending write before entry. */
    la_store32_rel(&os_locale_writer, 1);
    la_fence_seq();
    mutation = os_locale_quiescent();
    if (mutation)
      res = setlocale(opt, str);
    la_store32_rel(&os_locale_writer, 0);
    la_futex_wake(&os_locale_writer, INT_MAX);
  }
  if (!mutation) {
    /* An exact name returned by libc requests no change. Do not ask libc
    ** to mutate, and keep empty-string environment lookup on the write path. */
    res = setlocale(opt, NULL);
    if (str && (!*str || !res || strcmp(str, res) != 0))
      res = NULL;
  }
  if (res) {
    size_t len = strlen(res) + 1;
    copy = len <= sizeof(buf) ? buf : (char *)malloc(len);
    if (copy)
      memcpy(copy, res, len);
    else
      oom = 1;
  }
  os_locale_lock_release();
  if (oom)
    lj_err_mem(L);
  if (copy && copy != buf) {
    int status = lj_vm_cpcall(L, NULL, copy, os_locale_push_cp);
    free(copy);
    if (status)
      lj_err_throw(L, status);
  } else {
    lua_pushstring(L, copy);
  }
#endif
  return 1;
}

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"

LUALIB_API int luaopen_os(lua_State *L)
{
  LJ_LIB_REG(L, LUA_OSLIBNAME, os);
  return 1;
}
