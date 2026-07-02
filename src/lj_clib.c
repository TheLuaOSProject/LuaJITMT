/*
** FFI C library loader.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_udata.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#include "lj_strfmt.h"
#include "lj_tg.h"
#include "lj_thr.h"

static int clib_had_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int clib_fresh_stopreq(lua_State *L, uint32_t actions,
			      int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static void clib_checkstop_fresh(lua_State *L, uint32_t actions,
				 int had_stopreq)
{
  if (clib_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

/* -- OS-specific functions ----------------------------------------------- */

#if LJ_TARGET_DLOPEN

#include <dlfcn.h>
#include <stdio.h>

#if defined(RTLD_DEFAULT) && !defined(NO_RTLD_DEFAULT)
#define CLIB_DEFHANDLE	RTLD_DEFAULT
#elif LJ_TARGET_OSX || LJ_TARGET_BSD
#define CLIB_DEFHANDLE	((void *)(intptr_t)-2)
#else
#define CLIB_DEFHANDLE	NULL
#endif

LJ_NORET LJ_NOINLINE static void clib_error_(lua_State *L)
{
  lj_err_callermsg(L, dlerror());
}

#define clib_error(L, fmt, name)	clib_error_(L)

#if LJ_TARGET_CYGWIN
#define CLIB_SOPREFIX	"cyg"
#else
#define CLIB_SOPREFIX	"lib"
#endif

#if LJ_TARGET_OSX
#define CLIB_SOEXT	"%s.dylib"
#elif LJ_TARGET_CYGWIN
#define CLIB_SOEXT	"%s.dll"
#else
#define CLIB_SOEXT	"%s.so"
#endif

static const char *clib_extname(lua_State *L, const char *name)
{
  if (!strchr(name, '/')
#if LJ_TARGET_CYGWIN
      && !strchr(name, '\\')
#endif
     ) {
    if (!strchr(name, '.')) {
      name = lj_strfmt_pushf(L, CLIB_SOEXT, name);
      L->top--;
#if LJ_TARGET_CYGWIN
    } else {
      return name;
#endif
    }
    if (!(name[0] == CLIB_SOPREFIX[0] && name[1] == CLIB_SOPREFIX[1] &&
	  name[2] == CLIB_SOPREFIX[2])) {
      name = lj_strfmt_pushf(L, CLIB_SOPREFIX "%s", name);
      L->top--;
    }
  }
  return name;
}

/* Check for a recognized ld script line. */
static const char *clib_check_lds(lua_State *L, const char *buf)
{
  const char *p, *e;
  if ((!strncmp(buf, "GROUP", 5) || !strncmp(buf, "INPUT", 5)) &&
      (p = strchr(buf, '('))) {
    while (*++p == ' ') ;
    for (e = p; *e && *e != ' ' && *e != ')'; e++) ;
    return strdata(lj_str_new(L, p, e-p));
  }
  return NULL;
}

static FILE *clib_native_fopen(lua_State *L, const char *name,
			       uint32_t *actionsp)
{
  FILE *fp;
  lj_native_enter(L2TG(L));
  fp = fopen(name, "r");
  *actionsp = lj_native_leave(L);
  return fp;
}

static char *clib_native_fgets(lua_State *L, char *buf, int size, FILE *fp,
			       uint32_t *actionsp)
{
  char *p;
  lj_native_enter(L2TG(L));
  p = fgets(buf, size, fp);
  *actionsp = lj_native_leave(L);
  return p;
}

static uint32_t clib_native_fclose(lua_State *L, FILE *fp)
{
  uint32_t actions;
  lj_native_enter(L2TG(L));
  (void)fclose(fp);
  actions = lj_native_leave(L);
  return actions;
}

static void clib_lds_checkstop(lua_State *L, FILE *fp, uint32_t actions,
			       int had_stopreq)
{
  if (clib_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t close_actions = clib_native_fclose(L, fp);
    lj_safepoint_checkstop(L, actions | close_actions);
  }
}

/* Quick and dirty solution to resolve shared library name from ld script. */
static const char *clib_resolve_lds(lua_State *L, const char *name)
{
  uint32_t actions;
  int had_stopreq = clib_had_stopreq(L);
  FILE *fp = clib_native_fopen(L, name, &actions);
  const char *p = NULL;
  if (clib_fresh_stopreq(L, actions, had_stopreq)) {
    if (fp) {
      uint32_t close_actions = clib_native_fclose(L, fp);
      lj_safepoint_checkstop(L, actions | close_actions);
    } else {
      clib_checkstop_fresh(L, actions, had_stopreq);
    }
  }
  if (fp) {
    char buf[256];
    if (clib_native_fgets(L, buf, sizeof(buf), fp, &actions)) {
      clib_lds_checkstop(L, fp, actions, had_stopreq);
      if (!strncmp(buf, "/* GNU ld script", 16)) {  /* ld script magic? */
	while (clib_native_fgets(L, buf, sizeof(buf), fp, &actions)) {
	  clib_lds_checkstop(L, fp, actions, had_stopreq);  /* Check all lines. */
	  p = clib_check_lds(L, buf);
	  if (p) break;
	}
      } else {  /* Otherwise check only the first line. */
	p = clib_check_lds(L, buf);
      }
    }
    actions = clib_native_fclose(L, fp);
    clib_checkstop_fresh(L, actions, had_stopreq);
  }
  return p;
}

static void *clib_native_dlopen(lua_State *L, const char *name, int flags,
				uint32_t *actionsp)
{
  void *h;
  lj_native_enter(L2TG(L));
  h = dlopen(name, flags);
  *actionsp = lj_native_leave(L);
  return h;
}

static uint32_t clib_native_dlclose(lua_State *L, void *handle)
{
  uint32_t actions;
  if (!L) {
    (void)dlclose(handle);
    return 0;
  }
  lj_native_enter(L2TG(L));
  (void)dlclose(handle);
  actions = lj_native_leave(L);
  return actions;
}

static void *clib_native_dlsym(lua_State *L, void *handle, const char *name,
			       uint32_t *actionsp)
{
  void *p;
  lj_native_enter(L2TG(L));
  p = dlsym(handle, name);
  *actionsp = lj_native_leave(L);
  return p;
}

static void *clib_loadlib(lua_State *L, const char *name, int global)
{
  uint32_t actions;
  int had_stopreq = clib_had_stopreq(L);
  void *h = clib_native_dlopen(L, clib_extname(L, name),
			       RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL),
			       &actions);
  if (!h) {
    const char *e, *err = dlerror();
    clib_checkstop_fresh(L, actions, had_stopreq);
    if (err && *err == '/' && (e = strchr(err, ':')) &&
	(name = clib_resolve_lds(L, strdata(lj_str_new(L, err, e-err))))) {
      had_stopreq = clib_had_stopreq(L);
      h = clib_native_dlopen(L, name,
			     RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL),
			     &actions);
      if (h) {
	if (clib_fresh_stopreq(L, actions, had_stopreq)) {
	  uint32_t close_actions = clib_native_dlclose(L, h);
	  lj_safepoint_checkstop(L, actions | close_actions);
	}
	return h;
      }
      err = dlerror();
      clib_checkstop_fresh(L, actions, had_stopreq);
    }
    if (!err) err = "dlopen failed";
    lj_err_callermsg(L, err);
  } else if (clib_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t close_actions = clib_native_dlclose(L, h);
    lj_safepoint_checkstop(L, actions | close_actions);
  }
  return h;
}

static uint32_t clib_unloadlib(lua_State *L, CLibrary *cl)
{
  if (cl->handle && cl->handle != CLIB_DEFHANDLE)
    return clib_native_dlclose(L, cl->handle);
  return 0;
}

static void *clib_getsym(lua_State *L, CLibrary *cl, const char *name)
{
  uint32_t actions;
  int had_stopreq = clib_had_stopreq(L);
  void *p = clib_native_dlsym(L, cl->handle, name, &actions);
  clib_checkstop_fresh(L, actions, had_stopreq);
  return p;
}

#elif LJ_TARGET_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS	4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT	2
BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE*);
#endif

#define CLIB_DEFHANDLE	((void *)-1)

/* Default libraries. */
enum {
  CLIB_HANDLE_EXE,
#if !LJ_TARGET_UWP
  CLIB_HANDLE_DLL,
  CLIB_HANDLE_CRT,
  CLIB_HANDLE_KERNEL32,
  CLIB_HANDLE_USER32,
  CLIB_HANDLE_GDI32,
#endif
  CLIB_HANDLE_MAX
};

static void *clib_def_handle[CLIB_HANDLE_MAX];

LJ_NORET LJ_NOINLINE static void clib_error(lua_State *L, const char *fmt,
					    const char *name)
{
  DWORD err = GetLastError();
#if LJ_TARGET_XBOXONE
  wchar_t wbuf[128];
  char buf[128*2];
  if (!FormatMessageW(FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_FROM_SYSTEM,
		      NULL, err, 0, wbuf, sizeof(wbuf)/sizeof(wchar_t), NULL) ||
      !WideCharToMultiByte(CP_ACP, 0, wbuf, 128, buf, 128*2, NULL, NULL))
#else
  char buf[128];
  if (!FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_FROM_SYSTEM,
		      NULL, err, 0, buf, sizeof(buf), NULL))
#endif
    buf[0] = '\0';
  lj_err_callermsg(L, lj_strfmt_pushf(L, fmt, name, buf));
}

static int clib_needext(const char *s)
{
  while (*s) {
    if (*s == '/' || *s == '\\' || *s == '.') return 0;
    s++;
  }
  return 1;
}

static const char *clib_extname(lua_State *L, const char *name)
{
  if (clib_needext(name)) {
    name = lj_strfmt_pushf(L, "%s.dll", name);
    L->top--;
  }
  return name;
}

static void *clib_loadlib(lua_State *L, const char *name, int global)
{
  DWORD oldwerr = GetLastError();
  void *h = LJ_WIN_LOADLIBA(clib_extname(L, name));
  if (!h) clib_error(L, "cannot load module " LUA_QS ": %s", name);
  SetLastError(oldwerr);
  UNUSED(global);
  return h;
}

static uint32_t clib_unloadlib(lua_State *L, CLibrary *cl)
{
  uint32_t actions = 0;
  if (cl->handle == CLIB_DEFHANDLE) {
#if !LJ_TARGET_UWP
    MSize i;
    for (i = CLIB_HANDLE_KERNEL32; i < CLIB_HANDLE_MAX; i++) {
      void *h = clib_def_handle[i];
      if (h) {
	clib_def_handle[i] = NULL;
	if (L) lj_native_enter(L2TG(L));
	FreeLibrary((HINSTANCE)h);
	if (L) actions |= lj_native_leave(L);
      }
    }
#endif
  } else if (cl->handle) {
    if (L) lj_native_enter(L2TG(L));
    FreeLibrary((HINSTANCE)cl->handle);
    if (L) actions |= lj_native_leave(L);
  }
  return actions;
}

#if LJ_TARGET_UWP
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#endif

static void *clib_getsym(lua_State *L, CLibrary *cl, const char *name)
{
  void *p = NULL;
  UNUSED(L);
  if (cl->handle == CLIB_DEFHANDLE) {  /* Search default libraries. */
    MSize i;
    for (i = 0; i < CLIB_HANDLE_MAX; i++) {
      HINSTANCE h = (HINSTANCE)clib_def_handle[i];
      if (!(void *)h) {  /* Resolve default library handles (once). */
#if LJ_TARGET_UWP
	h = (HINSTANCE)&__ImageBase;
#else
	switch (i) {
	case CLIB_HANDLE_EXE: GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, NULL, &h); break;
	case CLIB_HANDLE_DLL:
	  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			     (const char *)clib_def_handle, &h);
	  break;
	case CLIB_HANDLE_CRT:
	  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			     (const char *)&_fmode, &h);
	  break;
	case CLIB_HANDLE_KERNEL32: h = LJ_WIN_LOADLIBA("kernel32.dll"); break;
	case CLIB_HANDLE_USER32: h = LJ_WIN_LOADLIBA("user32.dll"); break;
	case CLIB_HANDLE_GDI32: h = LJ_WIN_LOADLIBA("gdi32.dll"); break;
	}
	if (!h) continue;
#endif
	clib_def_handle[i] = (void *)h;
      }
      p = (void *)GetProcAddress(h, name);
      if (p) break;
    }
  } else {
    p = (void *)GetProcAddress((HINSTANCE)cl->handle, name);
  }
  return p;
}

#else

#define CLIB_DEFHANDLE	NULL

LJ_NORET LJ_NOINLINE static void clib_error(lua_State *L, const char *fmt,
					    const char *name)
{
  lj_err_callermsg(L, lj_strfmt_pushf(L, fmt, name, "no support for this OS"));
}

static void *clib_loadlib(lua_State *L, const char *name, int global)
{
  lj_err_callermsg(L, "no support for loading dynamic libraries for this OS");
  UNUSED(name); UNUSED(global);
  return NULL;
}

static uint32_t clib_unloadlib(lua_State *L, CLibrary *cl)
{
  UNUSED(L); UNUSED(cl);
  return 0;
}

static void *clib_getsym(lua_State *L, CLibrary *cl, const char *name)
{
  UNUSED(L); UNUSED(cl); UNUSED(name);
  return NULL;
}

#endif

/* -- C library indexing -------------------------------------------------- */

static CLibCacheEntry *clib_cache_find(CLibCacheEntry *head, GCstr *name)
{
  CLibCacheEntry *e;
  for (e = head; e != NULL;
       e = lj_clib_cache_next_acq(e)) {
    if (lj_clib_cache_name_acq(e) == name)
      return e;
  }
  return NULL;
}

cTValue *lj_clib_cache_get(CLibrary *cl, GCstr *name)
{
  CLibCacheEntry *head = lj_clib_cache_head_acq(cl);
  CLibCacheEntry *e = clib_cache_find(head, name);
  return e ? (cTValue *)&e->val : NULL;
}

static void clib_cache_publish_wait(lua_State *L)
{
  /*
  ** CLibrary cache publication is a short CAS window. Wait as native time for
  ** the current TG, so safepoint handshakes can observe a cache-fill loser
  ** while another mutator publishes the winning side entry.
  */
  (void)lj_thr_retry_yield(L);
}

CLibCacheEntry *lj_clib_cache_retired_head_acq(global_State *g)
{
  return (CLibCacheEntry *)gc2_clib_cache_retired_acq(g);
}

static int clib_cache_retired_cas(global_State *g, CLibCacheEntry **oldp,
				  CLibCacheEntry *entry)
{
  return gc2_clib_cache_retired_cas(g, (void **)oldp, entry);
}

static CLibCacheEntry *clib_cache_retired_xchg_acqrel(global_State *g,
						      CLibCacheEntry *head)
{
  return (CLibCacheEntry *)gc2_clib_cache_retired_xchg_acqrel(g, head);
}

static void clib_cache_retired_push(global_State *g, CLibCacheEntry *entry)
{
  CLibCacheEntry *head = lj_clib_cache_retired_head_acq(g);
  do {
    lj_clib_cache_retired_next_rel(entry, head);
  } while (!clib_cache_retired_cas(g, &head, entry));
}

static void clib_cache_retire(lua_State *L, global_State *g,
			      CLibCacheEntry *entry)
{
  if (L) {
    GCstr *name = lj_clib_cache_name_acq(entry);
    TValue tv;
    if (name) {
      TValue key;
      setstrV(L, &key, name);
      lj_gc_pubroot(L, &key);
    }
    lj_clib_cache_val_acq(&tv, entry);
    lj_gc_pubroot(L, &tv);
  }
  lj_clib_cache_retire_epoch_rel(entry, lj_gc2_retire_epoch(g));
  clib_cache_retired_push(g, entry);
}

uint32_t lj_clib_cache_reclaim_retired(global_State *g,
				       uint64_t completed_epoch)
{
  CLibCacheEntry *entry;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  entry = clib_cache_retired_xchg_acqrel(g, NULL);
  while (entry) {
    CLibCacheEntry *next = lj_clib_cache_retired_next_acq(entry);
    lj_clib_cache_retired_next_rel(entry, NULL);
    if (lj_clib_cache_retire_epoch_acq(entry) < completed_epoch) {
      lj_mem_freet(g, entry);
      reclaimed++;
    } else {
      clib_cache_retired_push(g, entry);
    }
    entry = next;
  }
  return reclaimed;
}

void lj_clib_cache_freeretired(global_State *g)
{
  CLibCacheEntry *entry;
  if (!g)
    return;
  entry = clib_cache_retired_xchg_acqrel(g, NULL);
  while (entry) {
    CLibCacheEntry *next = lj_clib_cache_retired_next_acq(entry);
    lj_mem_freet(g, entry);
    entry = next;
  }
}

static TValue *clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name,
				  cTValue *val)
{
  TValue key;
  CLibCacheEntry *e;
  setstrV(L, &key, name);
  lj_gc_pubroot(L, &key);  /* 11.7 CLibrary side-cache key. */
  lj_gc_pubroot(L, val);  /* 11.7 CLibrary side-cache value. */
  e = lj_mem_newt(L, sizeof(CLibCacheEntry), CLibCacheEntry);
  lj_clib_cache_next_rel(e, NULL);
  lj_clib_cache_retired_next_rel(e, NULL);
  lj_clib_cache_retire_epoch_rel(e, 0);
  lj_clib_cache_name_rel(e, name);
  lj_clib_cache_val_rel(L, e, val);
  for (;;) {
    CLibCacheEntry *head = lj_clib_cache_head_acq(cl);
    CLibCacheEntry *old = clib_cache_find(head, name);
    CLibCacheEntry *expect;
    if (old) {
      lj_mem_freet(G(L), e);
      return &old->val;
    }
    lj_clib_cache_next_rel(e, head);
    expect = head;
    if (lj_clib_cache_head_cas_rel(cl, &expect, e)) {
      lj_gc_arena_markmem(G(L), e);  /* 11.7 side-entry publish barrier. */
      lj_gc_pubroot(L, &key);  /* Publish-race barrier, see 11.7. */
      lj_gc_pubroot(L, &e->val);
      return &e->val;
    }
    clib_cache_publish_wait(L);
  }
}

static cTValue *clib_env_get(GCtab *env, GCstr *name)
{
  cTValue *tv = env ? lj_tab_getstr(env, name) : NULL;
  return tv && !lj_tv_isnil_acq(tv) ? tv : NULL;
}

static TValue *clib_env_publish(lua_State *L, GCtab *env, GCstr *name,
				cTValue *val)
{
  TValue keytv, old, *dst;
  if (!env)
    return (TValue *)(void *)val;
  setstrV(L, &keytv, name);
  lj_gc_pubroot(L, &keytv);
  lj_gc_pubroot(L, val);
  for (;;) {
    cTValue *cur = clib_env_get(env, name);
    int rc;
    if (cur)
      return (TValue *)(void *)cur;
    dst = lj_tab_setstr(L, env, name);
    rc = lj_tab_trysetnil_cas_keyed(L, env, dst, &keytv, val, &old);
    if (rc == LJ_TAB_STORE_CAS_OK) {
      lj_gc_pubtab(L, env);
      return dst;
    }
    lj_tab_store_wait_l(L);  /* CLibrary env mirror saw stale/FORWARD slot. */
  }
}

static void clib_cache_free(lua_State *L, global_State *g, CLibrary *cl)
{
  CLibCacheEntry *e = lj_clib_cache_head_xchg_acqrel(cl, NULL);
  while (e) {
    CLibCacheEntry *next = lj_clib_cache_next_acq(e);
    clib_cache_retire(L, g, e);
    e = next;
  }
}

#if LJ_TARGET_X86 && LJ_ABI_WIN
/* Compute argument size for fastcall/stdcall functions. */
static CTSize clib_func_argsize(CTState *cts, CType *ct)
{
  CTSize n = 0;
  CTypeID sib = ctype_sib_acq(ct);
  while (sib) {
    CType *d;
    CTInfo info;
    ct = ctype_get(cts, sib);
    info = ctype_info_acq(ct);
    if (ctype_isfield(info)) {
      d = ctype_rawchild(cts, ct);
      n += ((ctype_size_acq(d) + 3) & ~3);
    }
    sib = ctype_sib_acq(ct);
  }
  return n;
}
#endif

/* Index a C library by name. */
TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name)
{
  GCtab *cache_env = lj_clib_cache_env_acq(cl);
  cTValue *envtv = clib_env_get(cache_env, name);
  cTValue *ctv = lj_clib_cache_get(cl, name);
  if (envtv)
    return (TValue *)(void *)envtv;
  if (LJ_LIKELY(ctv && !lj_tv_isnil_acq(ctv)))
    return clib_env_publish(L, cache_env, name, ctv);
  {
    CTState *cts = ctype_cts(L);
    CType snap, *ct = &snap;
    CTypeID id;
    GCstr *symname = name;
    TValue tmp, *tv, *anchor;
    CTInfo info;
    int ok = lj_ctype_getname_snapshot(cts, name, CLNS_INDEX, &id, &snap,
				       &symname);
    if (ok < 0)
      ok = lj_ctype_getname_wait(L, cts, name, CLNS_INDEX, &id, &snap,
				 &symname);
    if (!ok) {
      lj_err_callerv(L, LJ_ERR_FFI_NODECL, strdata(name));
    }
    info = ctype_info_acq(ct);
    if (ctype_isconstval(info)) {
      CTypeID childid = ctype_cid(info);
      CTInfo cttinfo;
      CTSize cttsize;
      CTSize size = ctype_size_acq(ct);
      int cok = lj_ctype_info_snapshot(cts, childid, &cttinfo, &cttsize,
				       NULL, NULL);
      if (cok <= 0)
	cok = lj_ctype_info_wait(L, cts, childid, &cttinfo, &cttsize,
				 NULL, NULL);
      if (cok <= 0)
	lj_err_callerv(L, LJ_ERR_FFI_NODECL, strdata(name));
      lj_assertCTS(ctype_isinteger(cttinfo) && cttsize <= 4,
		   "only 32 bit const supported");  /* NYI */
      UNUSED(cttsize);
      if ((cttinfo & CTF_UNSIGNED) && (int32_t)size < 0) {
	setnumV(&tmp, (lua_Number)(uint32_t)size);
      } else {
	setintV(&tmp, (int32_t)size);
      }
    } else {
      const char *sym = strdata(symname);
#if LJ_TARGET_WINDOWS
      DWORD oldwerr = GetLastError();
#endif
      void *p = clib_getsym(L, cl, sym);
      GCcdata *cd;
      lj_assertCTS(ctype_isfunc(info) || ctype_isextern(info),
		   "unexpected ctype %08x in clib", info);
#if LJ_TARGET_X86 && LJ_ABI_WIN
      /* Retry with decorated name for fastcall/stdcall functions. */
      if (!p && ctype_isfunc(info)) {
	CTInfo cconv = ctype_cconv(info);
	if (cconv == CTCC_FASTCALL || cconv == CTCC_STDCALL) {
	  CTSize sz = clib_func_argsize(cts, ct);
	  const char *symd = lj_strfmt_pushf(L,
			       cconv == CTCC_FASTCALL ? "@%s@%d" : "_%s@%d",
			       sym, sz);
	  L->top--;
	  p = clib_getsym(L, cl, symd);
	}
      }
#endif
      if (!p)
	clib_error(L, "cannot resolve symbol " LUA_QS ": %s", sym);
#if LJ_TARGET_WINDOWS
      SetLastError(oldwerr);
#endif
      cd = lj_cdata_new_l(L, cts, id, CTSIZE_PTR);
      *(void **)cdataptr(cd) = p;
      setcdataV(L, &tmp, cd);
    }
    lj_state_checkstack(L, 1);
    anchor = L->top++;
    copyTV(L, anchor, &tmp);  /* Root tmp while allocating/publishing entry. */
    tv = clib_cache_publish(L, cl, name, anchor);
    tv = clib_env_publish(L, cache_env, name, tv);
    L->top--;
    return tv;
  }
}

/* -- C library management ------------------------------------------------ */

/* Create a new CLibrary object and push it on the stack. */
static CLibrary *clib_new(lua_State *L, GCtab *mt)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  GCudata *ud = lj_udata_new(L, sizeof(CLibrary), t);
  CLibrary *cl = (CLibrary *)uddata(ud);
  lj_clib_cache_env_rel(cl, t);
  cl->cache_head = NULL;
  lj_udata_metatable_rel(ud, mt);
  lj_gc_pubobjobj(L, ud, mt);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, mt);
  lj_udata_udtype_rel(ud, UDTYPE_FFI_CLIB);
  setudataV(L, L->top++, ud);
  return cl;
}

/* Load a C library. */
void lj_clib_load(lua_State *L, GCtab *mt, GCstr *name, int global)
{
  void *handle = clib_loadlib(L, strdata(name), global);
  CLibrary *cl = clib_new(L, mt);
  cl->handle = handle;
}

/* Unload a C library. */
void lj_clib_unload(lua_State *L, global_State *g, CLibrary *cl)
{
  uint32_t actions;
  int had_stopreq = L ? clib_had_stopreq(L) : 0;
  clib_cache_free(L, g, cl);
  actions = clib_unloadlib(L, cl);
  cl->handle = NULL;
  if (L)
    clib_checkstop_fresh(L, actions, had_stopreq);
}

/* Create the default C library object. */
void lj_clib_default(lua_State *L, GCtab *mt)
{
  CLibrary *cl = clib_new(L, mt);
  cl->handle = CLIB_DEFHANDLE;
}

#endif
