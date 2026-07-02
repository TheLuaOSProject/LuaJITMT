/*
** Debug library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_debug_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_state.h"
#include "lj_thr.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_lib.h"
#include "lj_trace.h"

/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_debug

LJLIB_CF(debug_getregistry)
{
  lj_registry_load_acq(G(L), L->top++);
  return 1;
}

LJLIB_CF(debug_getmetatable)	LJLIB_REC(.)
{
  lj_lib_checkany(L, 1);
  if (!lua_getmetatable(L, 1)) {
    setnilV(L->top-1);
  }
  return 1;
}

LJLIB_CF(debug_setmetatable)
{
  lj_lib_checktabornil(L, 2);
  L->top = L->base+2;
  lua_setmetatable(L, 1);
#if !LJ_52
  setboolV(L->top-1, 1);
#endif
  return 1;
}

LJLIB_CF(debug_getfenv)
{
  lj_lib_checkany(L, 1);
  lua_getfenv(L, 1);
  return 1;
}

LJLIB_CF(debug_setfenv)
{
  lj_lib_checktab(L, 2);
  L->top = L->base+2;
  if (!lua_setfenv(L, 1))
    lj_err_caller(L, LJ_ERR_SETFENV);
  return 1;
}

/* ------------------------------------------------------------------------ */

static void settabss(lua_State *L, const char *i, const char *v)
{
  lua_pushstring(L, v);
  lua_setfield(L, -2, i);
}

static void settabsi(lua_State *L, const char *i, int v)
{
  lua_pushinteger(L, v);
  lua_setfield(L, -2, i);
}

static void settabsb(lua_State *L, const char *i, int v)
{
  lua_pushboolean(L, v);
  lua_setfield(L, -2, i);
}

static lua_State *getthread(lua_State *L, int *arg)
{
  if (L->base < L->top && tvisthread(L->base)) {
    *arg = 1;
    return threadV(L->base);
  } else {
    *arg = 0;
    return L;
  }
}

static void debug_claimthread(lua_State *L, lua_State *L1, LJStateClaim *claim)
{
  claim->L = NULL;
  claim->tid = 0;
  claim->release = 0;
  if (L != L1 && !lj_state_tryclaim(L1, lj_thr_current_id(G(L)), claim))
    lj_err_callermsg(L, "thread busy");
}

static void treatstackoption(lua_State *L, lua_State *L1, const char *fname)
{
  if (L == L1) {
    lua_pushvalue(L, -2);
    lua_remove(L, -3);
  }
  else
    lua_xmove(L1, L, 1);
  lua_setfield(L, -2, fname);
}

static void debug_pushfunc_root(lua_State *L, GCfunc *fn)
{
  setfuncV(L, L->top, fn);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
}

LJLIB_CF(debug_getinfo)
{
  LJStateClaim claim;
  lj_Debug ar;
  GCfunc *fnarg = NULL, *fnres = NULL, *linesfn = NULL;
  int arg, opt_f = 0, opt_L = 0;
  int nres;
  lua_State *L1 = getthread(L, &arg);
  const char *options = luaL_optstring(L, arg+2, "flnSu");
  const char *p;
  int line_root = 0;
  claim.L = NULL;
  claim.tid = 0;
  claim.release = 0;
  for (p = options + (*options == '>'); *p; p++) {
    opt_f |= *p == 'f';
    opt_L |= *p == 'L';
  }
  nres = opt_f + opt_L;
  if (nres)
    lj_state_checkstack(L, 2);
  if (lua_isnumber(L, arg+1)) {
    debug_claimthread(L, L1, &claim);
    if (!lj_debug_getstack_claimed(L1, (int)lua_tointeger(L, arg+1), &ar)) {
      lj_state_dropclaim(&claim);
      setnilV(L->top-1);
      return 1;
    }
  } else if (L->base+arg < L->top && tvisfunc(L->base+arg)) {
    options = lua_pushfstring(L, ">%s", options);
    fnarg = funcV(L->base+arg);
  } else {
    lj_err_arg(L, arg+1, LJ_ERR_NOFUNCL);
  }
  if (!lj_debug_getinfo_claimed(fnarg ? L : L1, options, &ar, 1, fnarg,
				&fnres, &linesfn)) {
    lj_state_dropclaim(&claim);
    lj_err_arg(L, arg+2, LJ_ERR_INVOPT);
  }
  if (opt_f)
    debug_pushfunc_root(L, fnres);
  if (opt_L && !opt_f) {
    debug_pushfunc_root(L, linesfn);
    line_root = 1;
  }
  lj_state_dropclaim(&claim);
  if (opt_L) {
    lj_debug_pushactivelines(L, linesfn);
    if (line_root)
      lua_remove(L, -2);
  }
  lua_createtable(L, 0, 16);  /* Create result table. */
  for (; *options; options++) {
    switch (*options) {
    case 'S':
      settabss(L, "source", ar.source);
      settabss(L, "short_src", ar.short_src);
      settabsi(L, "linedefined", ar.linedefined);
      settabsi(L, "lastlinedefined", ar.lastlinedefined);
      settabss(L, "what", ar.what);
      break;
    case 'l':
      settabsi(L, "currentline", ar.currentline);
      break;
    case 'u':
      settabsi(L, "nups", ar.nups);
      settabsi(L, "nparams", ar.nparams);
      settabsb(L, "isvararg", ar.isvararg);
      break;
    case 'n':
      settabss(L, "name", ar.name);
      settabss(L, "namewhat", ar.namewhat);
      break;
    case 'f': break;
    case 'L': break;
    default: break;
    }
  }
  if (opt_L) treatstackoption(L, L, "activelines");
  if (opt_f) treatstackoption(L, L, "func");
  return 1;  /* Return result table. */
}

LJLIB_CF(debug_getlocal)
{
  LJStateClaim claim;
  int arg;
  lua_State *L1 = getthread(L, &arg);
  lj_Debug ar;
  TValue tv;
  GCfunc *fnroot = NULL;
  const char *name;
  int slot = lj_lib_checkint(L, arg+2);
  int level;
  if (tvisfunc(L->base+arg)) {
    L->top = L->base+arg+1;
    lua_pushstring(L, lua_getlocal(L, NULL, slot));
    return 1;
  }
  level = lj_lib_checkint(L, arg+1);
  lj_state_checkstack(L, 3);
  debug_claimthread(L, L1, &claim);
  if (!lj_debug_getstack_claimed(L1, level, &ar)) {
    lj_state_dropclaim(&claim);
    lj_err_arg(L, arg+1, LJ_ERR_LVLRNG);
  }
  name = lj_debug_getlocal_claimed(L1, &ar, slot, &tv, &fnroot);
  if (name) {
    int rooted = 0;
    copyTV(L, L->top, &tv);
    lj_state_stack_pubtv(L, L, L->top);
    L->top++;
    if (fnroot) {
      debug_pushfunc_root(L, fnroot);
      rooted = 1;
    }
    lj_state_dropclaim(&claim);
    lua_pushstring(L, name);
    if (rooted)
      lua_remove(L, -2);
    lua_pushvalue(L, -2);
    return 2;
  } else {
    lj_state_dropclaim(&claim);
    setnilV(L->top-1);
    return 1;
  }
}

LJLIB_CF(debug_setlocal)
{
  LJStateClaim claim;
  int arg;
  lua_State *L1 = getthread(L, &arg);
  lj_Debug ar;
  TValue tvcopy;
  TValue *tv;
  TValue *pubuv = NULL;
  GCfunc *fnroot = NULL;
  const char *name;
  int level = lj_lib_checkint(L, arg+1);
  int slot = lj_lib_checkint(L, arg+2);
  tv = lj_lib_checkany(L, arg+3);
  copyTV(L, &tvcopy, tv);
  lj_state_checkstack(L, 1);
  debug_claimthread(L, L1, &claim);
  if (!lj_debug_getstack_claimed(L1, level, &ar)) {
    lj_state_dropclaim(&claim);
    lj_err_arg(L, arg+1, LJ_ERR_LVLRNG);
  }
  name = lj_debug_setlocal_claimed(L1, &ar, slot, &tvcopy, &pubuv, &fnroot);
  if (name && fnroot)
    debug_pushfunc_root(L, fnroot);
  lj_state_dropclaim(&claim);
  if (pubuv)
    lj_gc_pubuv(G(L1), pubuv);
  lua_pushstring(L, name);
  if (name && fnroot)
    lua_remove(L, -2);
  return 1;
}

static int debug_getupvalue(lua_State *L, int get)
{
  int32_t n = lj_lib_checkint(L, 2);
  const char *name;
  lj_lib_checkfunc(L, 1);
  name = get ? lua_getupvalue(L, 1, n) : lua_setupvalue(L, 1, n);
  if (name) {
    lua_pushstring(L, name);
    if (!get) return 1;
    copyTV(L, L->top, L->top-2);
    L->top++;
    return 2;
  }
  return 0;
}

LJLIB_CF(debug_getupvalue)
{
  return debug_getupvalue(L, 1);
}

LJLIB_CF(debug_setupvalue)
{
  lj_lib_checkany(L, 3);
  return debug_getupvalue(L, 0);
}

LJLIB_CF(debug_upvalueid)
{
  GCfunc *fn = lj_lib_checkfunc(L, 1);
  int32_t n = lj_lib_checkint(L, 2) - 1;
  MSize nupvalues = isluafunc(fn) ? fn->l.nupvalues : fn->c.nupvalues;
  if ((uint32_t)n >= nupvalues)
    lj_err_arg(L, 2, LJ_ERR_IDXRNG);
  lua_pushlightuserdata(L, isluafunc(fn) ?
					   (void *)func_uvptr_acq(&fn->l, (uint32_t)n) :
					   (void *)&fn->c.upvalue[n]);
  return 1;
}

LJLIB_CF(debug_upvaluejoin)
{
  GCfunc *fn[2];
  GCRef *p[2];
  int i;
  for (i = 0; i < 2; i++) {
    int32_t n;
    fn[i] = lj_lib_checkfunc(L, 2*i+1);
    if (!isluafunc(fn[i]))
      lj_err_arg(L, 2*i+1, LJ_ERR_NOLFUNC);
    n = lj_lib_checkint(L, 2*i+2) - 1;
    if ((uint32_t)n >= fn[i]->l.nupvalues)
      lj_err_arg(L, 2*i+2, LJ_ERR_IDXRNG);
    p[i] = &fn[i]->l.uvptr[n];
  }
  {
    GCobj *uv = gcref_acq(*p[1]);
    GCobj *old = gcref_acq(*p[0]);
    if (old != uv) {
      if (lj_trace_flushall_hs(L))
	lj_err_caller(L, LJ_ERR_NOGCMM);
      setgcrefrel(*p[0], uv);
      lj_gc_pubobjobj(L, fn[0], uv);
    }
  }
  return 0;
}

#if LJ_52
LJLIB_CF(debug_getuservalue)
{
  TValue *o = L->base;
  if (o < L->top && tvisudata(o))
    settabV(L, o, lj_udata_env_acq(udataV(o)));
  else
    setnilV(o);
  L->top = o+1;
  return 1;
}

LJLIB_CF(debug_setuservalue)
{
  TValue *o = L->base;
  if (!(o < L->top && tvisudata(o)))
    lj_err_argt(L, 1, LUA_TUSERDATA);
  if (!(o+1 < L->top && tvistab(o+1)))
    lj_err_argt(L, 2, LUA_TTABLE);
  L->top = o+2;
  lua_setfenv(L, 1);
  return 1;
}
#endif

/* ------------------------------------------------------------------------ */

#define KEY_HOOK	(U64x(81000000,00000000)|'h')

static void hookf(lua_State *L, lua_Debug *ar)
{
  static const char *const hooknames[] =
    {"call", "return", "line", "count", "tail return"};
  (L->top++)->u64 = KEY_HOOK;
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isfunction(L, -1)) {
    lua_pushstring(L, hooknames[(int)ar->event]);
    if (ar->currentline >= 0)
      lua_pushinteger(L, ar->currentline);
    else lua_pushnil(L);
    lua_call(L, 2, 0);
  }
}

static int makemask(const char *smask, int count)
{
  int mask = 0;
  if (strchr(smask, 'c')) mask |= LUA_MASKCALL;
  if (strchr(smask, 'r')) mask |= LUA_MASKRET;
  if (strchr(smask, 'l')) mask |= LUA_MASKLINE;
  if (count > 0) mask |= LUA_MASKCOUNT;
  return mask;
}

static char *unmakemask(int mask, char *smask)
{
  int i = 0;
  if (mask & LUA_MASKCALL) smask[i++] = 'c';
  if (mask & LUA_MASKRET) smask[i++] = 'r';
  if (mask & LUA_MASKLINE) smask[i++] = 'l';
  smask[i] = '\0';
  return smask;
}

LJLIB_CF(debug_sethook)
{
  int arg, mask, count;
  lua_Hook func;
  (void)getthread(L, &arg);
  if (lua_isnoneornil(L, arg+1)) {
    lua_settop(L, arg+1);
    func = NULL; mask = 0; count = 0;  /* turn off hooks */
  } else {
    const char *smask = luaL_checkstring(L, arg+2);
    luaL_checktype(L, arg+1, LUA_TFUNCTION);
    count = (int)luaL_optinteger(L, arg+3, 0);
    func = hookf; mask = makemask(smask, count);
  }
  (L->top++)->u64 = KEY_HOOK;
  lua_pushvalue(L, arg+1);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_sethook(L, func, mask, count);
  return 0;
}

LJLIB_CF(debug_gethook)
{
  char buff[5];
  int mask = lua_gethookmask(L);
  lua_Hook hook = lua_gethook(L);
  if (hook != NULL && hook != hookf) {  /* external hook? */
    lua_pushliteral(L, "external hook");
  } else {
    (L->top++)->u64 = KEY_HOOK;
    lua_rawget(L, LUA_REGISTRYINDEX);   /* get hook */
  }
  lua_pushstring(L, unmakemask(mask, buff));
  lua_pushinteger(L, lua_gethookcount(L));
  return 3;
}

/* ------------------------------------------------------------------------ */

static int debug_had_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int debug_fresh_stopreq(lua_State *L, uint32_t actions,
			       int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static void debug_checkstop_fresh(lua_State *L, uint32_t actions,
				  int had_stopreq)
{
  if (debug_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

static void debug_native_fputs(lua_State *L, const char *s, FILE *fp)
{
  uint32_t actions;
  int had_stopreq = debug_had_stopreq(L);
  lj_native_enter(L2TG(L));
  (void)fputs(s, fp);
  actions = lj_native_leave(L);
  debug_checkstop_fresh(L, actions, had_stopreq);
}

static char *debug_native_fgets(lua_State *L, char *buf, int size, FILE *fp)
{
  uint32_t actions;
  int had_stopreq = debug_had_stopreq(L);
  char *p;
  lj_native_enter(L2TG(L));
  p = fgets(buf, size, fp);
  actions = lj_native_leave(L);
  debug_checkstop_fresh(L, actions, had_stopreq);
  return p;
}

LJLIB_CF(debug_debug)
{
  for (;;) {
    char buffer[250];
    debug_native_fputs(L, "lua_debug> ", stderr);
    if (debug_native_fgets(L, buffer, sizeof(buffer), stdin) == 0 ||
	strcmp(buffer, "cont\n") == 0)
      return 0;
    if (luaL_loadbuffer(L, buffer, strlen(buffer), "=(debug command)") ||
	lua_pcall(L, 0, 0, 0)) {
      const char *s = lua_tostring(L, -1);
      debug_native_fputs(L, s ? s : "(error object is not a string)", stderr);
      debug_native_fputs(L, "\n", stderr);
    }
    lua_settop(L, 0);  /* remove eventual returns */
  }
}

/* ------------------------------------------------------------------------ */

#define LEVELS1	12	/* size of the first part of the stack */
#define LEVELS2	10	/* size of the second part of the stack */

LJLIB_CF(debug_traceback)
{
  int arg;
  lua_State *L1 = getthread(L, &arg);
  const char *msg = lua_tostring(L, arg+1);
  int level = lj_lib_optint(L, arg+2, (L == L1));
  if (msg == NULL && L->top > L->base+arg)
    L->top = L->base+arg+1;
  else
    luaL_traceback(L, L1, msg, level);
  return 1;
}

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"

LUALIB_API int luaopen_debug(lua_State *L)
{
  LJ_LIB_REG(L, LUA_DBLIBNAME, debug);
  return 1;
}
