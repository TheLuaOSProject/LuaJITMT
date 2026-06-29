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
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_thr.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_trace.h"
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

static LJ_AINLINE void lib_trace_flush_env(lua_State *L)
{
  if (lj_trace_flushall_hs(L))
    lj_err_caller(L, LJ_ERR_NOGCMM);
}

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
    TValue uv;
    if (!tvistab(o)) lj_err_argt(L, 1, LUA_TTABLE);
    if (LJ_FR2) { copyTV(L, o-1, o); o--; }
    lj_lib_upvalue_load_acq(L, 1, &uv);
    setfuncV(L, o-1, funcV(&uv));
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
    lj_tab_nomm_rel(mt, 0);  /* Do not trust stale metamethod miss caches. */
  lj_tab_metatable_rel(t, mt);
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
  settabV(L, L->top++, isluafunc(fn) ? lj_func_env_acq(fn) :
				       lj_state_env_acq(L));
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
      lib_trace_flush_env(L);
      /* NOBARRIER: A thread (i.e. L) is never black. */
      lj_state_env_rel(L, t);
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
  lib_trace_flush_env(L);
  lj_func_env_rel(fn, t);
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
      lj_func_env_rel(fn, t);
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
      s = lj_bufx_data_acq(sbx, &len);
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
#define LUA_GCWORKERS		(LUA_GCSTATS+1)

static TValue *gc_stats_storetv_str(lua_State *L, GCtab *t, const char *name,
				    cTValue *src)
{
  GCstr *key = lj_str_newz(L, name);
  TValue keytv, *dst;
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, t, key);
    if (lj_tab_trystoretv_cas_keyed(L, t, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      return dst;
    lj_tab_store_wait_no_l();  /* GC stats string store saw stale/FORWARD slot. */
  }
}

static TValue *gc_stats_storetv_int(lua_State *L, GCtab *t, int32_t key,
				    cTValue *src)
{
  TValue keytv, *dst;
  setintV(&keytv, key);
  for (;;) {
    dst = lj_tab_setint(L, t, key);
    if (lj_tab_trystoretv_cas_keyed(L, t, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      return dst;
    lj_tab_store_wait_no_l();  /* GC stats int store saw stale/FORWARD slot. */
  }
}

static void gc_stats_setnum(lua_State *L, GCtab *t, const char *name,
			    uint64_t n)
{
  TValue tv;
  setnumV(&tv, (lua_Number)n);
  gc_stats_storetv_str(L, t, name, &tv);
}

static void gc_stats_setint(lua_State *L, GCtab *t, const char *name,
			    uint32_t n)
{
  TValue tv;
  setintV(&tv, (int32_t)n);
  gc_stats_storetv_str(L, t, name, &tv);
}

static void gc_stats_set_latency_buckets(lua_State *L, GCtab *t,
					 const GC2StatsSnapshot *s)
{
  GCtab *bt;
  TValue tv;
  uint32_t i;
  lua_createtable(L, LJ_GC2_HS_LATENCY_BUCKETS, 0);
  bt = tabV(L->top - 1);
  for (i = 0; i < LJ_GC2_HS_LATENCY_BUCKETS; i++) {
    setnumV(&tv, (lua_Number)s->poll_ack_latency_buckets[i]);
    gc_stats_storetv_int(L, bt, (int32_t)i + 1, &tv);
  }
  lj_gc_pubtab(L, bt);
  settabV(L, &tv, bt);
  gc_stats_storetv_str(L, t, "poll_ack_latency_buckets", &tv);
  lj_gc_pubtabobj(L, t, bt);
  L->top--;
}

static void gc_stats_push(lua_State *L)
{
  global_State *g = G(L);
  GC2StatsSnapshot s;
  GCtab *t;
  lj_gc2_stats_snapshot(g, &s);
  lua_createtable(L, 0, 86);
  t = tabV(L->top - 1);
  gc_stats_setnum(L, t, "total_bytes", s.total_bytes);
  gc_stats_setnum(L, t, "total_kbytes", s.total_bytes >> 10);
  gc_stats_setint(L, t, "phase", s.phase);
  gc_stats_setint(L, t, "generational", s.generational);
  gc_stats_setint(L, t, "cycle_minor_requested", s.cycle_minor_requested);
  gc_stats_setint(L, t, "cycle_sweep_minor", s.cycle_sweep_minor);
  gc_stats_setint(L, t, "minor_sweep_enabled", s.minor_sweep_enabled);
  gc_stats_setint(L, t, "cycle_roots_minor", s.cycle_roots_minor);
  gc_stats_setint(L, t, "minor_roots_enabled", s.minor_roots_enabled);
  gc_stats_setnum(L, t, "cycle_requests", s.cycle_requests);
  gc_stats_setnum(L, t, "cycle_starts", s.cycle_starts);
  gc_stats_setnum(L, t, "major_cycle_starts", s.major_cycle_starts);
  gc_stats_setnum(L, t, "minor_cycle_requests", s.minor_cycle_requests);
  gc_stats_setnum(L, t, "minor_cycle_starts", s.minor_cycle_starts);
  gc_stats_setnum(L, t, "minor_sweep_deferred", s.minor_sweep_deferred);
  gc_stats_setnum(L, t, "minor_sweep_arenas", s.minor_sweep_arenas);
  gc_stats_setnum(L, t, "minor_roots_deferred", s.minor_roots_deferred);
  gc_stats_setnum(L, t, "major_root_scans", s.major_root_scans);
  gc_stats_setnum(L, t, "minor_root_scans", s.minor_root_scans);
  gc_stats_setnum(L, t, "minor_survival_base_live",
		  s.minor_survival_base_live);
  gc_stats_setnum(L, t, "minor_survival_bytes", s.minor_survival_bytes);
  gc_stats_setint(L, t, "minor_survival_pct", s.minor_survival_pct);
  gc_stats_setint(L, t, "minor_survival_threshold_pct",
		  s.minor_survival_threshold_pct);
  gc_stats_setnum(L, t, "minor_survival_major_requests",
		  s.minor_survival_major_requests);
  gc_stats_setnum(L, t, "remembered_barriers", s.remembered_barriers);
  gc_stats_setnum(L, t, "remembered_pushed", s.remembered_pushed);
  gc_stats_setnum(L, t, "remembered_overflows", s.remembered_overflows);
  gc_stats_setnum(L, t, "remembered_filtered", s.remembered_filtered);
  gc_stats_setnum(L, t, "remembered_drained", s.remembered_drained);
  gc_stats_setnum(L, t, "poll_ack_samples", s.poll_ack_samples);
  gc_stats_setnum(L, t, "poll_ack_latency_sum_ns",
		  s.poll_ack_latency_sum_ns);
  gc_stats_setnum(L, t, "poll_ack_latency_max_ns",
		  s.poll_ack_latency_max_ns);
  gc_stats_set_latency_buckets(L, t, &s);
  gc_stats_setnum(L, t, "alloc_since_trigger", s.alloc_since_trigger);
  gc_stats_setnum(L, t, "cycle_alloc_bytes", s.cycle_alloc_bytes);
  gc_stats_setnum(L, t, "trigger_bytes", s.trigger_bytes);
  gc_stats_setnum(L, t, "hard_bytes", s.hard_bytes);
  gc_stats_setnum(L, t, "assist_runs", s.assist_runs);
  gc_stats_setnum(L, t, "assist_grey_drained", s.assist_grey_drained);
  gc_stats_setnum(L, t, "assist_ssb_converted", s.assist_ssb_converted);
  gc_stats_setnum(L, t, "assist_weak_drained", s.assist_weak_drained);
  gc_stats_setnum(L, t, "worker_runs", s.worker_runs);
  gc_stats_setnum(L, t, "worker_grey_drained", s.worker_grey_drained);
  gc_stats_setnum(L, t, "worker_ssb_converted", s.worker_ssb_converted);
  gc_stats_setnum(L, t, "worker_weak_drained", s.worker_weak_drained);
  gc_stats_setnum(L, t, "worker_idle_declares", s.worker_idle_declares);
  gc_stats_setnum(L, t, "worker_busy_retries", s.worker_busy_retries);
  gc_stats_setnum(L, t, "worker_wakes", s.worker_wakes);
  gc_stats_setnum(L, t, "worker_parks", s.worker_parks);
  gc_stats_setnum(L, t, "worker_async_progress", s.worker_async_progress);
  gc_stats_setnum(L, t, "sweep_owner_runs", s.sweep_owner_runs);
  gc_stats_setnum(L, t, "sweep_owner_arenas", s.sweep_owner_arenas);
  gc_stats_setnum(L, t, "sweep_owner_live_cells", s.sweep_owner_live_cells);
  gc_stats_setnum(L, t, "sweep_live_updates", s.sweep_live_updates);
  gc_stats_setnum(L, t, "sweep_live_huge_bytes", s.sweep_live_huge_bytes);
  gc_stats_setnum(L, t, "live_estimate", s.live_estimate);
  gc_stats_setnum(L, t, "weak_clear_tables", s.weak_clear_tables);
  gc_stats_setnum(L, t, "weak_clear_cleared", s.weak_clear_cleared);
  gc_stats_setnum(L, t, "weak_bridge_skipped", s.weak_bridge_skipped);
  gc_stats_setnum(L, t, "weak_bridge_fallbacks", s.weak_bridge_fallbacks);
  gc_stats_setnum(L, t, "weak_bridge_backfills", s.weak_bridge_backfills);
  gc_stats_setnum(L, t, "weak_bridge_backfill_tables",
		  s.weak_bridge_backfill_tables);
  gc_stats_setnum(L, t, "weak_bridge_backfill_slots",
		  s.weak_bridge_backfill_slots);
  gc_stats_setnum(L, t, "weak_bridge_backfill_cleared",
		  s.weak_bridge_backfill_cleared);
  gc_stats_setnum(L, t, "weak_keys_marked", s.weak_keys_marked);
  gc_stats_setnum(L, t, "weak_values_marked", s.weak_values_marked);
  gc_stats_setnum(L, t, "finreg_cdata_sets", s.finreg_cdata_sets);
  gc_stats_setnum(L, t, "finreg_cdata_clears", s.finreg_cdata_clears);
  gc_stats_setnum(L, t, "finreg_cdata_queued", s.finreg_cdata_queued);
  gc_stats_setnum(L, t, "finreg_cdata_sweep_queued",
		  s.finreg_cdata_sweep_queued);
  gc_stats_setnum(L, t, "finreg_cdata_pweak_queued",
		  s.finreg_cdata_pweak_queued);
  gc_stats_setnum(L, t, "finreg_cdata_pweak_claimed",
		  s.finreg_cdata_pweak_claimed);
  gc_stats_setnum(L, t, "finreg_cdata_preclaim_overflow",
		  s.finreg_cdata_preclaim_overflow);
  gc_stats_setnum(L, t, "finreg_cdata_preclaim_dispatched",
		  s.finreg_cdata_preclaim_dispatched);
  gc_stats_setnum(L, t, "finreg_cdata_order_seen",
		  s.finreg_cdata_order_seen);
  gc_stats_setnum(L, t, "finreg_cdata_order_claimed",
		  s.finreg_cdata_order_claimed);
  gc_stats_setnum(L, t, "finreg_cdata_order_unlinked",
		  s.finreg_cdata_order_unlinked);
  gc_stats_setnum(L, t, "finreg_cdata_order_queued",
		  s.finreg_cdata_order_queued);
  gc_stats_setnum(L, t, "finreg_cdata_order_retired",
		  s.finreg_cdata_order_retired);
  gc_stats_setnum(L, t, "finreg_cdata_order_tombstones",
		  s.finreg_cdata_order_tombstones);
  gc_stats_setnum(L, t, "finreg_cdata_order_fallbacks",
		  s.finreg_cdata_order_fallbacks);
  gc_stats_setnum(L, t, "finreg_cdata_pending_order_hits",
		  s.finreg_cdata_pending_order_hits);
  gc_stats_setnum(L, t, "finreg_udata_sets", s.finreg_udata_sets);
  gc_stats_setnum(L, t, "finreg_udata_clears", s.finreg_udata_clears);
  gc_stats_setnum(L, t, "finreg_udata_queued", s.finreg_udata_queued);
  gc_stats_setnum(L, t, "finreg_udata_registered",
		  s.finreg_udata_registered);
  gc_stats_setnum(L, t, "finreg_udata_retired_nodes",
		  s.finreg_udata_retired_nodes);
  gc_stats_setnum(L, t, "finreg_udata_discovered",
		  s.finreg_udata_discovered);
  gc_stats_setnum(L, t, "finreg_udata_forgets", s.finreg_udata_forgets);
  gc_stats_setnum(L, t, "finalizer_queued", s.finalizer_queued);
  gc_stats_setnum(L, t, "finalizer_dequeued", s.finalizer_dequeued);
  gc_stats_setnum(L, t, "finalizer_mpsc_drained",
		  s.finalizer_mpsc_drained);
  gc_stats_setnum(L, t, "finalizer_enters", s.finalizer_enters);
  gc_stats_setnum(L, t, "finalizer_leaves", s.finalizer_leaves);
  gc_stats_setnum(L, t, "finalizer_sweep_blocks",
		  s.finalizer_sweep_blocks);
  gc_stats_setnum(L, t, "finalizer_spawn_deferrals",
		  s.finalizer_spawn_deferrals);
  gc_stats_setnum(L, t, "finalizer_spawn_release_wakes",
		  s.finalizer_spawn_release_wakes);
  lj_gc_pubtab(L, t);
}

LJLIB_CF(gcinfo)
{
  setintV(L->top++, (int32_t)(lj_gc_total_load(G(L)) >> 10));
  return 1;
}

LJLIB_CF(collectgarbage)
{
  int opt = lj_lib_checkopt(L, 1, LUA_GCCOLLECT,  /* ORDER LUA_GC* */
    "\4stop\7restart\7collect\5count\1\377\4step\10setpause\12setstepmul\1\377\11isrunning\14generational\13incremental\5stats\7workers");
  int hasdata = L->base+1 < L->top && !tvisnil(L->base+1);
  int32_t data = lj_lib_optint(L, 2, 0);
  if (opt == LUA_GCCOUNT) {
    setnumV(L->top, (lua_Number)lj_gc_total_load(G(L))/1024.0);
  } else if (opt == LUA_GCSTATS) {
    gc_stats_push(L);
    return 1;
  } else if (opt == LUA_GCWORKERS) {
    global_State *g = G(L);
    uint32_t old = lj_gc2_workers_count(g);
    uint32_t actions = 0;
    if (hasdata) {
      int ok = lj_gc2_workers_set_l(L, data <= 0 ? 0u : (uint32_t)data,
				    &actions);
      lj_safepoint_checkstop(L, actions);
      if (!ok)
	lj_err_callermsg(L, "cannot start GC worker");
    }
    setintV(L->top, (int32_t)old);
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

static int print_had_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int print_fresh_stopreq(lua_State *L, uint32_t actions,
			       int had_stopreq)
{
  TGState *tg = L2TG(L);
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

static void print_checkstop_fresh(lua_State *L, uint32_t actions,
				  int had_stopreq)
{
  if (print_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

static void print_native_write(lua_State *L, const char *str, size_t size)
{
  uint32_t actions;
  int had_stopreq;
  if (size == 0)
    return;
  had_stopreq = print_had_stopreq(L);
  lj_native_enter(L2TG(L));
  (void)fwrite(str, 1, size, stdout);
  actions = lj_native_leave(L);
  print_checkstop_fresh(L, actions, had_stopreq);
}

static void print_native_char(lua_State *L, int c)
{
  char ch = (char)c;
  print_native_write(L, &ch, 1);
}

LJLIB_CF(print)
{
  ptrdiff_t i, nargs = L->top - L->base;
  TValue uv;
  TValue tvsnap;
  GCstr *tostring_str;
  cTValue *tv;
  int shortcut;
  lj_lib_upvalue_load_acq(L, 1, &uv);
  tostring_str = strV(&uv);
  tv = lj_tab_getstr(lj_state_env_acq(L), tostring_str);
  if (tv) {
    lj_tv_load_acq(&tvsnap, tv);
    if (!tvisnil(&tvsnap)) {
      copyTV(L, L->top, &tvsnap);
      tv = L->top++;
    } else {
      tv = NULL;
    }
  }
  if (!tv) {
    setstrV(L, L->top++, tostring_str);
    lua_gettable(L, LUA_GLOBALSINDEX);
    tv = L->top-1;
  }
  shortcut = (tvisfunc(tv) && funcV(tv)->c.ffid == FF_tostring) &&
	     !lj_basemt_it_acq(G(L), LJ_TNUMX);
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
      print_native_char(L, '\t');
    print_native_write(L, str, size);
  }
  print_native_char(L, '\n');
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
  TValue uv;
  lj_lib_upvalue_load_acq(L, 1, &uv);
  return ffh_resume(L, threadV(&uv), 1);
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
  TValue keytv, tv, *dst;
  setstrV(L, &tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_no_l();  /* Base string store saw stale/FORWARD slot. */
  }
}

static void newproxy_weaktable(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 1);
  settabV(L, L->top++, t);
  lj_tab_metatable_rel(t, t);
  base_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "kv"));
  lj_tab_nomm_rel(t, (uint8_t)(~(1u<<MM_mode)));
  lj_gc_pubtab(L, t);
}

static void base_storetab_str(lua_State *L, GCtab *tab, GCstr *key, GCtab *val)
{
  TValue keytv, tv, *dst;
  settabV(L, &tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_no_l();  /* Base table store saw stale/FORWARD slot. */
  }
}

LUALIB_API int luaopen_base(lua_State *L)
{
  /* NOBARRIER: Table and value are the same. */
  GCtab *env = lj_state_env_acq(L);
  base_storetab_str(L, env, lj_str_newlit(L, "_G"), env);
  lua_pushliteral(L, LUA_VERSION);  /* top-3. */
  newproxy_weaktable(L);  /* top-2. */
  LJ_LIB_REG(L, "_G", base);
  LJ_LIB_REG(L, LUA_COLIBNAME, coroutine);
  return 2;
}
