/*
** Base and coroutine library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2011 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#include <stdio.h>

#define lib_base_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_thr.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cconv.h"
#endif
#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_dispatch.h"
#include "lj_char.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_lib.h"

/* -- Base library: checks ------------------------------------------------ */

#define LJLIB_MODULE_base

LJLIB_ASM(assert)		LJLIB_REC(.)
{
  lj_lib_checkany(L, 1);
  if (L->top == L->base+1)
    lj_err_caller(L, LJ_ERR_ASSERT);
  else if (tvisstr(L->base+1) || tvisnumber(L->base+1))
    lj_err_callermsg(L, strdata(lj_lib_checkstr(L, 2)));
  else
    lj_err_run(L);
  return FFH_UNREACHABLE;
}

/* ORDER LJ_T */
LJLIB_PUSH("nil")
LJLIB_PUSH("boolean")
LJLIB_PUSH(top-1)  /* boolean */
LJLIB_PUSH("userdata")
LJLIB_PUSH("string")
LJLIB_PUSH("upval")
LJLIB_PUSH("thread")
LJLIB_PUSH("proto")
LJLIB_PUSH("function")
LJLIB_PUSH("trace")
LJLIB_PUSH("cdata")
LJLIB_PUSH("table")
LJLIB_PUSH(top-9)  /* userdata */
LJLIB_PUSH("number")
LJLIB_ASM_(type)		LJLIB_REC(.)
/* Recycle the lj_lib_checkany(L, 1) from assert. */

/* -- Base library: iterators --------------------------------------------- */

/* This solves a circular dependency problem -- change FF_next_N as needed. */
LJ_STATIC_ASSERT((int)FF_next == FF_next_N);

LJLIB_ASM(next)			LJLIB_REC(.)
{
  lj_lib_checktab(L, 1);
  lj_err_msg(L, LJ_ERR_NEXTIDX);
  return FFH_UNREACHABLE;
}

#if LJ_52 || LJ_HASFFI
static int ffh_pairs(lua_State *L, MMS mm)
{
  TValue *o = lj_lib_checkany(L, 1);
  TValue motv;
  cTValue *mo = lj_meta_lookuptv(L, &motv, o, mm);
  if ((LJ_52 || tviscdata(o)) && !tvisnil(mo)) {
    L->top = o+1;  /* Only keep one argument. */
    copyTV(L, L->base-1-LJ_FR2, mo);  /* Replace callable. */
    return FFH_TAILCALL;
  } else {
    if (!tvistab(o)) lj_err_argt(L, 1, LUA_TTABLE);
    if (LJ_FR2) { copyTV(L, o-1, o); o--; }
    setfuncV(L, o-1, funcV(lj_lib_upvalue(L, 1)));
    if (mm == MM_pairs) setnilV(o+1); else setintV(o+1, 0);
    return FFH_RES(3);
  }
}
#else
#define ffh_pairs(L, mm)	(lj_lib_checktab(L, 1), FFH_UNREACHABLE)
#endif

LJLIB_PUSH(lastcl)
LJLIB_ASM(pairs)		LJLIB_REC(xpairs 0)
{
  return ffh_pairs(L, MM_pairs);
}

LJLIB_NOREGUV LJLIB_ASM(ipairs_aux)	LJLIB_REC(.)
{
  lj_lib_checktab(L, 1);
  lj_lib_checkint(L, 2);
  return FFH_UNREACHABLE;
}

LJLIB_PUSH(lastcl)
LJLIB_ASM(ipairs)		LJLIB_REC(xpairs 1)
{
  return ffh_pairs(L, MM_ipairs);
}

/* -- Base library: getters and setters ----------------------------------- */

LJLIB_ASM_(getmetatable)	LJLIB_REC(.)
/* Recycle the lj_lib_checkany(L, 1) from assert. */

LJLIB_ASM(setmetatable)		LJLIB_REC(.)
{
  GCtab *t = lj_lib_checktab(L, 1);
  GCtab *mt = lj_lib_checktabornil(L, 2);
  TValue motv;
  if (!tvisnil(lj_meta_lookuptv(L, &motv, L->base, MM_metatable)))
    lj_err_caller(L, LJ_ERR_PROTMT);
  if (mt)
    mt->nomm = 0;  /* Do not trust stale metamethod miss caches. */
  setgcrefmt(t->metatable, obj2gco(mt));
  if (mt) { lj_gc_pubtabobj(L, t, mt); }
  settabV(L, L->base-1-LJ_FR2, t);
  return FFH_RES(1);
}

LJLIB_CF(getfenv)		LJLIB_REC(.)
{
  GCfunc *fn;
  cTValue *o = L->base;
  if (!(o < L->top && tvisfunc(o))) {
    int level = lj_lib_optint(L, 1, 1);
    if (level < 0)
      lj_err_arg(L, 1, LJ_ERR_INVLVL);
    o = lj_debug_frame(L, level, &level);
    if (o == NULL)
      lj_err_arg(L, 1, LJ_ERR_INVLVL);
    if (LJ_FR2) o--;
  }
  fn = &gcval(o)->fn;
  settabV(L, L->top++, isluafunc(fn) ? tabref_acq(fn->l.env) :
				       tabref_acq(L->env));
  return 1;
}

LJLIB_CF(setfenv)
{
  GCfunc *fn;
  GCtab *t = lj_lib_checktab(L, 2);
  cTValue *o = L->base;
  if (!(o < L->top && tvisfunc(o))) {
    int level = lj_lib_checkint(L, 1);
    if (level == 0) {
      /* NOBARRIER: A thread (i.e. L) is never black. */
      setgcrefrel(L->env, obj2gco(t));
      return 0;
    }
    if (level < 0)
      lj_err_arg(L, 1, LJ_ERR_INVLVL);
    o = lj_debug_frame(L, level, &level);
    if (o == NULL)
      lj_err_arg(L, 1, LJ_ERR_INVLVL);
    if (LJ_FR2) o--;
  }
  fn = &gcval(o)->fn;
  if (!isluafunc(fn))
    lj_err_caller(L, LJ_ERR_SETFENV);
  setgcrefrel(fn->l.env, obj2gco(t));
  lj_gc_pubobjobj(L, obj2gco(fn), t);
  setfuncV(L, L->top++, fn);
  return 1;
}

LJLIB_ASM(rawget)		LJLIB_REC(.)
{
  lj_lib_checktab(L, 1);
  lj_lib_checkany(L, 2);
  return FFH_UNREACHABLE;
}

LJLIB_CF(rawset)		LJLIB_REC(.)
{
  lj_lib_checktab(L, 1);
  lj_lib_checkany(L, 2);
  L->top = 1+lj_lib_checkany(L, 3);
  lua_rawset(L, 1);
  return 1;
}

LJLIB_CF(rawequal)		LJLIB_REC(.)
{
  cTValue *o1 = lj_lib_checkany(L, 1);
  cTValue *o2 = lj_lib_checkany(L, 2);
  setboolV(L->top-1, lj_obj_equal(o1, o2));
  return 1;
}

#if LJ_52
LJLIB_CF(rawlen)		LJLIB_REC(.)
{
  cTValue *o = L->base;
  int32_t len;
  if (L->top > o && tvisstr(o))
    len = (int32_t)strV(o)->len;
  else
    len = (int32_t)lj_tab_len(lj_lib_checktab(L, 1));
  setintV(L->top-1, len);
  return 1;
}
#endif

LJLIB_CF(unpack)
{
  GCtab *t = lj_lib_checktab(L, 1);
  int32_t n, i = lj_lib_optint(L, 2, 1);
  int32_t e = (L->base+3-1 < L->top && !tvisnil(L->base+3-1)) ?
	      lj_lib_checkint(L, 3) : (int32_t)lj_tab_len(t);
  uint32_t nu;
  if (i > e) return 0;
  nu = (uint32_t)e - (uint32_t)i;
  n = (int32_t)(nu+1);
  if (nu >= LUAI_MAXCSTACK || !lua_checkstack(L, n))
    lj_err_caller(L, LJ_ERR_UNPACK);
  do {
    cTValue *tv = lj_tab_getint(t, i);
    if (tv) {
      TValue val;
      lj_tv_load_acq(&val, tv);
      copyTV(L, L->top++, &val);
    } else {
      setnilV(L->top++);
    }
    if (i >= e) break;
    i++;
  } while (1);
  return n;
}

LJLIB_CF(select)		LJLIB_REC(.)
{
  int32_t n = (int32_t)(L->top - L->base);
  if (n >= 1 && tvisstr(L->base) && *strVdata(L->base) == '#') {
    setintV(L->top-1, n-1);
    return 1;
  } else {
    int32_t i = lj_lib_checkint(L, 1);
    if (i < 0) i = n + i; else if (i > n) i = n;
    if (i < 1)
      lj_err_arg(L, 1, LJ_ERR_IDXRNG);
    return n - i;
  }
}

/* -- Base library: conversions ------------------------------------------- */

LJLIB_ASM(tonumber)		LJLIB_REC(.)
{
  int32_t base = lj_lib_optint(L, 2, 10);
  if (base == 10) {
    TValue *o = lj_lib_checkany(L, 1);
    if (lj_strscan_numberobj(o)) {
      copyTV(L, L->base-1-LJ_FR2, o);
      return FFH_RES(1);
    }
#if LJ_HASFFI
    if (tviscdata(o)) {
      CTState *cts = ctype_cts(L);
      CType *ct = lj_ctype_rawref(cts, cdataV(o)->ctypeid);
      if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
      if (ctype_isnum(ct->info) || ctype_iscomplex(ct->info)) {
	if (LJ_DUALNUM && ctype_isinteger_or_bool(ct->info) &&
	    ct->size <= 4 && !(ct->size == 4 && (ct->info & CTF_UNSIGNED))) {
	  int32_t i;
	  lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_INT32), CTID_INT32,
			   (uint8_t *)&i, o, 0);
	  setintV(L->base-1-LJ_FR2, i);
	  return FFH_RES(1);
	}
	lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_DOUBLE), CTID_DOUBLE,
			 (uint8_t *)&(L->base-1-LJ_FR2)->n, o, 0);
	return FFH_RES(1);
      }
    }
#endif
  } else {
    const char *p = strdata(lj_lib_checkstr(L, 1));
    char *ep;
    unsigned int neg = 0;
    unsigned long ul;
    if (base < 2 || base > 36)
      lj_err_arg(L, 2, LJ_ERR_BASERNG);
    while (lj_char_isspace((unsigned char)(*p))) p++;
    if (*p == '-') { p++; neg = 1; } else if (*p == '+') { p++; }
    if (lj_char_isalnum((unsigned char)(*p))) {
      ul = strtoul(p, &ep, base);
      if (p != ep) {
	while (lj_char_isspace((unsigned char)(*ep))) ep++;
	if (*ep == '\0') {
	  if (LJ_DUALNUM && LJ_LIKELY(ul < 0x80000000u+neg)) {
	    if (neg) ul = ~ul+1u;
	    setintV(L->base-1-LJ_FR2, (int32_t)ul);
	  } else {
	    lua_Number n = (lua_Number)ul;
	    if (neg) n = -n;
	    setnumV(L->base-1-LJ_FR2, n);
	  }
	  return FFH_RES(1);
	}
      }
    }
  }
  setnilV(L->base-1-LJ_FR2);
  return FFH_RES(1);
}

LJLIB_ASM(tostring)		LJLIB_REC(.)
{
  TValue *o = lj_lib_checkany(L, 1);
  TValue motv;
  cTValue *mo;
  L->top = o+1;  /* Only keep one argument. */
  if (!tvisnil(mo = lj_meta_lookuptv(L, &motv, o, MM_tostring))) {
    copyTV(L, L->base-1-LJ_FR2, mo);  /* Replace callable. */
    return FFH_TAILCALL;
  }
  lj_gc_check(L);
  setstrV(L, L->base-1-LJ_FR2, lj_strfmt_obj(L, L->base));
  return FFH_RES(1);
}

/* -- Base library: throw and catch errors -------------------------------- */

LJLIB_CF(error)
{
  int32_t level = lj_lib_optint(L, 2, 1);
  lua_settop(L, 1);
  if (lua_isstring(L, 1) && level > 0) {
    luaL_where(L, level);
    lua_pushvalue(L, 1);
    lua_concat(L, 2);
  }
  return lua_error(L);
}

LJLIB_ASM(pcall)		LJLIB_REC(.)
{
  lj_lib_checkany(L, 1);
  lj_lib_checkfunc(L, 2);  /* For xpcall only. */
  return FFH_UNREACHABLE;
}
LJLIB_ASM_(xpcall)		LJLIB_REC(.)

/* -- Base library: load Lua code ----------------------------------------- */

static int load_aux(lua_State *L, int status, int envarg)
{
  if (status == LUA_OK) {
    /*
    ** Set environment table for top-level function.
    ** Don't do this for non-native bytecode, which returns a prototype.
    */
    if (tvistab(L->base+envarg-1) && tvisfunc(L->top-1)) {
      GCfunc *fn = funcV(L->top-1);
      GCtab *t = tabV(L->base+envarg-1);
      setgcrefrel(fn->c.env, obj2gco(t));
      lj_gc_pubobjobj(L, fn, t);
    }
    return 1;
  } else {
    setnilV(L->top-2);
    return 2;
  }
}

LJLIB_CF(loadfile)
{
  GCstr *fname = lj_lib_optstr(L, 1);
  GCstr *mode = lj_lib_optstr(L, 2);
  int status;
  lua_settop(L, 3);  /* Ensure env arg exists. */
  status = luaL_loadfilex(L, fname ? strdata(fname) : NULL,
			  mode ? strdata(mode) : NULL);
  return load_aux(L, status, 3);
}

static const char *reader_func(lua_State *L, void *ud, size_t *size)
{
  UNUSED(ud);
  luaL_checkstack(L, 2, "too many nested functions");
  copyTV(L, L->top++, L->base);
  lua_call(L, 0, 1);  /* Call user-supplied function. */
  L->top--;
  if (tvisnil(L->top)) {
    *size = 0;
    return NULL;
  } else if (tvisstr(L->top) || tvisnumber(L->top)) {
    copyTV(L, L->base+4, L->top);  /* Anchor string in reserved stack slot. */
    return lua_tolstring(L, 5, size);
  } else {
    lj_err_caller(L, LJ_ERR_RDRSTR);
    return NULL;
  }
}

LJLIB_CF(load)
{
  GCstr *name = lj_lib_optstr(L, 2);
  GCstr *mode = lj_lib_optstr(L, 3);
  int status;
  if (L->base < L->top &&
      (tvisstr(L->base) || tvisnumber(L->base) || tvisbuf(L->base))) {
    const char *s;
    MSize len;
    if (tvisbuf(L->base)) {
      SBufExt *sbx = bufV(L->base);
      s = sbx->r;
      len = sbufxlen(sbx);
      if (!name) name = &G(L)->strempty;  /* Buffers are not NUL-terminated. */
    } else {
      GCstr *str = lj_lib_checkstr(L, 1);
      s = strdata(str);
      len = str->len;
    }
    lua_settop(L, 4);  /* Ensure env arg exists. */
    status = luaL_loadbufferx(L, s, len, name ? strdata(name) : s,
			      mode ? strdata(mode) : NULL);
  } else {
    lj_lib_checkfunc(L, 1);
    lua_settop(L, 5);  /* Reserve a slot for the string from the reader. */
    status = lua_loadx(L, reader_func, NULL, name ? strdata(name) : "=(load)",
		       mode ? strdata(mode) : NULL);
  }
  return load_aux(L, status, 4);
}

LJLIB_CF(loadstring)
{
  return lj_cf_load(L);
}

LJLIB_CF(dofile)
{
  GCstr *fname = lj_lib_optstr(L, 1);
  setnilV(L->top);
  L->top = L->base+1;
  if (luaL_loadfile(L, fname ? strdata(fname) : NULL) != LUA_OK)
    lua_error(L);
  lua_call(L, 0, LUA_MULTRET);
  return (int)(L->top - L->base) - 1;
}

/* -- Base library: GC control -------------------------------------------- */

#define LUA_GCSTATS		(LUA_GCINCREMENTAL+1)

static void gc_stats_setnum(lua_State *L, GCtab *t, const char *name,
			    uint64_t n)
{
  TValue tv;
  setnumV(&tv, (lua_Number)n);
  copyTVrel(L, lj_tab_setstr(L, t, lj_str_newz(L, name)), &tv);
}

static void gc_stats_setint(lua_State *L, GCtab *t, const char *name,
			    uint32_t n)
{
  TValue tv;
  setintV(&tv, (int32_t)n);
  copyTVrel(L, lj_tab_setstr(L, t, lj_str_newz(L, name)), &tv);
}

static void gc_stats_set_latency_buckets(lua_State *L, GCtab *t,
					 GC2State *gc2)
{
  GCtab *bt;
  TValue tv;
  uint32_t i;
  lua_createtable(L, LJ_GC2_HS_LATENCY_BUCKETS, 0);
  bt = tabV(L->top - 1);
  for (i = 0; i < LJ_GC2_HS_LATENCY_BUCKETS; i++) {
    setnumV(&tv, (lua_Number)la_load64_acq(
      &gc2->hs_ack_latency_buckets[i]));
    copyTVrel(L, lj_tab_setint(L, bt, (int32_t)i + 1), &tv);
  }
  lj_tab_storetab(L, lj_tab_setstr(L, t,
		   lj_str_newlit(L, "poll_ack_latency_buckets")), bt);
  L->top--;
}

static void gc_stats_push(lua_State *L)
{
  global_State *g = G(L);
  GC2State *gc2 = &g->gc2;
  GCtab *t;
  lua_createtable(L, 0, 81);
  t = tabV(L->top - 1);
  gc_stats_setnum(L, t, "total_bytes", g->gc.total);
  gc_stats_setnum(L, t, "total_kbytes", g->gc.total >> 10);
  gc_stats_setint(L, t, "phase", la_load32_acq(&gc2->phase));
  gc_stats_setint(L, t, "generational",
		  la_load32_acq(&gc2->generational));
  gc_stats_setint(L, t, "cycle_minor_requested",
		  la_load32_acq(&gc2->cycle_minor_requested));
  gc_stats_setint(L, t, "cycle_sweep_minor",
		  la_load32_acq(&gc2->cycle_sweep_minor));
  gc_stats_setint(L, t, "minor_sweep_enabled",
		  la_load32_acq(&gc2->minor_sweep_enabled));
  gc_stats_setint(L, t, "cycle_roots_minor",
		  la_load32_acq(&gc2->cycle_roots_minor));
  gc_stats_setint(L, t, "minor_roots_enabled",
		  la_load32_acq(&gc2->minor_roots_enabled));
  gc_stats_setnum(L, t, "cycle_requests", la_load64_acq(&gc2->cycle_requests));
  gc_stats_setnum(L, t, "cycle_starts", la_load64_acq(&gc2->cycle_starts));
  gc_stats_setnum(L, t, "major_cycle_starts",
		  la_load64_acq(&gc2->major_cycle_starts));
  gc_stats_setnum(L, t, "minor_cycle_requests",
		  la_load64_acq(&gc2->minor_cycle_requests));
  gc_stats_setnum(L, t, "minor_cycle_starts",
		  la_load64_acq(&gc2->minor_cycle_starts));
  gc_stats_setnum(L, t, "minor_sweep_deferred",
		  la_load64_acq(&gc2->minor_sweep_deferred));
  gc_stats_setnum(L, t, "minor_sweep_arenas",
		  la_load64_acq(&gc2->minor_sweep_arenas));
  gc_stats_setnum(L, t, "minor_roots_deferred",
		  la_load64_acq(&gc2->minor_roots_deferred));
  gc_stats_setnum(L, t, "major_root_scans",
		  la_load64_acq(&gc2->major_root_scans));
  gc_stats_setnum(L, t, "minor_root_scans",
		  la_load64_acq(&gc2->minor_root_scans));
  gc_stats_setnum(L, t, "minor_survival_base_live",
		  la_load64_acq(&gc2->minor_survival_base_live));
  gc_stats_setnum(L, t, "minor_survival_bytes",
		  la_load64_acq(&gc2->minor_survival_bytes));
  gc_stats_setint(L, t, "minor_survival_pct",
		  la_load32_acq(&gc2->minor_survival_pct));
  gc_stats_setint(L, t, "minor_survival_threshold_pct",
		  la_load32_acq(&gc2->minor_survival_threshold_pct));
  gc_stats_setnum(L, t, "minor_survival_major_requests",
		  la_load64_acq(&gc2->minor_survival_major_requests));
  gc_stats_setnum(L, t, "remembered_barriers",
		  la_load64_acq(&gc2->remembered_barriers));
  gc_stats_setnum(L, t, "remembered_pushed",
		  la_load64_acq(&gc2->remembered_pushed));
  gc_stats_setnum(L, t, "remembered_overflows",
		  la_load64_acq(&gc2->remembered_overflows));
  gc_stats_setnum(L, t, "remembered_filtered",
		  la_load64_acq(&gc2->remembered_filtered));
  gc_stats_setnum(L, t, "remembered_drained",
		  la_load64_acq(&gc2->remembered_drained));
  gc_stats_setnum(L, t, "poll_ack_samples",
		  la_load64_acq(&gc2->hs_ack_latency_samples));
  gc_stats_setnum(L, t, "poll_ack_latency_sum_ns",
		  la_load64_acq(&gc2->hs_ack_latency_sum_ns));
  gc_stats_setnum(L, t, "poll_ack_latency_max_ns",
		  la_load64_acq(&gc2->hs_ack_latency_max_ns));
  gc_stats_set_latency_buckets(L, t, gc2);
  gc_stats_setnum(L, t, "alloc_since_trigger",
		  la_load64_acq(&gc2->alloc_since_trigger));
  gc_stats_setnum(L, t, "cycle_alloc_bytes",
		  la_load64_acq(&gc2->cycle_alloc_bytes));
  gc_stats_setnum(L, t, "trigger_bytes", la_load64_acq(&gc2->trigger_bytes));
  gc_stats_setnum(L, t, "hard_bytes", la_load64_acq(&gc2->hard_bytes));
  gc_stats_setnum(L, t, "assist_runs", la_load64_acq(&gc2->assist_runs));
  gc_stats_setnum(L, t, "assist_grey_drained",
		  la_load64_acq(&gc2->assist_grey_drained));
  gc_stats_setnum(L, t, "assist_ssb_converted",
		  la_load64_acq(&gc2->assist_ssb_converted));
  gc_stats_setnum(L, t, "assist_weak_drained",
		  la_load64_acq(&gc2->assist_weak_drained));
  gc_stats_setnum(L, t, "worker_runs", la_load64_acq(&gc2->worker_runs));
  gc_stats_setnum(L, t, "worker_grey_drained",
		  la_load64_acq(&gc2->worker_grey_drained));
  gc_stats_setnum(L, t, "worker_ssb_converted",
		  la_load64_acq(&gc2->worker_ssb_converted));
  gc_stats_setnum(L, t, "worker_weak_drained",
		  la_load64_acq(&gc2->worker_weak_drained));
  gc_stats_setnum(L, t, "sweep_owner_runs",
		  la_load64_acq(&gc2->sweep_owner_runs));
  gc_stats_setnum(L, t, "sweep_owner_arenas",
		  la_load64_acq(&gc2->sweep_owner_arenas));
  gc_stats_setnum(L, t, "sweep_owner_live_cells",
		  la_load64_acq(&gc2->sweep_owner_live_cells));
  gc_stats_setnum(L, t, "sweep_live_updates",
		  la_load64_acq(&gc2->sweep_live_updates));
  gc_stats_setnum(L, t, "sweep_live_huge_bytes",
		  la_load64_acq(&gc2->sweep_live_huge_bytes));
  gc_stats_setnum(L, t, "live_estimate", la_load64_acq(&gc2->live_estimate));
  gc_stats_setnum(L, t, "weak_clear_tables",
		  la_load64_acq(&gc2->weak_clear_tables));
  gc_stats_setnum(L, t, "weak_clear_cleared",
		  la_load64_acq(&gc2->weak_clear_cleared));
  gc_stats_setnum(L, t, "weak_legacy_fallbacks",
		  la_load64_acq(&gc2->weak_legacy_fallbacks));
  gc_stats_setnum(L, t, "weak_legacy_backfills",
		  la_load64_acq(&gc2->weak_legacy_backfills));
  gc_stats_setnum(L, t, "weak_legacy_backfill_tables",
		  la_load64_acq(&gc2->weak_legacy_backfill_tables));
  gc_stats_setnum(L, t, "weak_legacy_backfill_slots",
		  la_load64_acq(&gc2->weak_legacy_backfill_slots));
  gc_stats_setnum(L, t, "weak_legacy_backfill_cleared",
		  la_load64_acq(&gc2->weak_legacy_backfill_cleared));
  gc_stats_setnum(L, t, "weak_keys_marked",
		  la_load64_acq(&gc2->weak_keys_marked));
  gc_stats_setnum(L, t, "weak_values_marked",
		  la_load64_acq(&gc2->weak_values_marked));
  gc_stats_setnum(L, t, "finreg_cdata_sets",
		  la_load64_acq(&gc2->finreg_cdata_sets));
  gc_stats_setnum(L, t, "finreg_cdata_clears",
		  la_load64_acq(&gc2->finreg_cdata_clears));
  gc_stats_setnum(L, t, "finreg_cdata_queued",
		  la_load64_acq(&gc2->finreg_cdata_queued));
  gc_stats_setnum(L, t, "finreg_cdata_sweep_queued",
		  la_load64_acq(&gc2->finreg_cdata_sweep_queued));
  gc_stats_setnum(L, t, "finreg_cdata_pweak_queued",
		  la_load64_acq(&gc2->finreg_cdata_pweak_queued));
  gc_stats_setnum(L, t, "finreg_cdata_pweak_claimed",
		  la_load64_acq(&gc2->finreg_cdata_pweak_claimed));
  gc_stats_setnum(L, t, "finreg_cdata_preclaim_overflow",
		  la_load64_acq(&gc2->finreg_cdata_preclaim_overflow));
  gc_stats_setnum(L, t, "finreg_cdata_preclaim_dispatched",
		  la_load64_acq(&gc2->finreg_cdata_preclaim_dispatched));
  gc_stats_setnum(L, t, "finreg_cdata_order_seen",
		  la_load64_acq(&gc2->finreg_cdata_order_seen));
  gc_stats_setnum(L, t, "finreg_cdata_order_claimed",
		  la_load64_acq(&gc2->finreg_cdata_order_claimed));
  gc_stats_setnum(L, t, "finreg_cdata_order_unlinked",
		  la_load64_acq(&gc2->finreg_cdata_order_unlinked));
  gc_stats_setnum(L, t, "finreg_cdata_order_queued",
		  la_load64_acq(&gc2->finreg_cdata_order_queued));
  gc_stats_setnum(L, t, "finreg_cdata_order_tombstones",
		  la_load64_acq(&gc2->finreg_cdata_order_tombstones));
  gc_stats_setnum(L, t, "finreg_cdata_order_fallbacks",
		  la_load64_acq(&gc2->finreg_cdata_order_fallbacks));
  gc_stats_setnum(L, t, "finreg_cdata_pweak_root_fallbacks",
		  la_load64_acq(&gc2->finreg_cdata_pweak_root_fallbacks));
  gc_stats_setnum(L, t, "finreg_cdata_pending_order_hits",
		  la_load64_acq(&gc2->finreg_cdata_pending_order_hits));
  gc_stats_setnum(L, t, "finreg_udata_sets",
		  la_load64_acq(&gc2->finreg_udata_sets));
  gc_stats_setnum(L, t, "finreg_udata_clears",
		  la_load64_acq(&gc2->finreg_udata_clears));
  gc_stats_setnum(L, t, "finreg_udata_queued",
		  la_load64_acq(&gc2->finreg_udata_queued));
  gc_stats_setnum(L, t, "finreg_udata_registered",
		  la_load64_acq(&gc2->finreg_udata_registered));
  gc_stats_setnum(L, t, "finreg_udata_discovered",
		  la_load64_acq(&gc2->finreg_udata_discovered));
  gc_stats_setnum(L, t, "finreg_udata_forgets",
		  la_load64_acq(&gc2->finreg_udata_forgets));
  gc_stats_setnum(L, t, "finalizer_queued",
		  la_load64_acq(&gc2->finalizer_queued));
  gc_stats_setnum(L, t, "finalizer_dequeued",
		  la_load64_acq(&gc2->finalizer_dequeued));
  gc_stats_setnum(L, t, "finalizer_mpsc_drained",
		  la_load64_acq(&gc2->finalizer_mpsc_drained));
  gc_stats_setnum(L, t, "finalizer_enters",
		  la_load64_acq(&gc2->finalizer_enters));
  gc_stats_setnum(L, t, "finalizer_leaves",
		  la_load64_acq(&gc2->finalizer_leaves));
  gc_stats_setnum(L, t, "finalizer_sweep_blocks",
		  la_load64_acq(&gc2->finalizer_sweep_blocks));
  gc_stats_setnum(L, t, "finalizer_spawn_deferrals",
		  la_load64_acq(&gc2->finalizer_spawn_deferrals));
}

LJLIB_CF(gcinfo)
{
  setintV(L->top++, (int32_t)(G(L)->gc.total >> 10));
  return 1;
}

LJLIB_CF(collectgarbage)
{
  int opt = lj_lib_checkopt(L, 1, LUA_GCCOLLECT,  /* ORDER LUA_GC* */
    "\4stop\7restart\7collect\5count\1\377\4step\10setpause\12setstepmul\1\377\11isrunning\14generational\13incremental\5stats");
  int32_t data = lj_lib_optint(L, 2, 0);
  if (opt == LUA_GCCOUNT) {
    setnumV(L->top, (lua_Number)G(L)->gc.total/1024.0);
  } else if (opt == LUA_GCSTATS) {
    gc_stats_push(L);
    return 1;
  } else if (opt == LUA_GCGENERATIONAL || opt == LUA_GCINCREMENTAL) {
    int res = lua_gc(L, opt, data);
    setstrV(L, L->top,
	    lj_str_newz(L, res ? "generational" : "incremental"));
  } else {
    int res = lua_gc(L, opt, data);
    if (opt == LUA_GCSTEP || opt == LUA_GCISRUNNING)
      setboolV(L->top, res);
    else
      setintV(L->top, res);
  }
  L->top++;
  return 1;
}

/* -- Base library: miscellaneous functions ------------------------------- */

LJLIB_PUSH(top-2)  /* Upvalue holds weak table. */
LJLIB_CF(newproxy)
{
  lua_settop(L, 1);
  lua_newuserdata(L, 0);
  if (lua_toboolean(L, 1) == 0) {  /* newproxy(): without metatable. */
    return 1;
  } else if (lua_isboolean(L, 1)) {  /* newproxy(true): with metatable. */
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_pushboolean(L, 1);
    lua_rawset(L, lua_upvalueindex(1));  /* Remember mt in weak table. */
  } else {  /* newproxy(proxy): inherit metatable. */
    int validproxy = 0;
    if (lua_getmetatable(L, 1)) {
      lua_rawget(L, lua_upvalueindex(1));
      validproxy = lua_toboolean(L, -1);
      lua_pop(L, 1);
    }
    if (!validproxy)
      lj_err_arg(L, 1, LJ_ERR_NOPROXY);
    lua_getmetatable(L, 1);
  }
  lua_setmetatable(L, 2);
  return 1;
}

LJLIB_PUSH("tostring")
LJLIB_CF(print)
{
  ptrdiff_t i, nargs = L->top - L->base;
  cTValue *tv = lj_tab_getstr(tabref_acq(L->env),
			      strV(lj_lib_upvalue(L, 1)));
  int shortcut;
  if (tv && !tvisnil(tv)) {
    copyTV(L, L->top++, tv);
  } else {
    setstrV(L, L->top++, strV(lj_lib_upvalue(L, 1)));
    lua_gettable(L, LUA_GLOBALSINDEX);
    tv = L->top-1;
  }
  shortcut = (tvisfunc(tv) && funcV(tv)->c.ffid == FF_tostring) &&
	     !gcrefu(basemt_it(G(L), LJ_TNUMX));
  for (i = 0; i < nargs; i++) {
    cTValue *o = &L->base[i];
    const char *str;
    size_t size;
    MSize len;
    if (shortcut && (str = lj_strfmt_wstrnum(L, o, &len)) != NULL) {
      size = len;
    } else {
      copyTV(L, L->top+1, o);
      copyTV(L, L->top, L->top-1);
      L->top += 2;
      lua_call(L, 1, 1);
      str = lua_tolstring(L, -1, &size);
      if (!str)
	lj_err_caller(L, LJ_ERR_PRTOSTR);
      L->top--;
    }
    if (i)
      putchar('\t');
    fwrite(str, 1, size, stdout);
  }
  putchar('\n');
  return 0;
}

LJLIB_PUSH(top-3)
LJLIB_SET(_VERSION)

#include "lj_libdef.h"

/* -- Coroutine library --------------------------------------------------- */

#define LJLIB_MODULE_coroutine

LJLIB_CF(coroutine_status)
{
  LJStateClaim claim;
  const char *s;
  lua_State *co;
  if (!(L->top > L->base && tvisthread(L->base)))
    lj_err_arg(L, 1, LJ_ERR_NOCORO);
  co = threadV(L->base);
  if (co == L) s = "running";
  else {
    uint32_t tid = lj_thr_current_id(G(L));
    if (!lj_state_tryclaim(co, tid, &claim))
      lj_err_callermsg(L, "thread busy");
    if (co->status == LUA_YIELD) s = "suspended";
    else if (co->status != LUA_OK) s = "dead";
    else if (co->base > tvref(co->stack)+1+LJ_FR2) s = "normal";
    else if (co->top == co->base) s = "dead";
    else s = "suspended";
    lj_state_dropclaim(&claim);
  }
  lua_pushstring(L, s);
  return 1;
}

LJLIB_CF(coroutine_running)
{
#if LJ_52
  int ismain = lua_pushthread(L);
  setboolV(L->top++, ismain);
  return 2;
#else
  if (lua_pushthread(L))
    setnilV(L->top++);
  return 1;
#endif
}

LJLIB_CF(coroutine_isyieldable)
{
  setboolV(L->top++, cframe_canyield(L->cframe));
  return 1;
}

LJLIB_CF(coroutine_create)
{
  lua_State *L1;
  if (!(L->base < L->top && tvisfunc(L->base)))
    lj_err_argt(L, 1, LUA_TFUNCTION);
  L1 = lua_newthread(L);
  setfuncV(L, L1->top++, funcV(L->base));
  return 1;
}

LJLIB_ASM(coroutine_yield)
{
  lj_err_caller(L, LJ_ERR_CYIELD);
  return FFH_UNREACHABLE;
}

static int ffh_resume(lua_State *L, lua_State *co, int wrap)
{
  LJStateClaim claim;
  if (!lj_state_tryclaim(co, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(L, "thread busy");
  if (co->cframe != NULL || co->status > LUA_YIELD ||
      (co->status == LUA_OK && co->top == co->base)) {
    ErrMsg em = co->cframe ? LJ_ERR_CORUN : LJ_ERR_CODEAD;
    lj_state_dropclaim(&claim);
    if (wrap)
      lj_err_caller(L, em);
    setboolV(L->base-1-LJ_FR2, 0);
    setstrV(L, L->base-LJ_FR2, lj_err_str(L, em));
    return FFH_RES(2);
  }
  if (lj_state_cpgrowstack(co, (MSize)(L->top - L->base)) != LUA_OK) {
    cTValue *msg = --co->top;
    setstrV(L, L->top++, strV(msg));
    lj_state_dropclaim(&claim);
    lj_err_callermsg(L, strVdata(L->top-1));
  }
  lj_state_dropclaim(&claim);
  return FFH_RETRY;
}

LJLIB_ASM(coroutine_resume)
{
  if (!(L->top > L->base && tvisthread(L->base)))
    lj_err_arg(L, 1, LJ_ERR_NOCORO);
  return ffh_resume(L, threadV(L->base), 0);
}

LJLIB_NOREG LJLIB_ASM(coroutine_wrap_aux)
{
  return ffh_resume(L, threadV(lj_lib_upvalue(L, 1)), 1);
}

/* Inline declarations. */
LJ_ASMF void lj_ff_coroutine_wrap_aux(void);
LJ_ASMF lua_State *LJ_FASTCALL lj_ffh_coroutine_claim(lua_State *L,
						      lua_State *co);
#if !(LJ_TARGET_MIPS && defined(ljamalg_c))
LJ_FUNCA_NORET void LJ_FASTCALL lj_ffh_coroutine_wrap_err(lua_State *L,
							  lua_State *co);
#endif

lua_State *LJ_FASTCALL lj_ffh_coroutine_claim(lua_State *L, lua_State *co)
{
  LJStateClaim claim;
  uintptr_t coflag;
  if (!lj_state_tryclaim(co, lj_thr_current_id(G(L)), &claim))
    return NULL;
  coflag = (uintptr_t)co | (uintptr_t)claim.release;
  return (lua_State *)coflag;
}

/* Error handler, called from assembler VM. */
void LJ_FASTCALL lj_ffh_coroutine_wrap_err(lua_State *L, lua_State *co)
{
  uintptr_t coflag = (uintptr_t)co;
  co = (lua_State *)(coflag & ~(uintptr_t)1);
  co->top--; copyTV(L, L->top, co->top); L->top++;
  if (coflag & 1)
    lj_state_release(co, lj_thr_current_id(G(L)));
  if (tvisstr(L->top-1))
    lj_err_callermsg(L, strVdata(L->top-1));
  else
    lj_err_run(L);
}

/* Forward declaration. */
static void setpc_wrap_aux(lua_State *L, GCfunc *fn);

LJLIB_CF(coroutine_wrap)
{
  GCfunc *fn;
  lj_cf_coroutine_create(L);
  fn = lj_lib_pushcc(L, lj_ffh_coroutine_wrap_aux, FF_coroutine_wrap_aux, 1);
  setpc_wrap_aux(L, fn);
  return 1;
}

#include "lj_libdef.h"

/* Fix the PC of wrap_aux. Really ugly workaround. */
static void setpc_wrap_aux(lua_State *L, GCfunc *fn)
{
  setmref(fn->c.pc, &L2GG(L)->bcff[lj_lib_init_coroutine[1]+2]);
}

/* ------------------------------------------------------------------------ */

static void base_storestr_str(lua_State *L, GCtab *tab, GCstr *key, GCstr *val)
{
  TValue tv, *dst;
  setstrV(L, &tv, val);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK)
      return;
    la_cpu_pause();  /* base string store saw FORWARD after lookup. */
  }
}

static void newproxy_weaktable(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 1);
  settabV(L, L->top++, t);
  setgcrefmt(t->metatable, obj2gco(t));
  base_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "kv"));
  t->nomm = (uint8_t)(~(1u<<MM_mode));
  lj_gc_pubtab(L, t);
}

static void base_storetab_str(lua_State *L, GCtab *tab, GCstr *key, GCtab *val)
{
  TValue tv, *dst;
  settabV(L, &tv, val);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK)
      return;
    la_cpu_pause();  /* base table store saw FORWARD after lookup. */
  }
}

LUALIB_API int luaopen_base(lua_State *L)
{
  /* NOBARRIER: Table and value are the same. */
  GCtab *env = tabref_acq(L->env);
  base_storetab_str(L, env, lj_str_newlit(L, "_G"), env);
  lua_pushliteral(L, LUA_VERSION);  /* top-3. */
  newproxy_weaktable(L);  /* top-2. */
  LJ_LIB_REG(L, "_G", base);
  LJ_LIB_REG(L, LUA_COLIBNAME, coroutine);
  return 2;
}
