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
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_udata.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#include "lj_strfmt.h"

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

/* Quick and dirty solution to resolve shared library name from ld script. */
static const char *clib_resolve_lds(lua_State *L, const char *name)
{
  FILE *fp = fopen(name, "r");
  const char *p = NULL;
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
      if (!strncmp(buf, "/* GNU ld script", 16)) {  /* ld script magic? */
	while (fgets(buf, sizeof(buf), fp)) {  /* Check all lines. */
	  p = clib_check_lds(L, buf);
	  if (p) break;
	}
      } else {  /* Otherwise check only the first line. */
	p = clib_check_lds(L, buf);
      }
    }
    fclose(fp);
  }
  return p;
}

static void *clib_loadlib(lua_State *L, const char *name, int global)
{
  void *h = dlopen(clib_extname(L, name),
		   RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL));
  if (!h) {
    const char *e, *err = dlerror();
    if (err && *err == '/' && (e = strchr(err, ':')) &&
	(name = clib_resolve_lds(L, strdata(lj_str_new(L, err, e-err))))) {
      h = dlopen(name, RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL));
      if (h) return h;
      err = dlerror();
    }
    if (!err) err = "dlopen failed";
    lj_err_callermsg(L, err);
  }
  return h;
}

static void clib_unloadlib(CLibrary *cl)
{
  if (cl->handle && cl->handle != CLIB_DEFHANDLE)
    dlclose(cl->handle);
}

static void *clib_getsym(CLibrary *cl, const char *name)
{
  void *p = dlsym(cl->handle, name);
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

static void clib_unloadlib(CLibrary *cl)
{
  if (cl->handle == CLIB_DEFHANDLE) {
#if !LJ_TARGET_UWP
    MSize i;
    for (i = CLIB_HANDLE_KERNEL32; i < CLIB_HANDLE_MAX; i++) {
      void *h = clib_def_handle[i];
      if (h) {
	clib_def_handle[i] = NULL;
	FreeLibrary((HINSTANCE)h);
      }
    }
#endif
  } else if (cl->handle) {
    FreeLibrary((HINSTANCE)cl->handle);
  }
}

#if LJ_TARGET_UWP
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#endif

static void *clib_getsym(CLibrary *cl, const char *name)
{
  void *p = NULL;
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

static void clib_unloadlib(CLibrary *cl)
{
  UNUSED(cl);
}

static void *clib_getsym(CLibrary *cl, const char *name)
{
  UNUSED(cl); UNUSED(name);
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

static TValue *clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name,
				  cTValue *val)
{
  TValue key;
  CLibCacheEntry *e;
  setstrV(L, &key, name);
  lj_gc_barrierroot(L, &key);  /* 11.7 CLibrary side-cache key. */
  lj_gc_barrierroot(L, val);  /* 11.7 CLibrary side-cache value. */
  e = lj_mem_newt(L, sizeof(CLibCacheEntry), CLibCacheEntry);
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
      lj_gc_barrierroot(L, &key);  /* Publish-race barrier, see 11.7. */
      lj_gc_barrierroot(L, &e->val);
      return &e->val;
    }
    la_cpu_pause();
  }
}

static void clib_cache_free(global_State *g, CLibrary *cl)
{
  CLibCacheEntry *e = lj_clib_cache_head_xchg_acqrel(cl, NULL);
  while (e) {
    CLibCacheEntry *next = lj_clib_cache_next_acq(e);
    lj_mem_freet(g, e);
    e = next;
  }
}

#if LJ_TARGET_X86 && LJ_ABI_WIN
/* Compute argument size for fastcall/stdcall functions. */
static CTSize clib_func_argsize(CTState *cts, CType *ct)
{
  CTSize n = 0;
  while (ct->sib) {
    CType *d;
    ct = ctype_get(cts, ct->sib);
    if (ctype_isfield(ct->info)) {
      d = ctype_rawchild(cts, ct);
      n += ((d->size + 3) & ~3);
    }
  }
  return n;
}
#endif

/* Get redirected or mangled external symbol. */
static const char *clib_extsym(CTState *cts, CType *ct, GCstr *name)
{
  if (ct->sib) {
    CType *ctf = ctype_get(cts, ct->sib);
    GCstr *redir = ctype_name_acq(ctf);
    if (ctype_isxattrib(ctf->info, CTA_REDIR)) {
      lj_assertCTS(redir != NULL, "missing redirected symbol name");
      return strdata(redir);
    }
  }
  return strdata(name);
}

/* Index a C library by name. */
TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name)
{
  cTValue *ctv = lj_clib_cache_get(cl, name);
  if (LJ_LIKELY(ctv && !lj_tv_isnil_acq(ctv)))
    return (TValue *)ctv;
  {
    CTState *cts = ctype_cts(L);
    CType *ct;
    CTypeID id;
    TValue tmp, *tv, *anchor;
    lj_ctype_parse_lock(cts, L);
    /* 11.2: ffi.C namespace readers wait out parser rollback. */
    id = lj_ctype_getname(cts, &ct, name, CLNS_INDEX);
    if (!id) {
      lj_ctype_parse_unlock(cts);
      lj_err_callerv(L, LJ_ERR_FFI_NODECL, strdata(name));
    }
    if (ctype_isconstval(ct->info)) {
      CType *ctt = ctype_child(cts, ct);
      lj_assertCTS(ctype_isinteger(ctt->info) && ctt->size <= 4,
		   "only 32 bit const supported");  /* NYI */
      if ((ctt->info & CTF_UNSIGNED) && (int32_t)ct->size < 0) {
	setnumV(&tmp, (lua_Number)(uint32_t)ct->size);
      } else {
	setintV(&tmp, (int32_t)ct->size);
      }
      lj_ctype_parse_unlock(cts);
    } else {
      lj_ctype_parse_unlock(cts);
      const char *sym = clib_extsym(cts, ct, name);
#if LJ_TARGET_WINDOWS
      DWORD oldwerr = GetLastError();
#endif
      void *p = clib_getsym(cl, sym);
      GCcdata *cd;
      lj_assertCTS(ctype_isfunc(ct->info) || ctype_isextern(ct->info),
		   "unexpected ctype %08x in clib", ct->info);
#if LJ_TARGET_X86 && LJ_ABI_WIN
      /* Retry with decorated name for fastcall/stdcall functions. */
      if (!p && ctype_isfunc(ct->info)) {
	CTInfo cconv = ctype_cconv(ct->info);
	if (cconv == CTCC_FASTCALL || cconv == CTCC_STDCALL) {
	  CTSize sz = clib_func_argsize(cts, ct);
	  const char *symd = lj_strfmt_pushf(L,
			       cconv == CTCC_FASTCALL ? "@%s@%d" : "_%s@%d",
			       sym, sz);
	  L->top--;
	  p = clib_getsym(cl, symd);
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
  cl->cache = t;
  cl->cache_head = NULL;
  setgcrefmt(ud->metatable, obj2gco(mt));
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
void lj_clib_unload(global_State *g, CLibrary *cl)
{
  clib_cache_free(g, cl);
  clib_unloadlib(cl);
  cl->handle = NULL;
}

/* Create the default C library object. */
void lj_clib_default(lua_State *L, GCtab *mt)
{
  CLibrary *cl = clib_new(L, mt);
  cl->handle = CLIB_DEFHANDLE;
}

#endif
