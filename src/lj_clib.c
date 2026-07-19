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
#include "lj_vm.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#include "lj_strfmt.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include <stdlib.h>

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
      lj_state_stack_pubtv(L, L, L->top-1);
#if LJ_TARGET_CYGWIN
    } else {
      return name;
#endif
    }
    if (!(name[0] == CLIB_SOPREFIX[0] && name[1] == CLIB_SOPREFIX[1] &&
	  name[2] == CLIB_SOPREFIX[2])) {
      name = lj_strfmt_pushf(L, CLIB_SOPREFIX "%s", name);
      lj_state_stack_pubtv(L, L, L->top-1);
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
    GCstr *s;
    while (*++p == ' ') ;
    for (e = p; *e && *e != ' ' && *e != ')'; e++) ;
    lj_state_checkstack(L, 1);
    s = lj_str_new(L, p, e-p);
    setstrV(L, L->top, s);
    lj_state_stack_pubtv(L, L, L->top);
    L->top++;  /* Root through fclose and the retry dlopen. */
    return strdata(s);
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
  if (lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t close_actions = clib_native_fclose(L, fp);
    lj_safepoint_checkstop(L, actions | close_actions | LJ_GC2_HS_STOPREQ);
  }
}

/* Quick and dirty solution to resolve shared library name from ld script. */
static const char *clib_resolve_lds(lua_State *L, const char *name)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  FILE *fp = clib_native_fopen(L, name, &actions);
  const char *p = NULL;
  if (lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
    if (fp) {
      uint32_t close_actions = clib_native_fclose(L, fp);
      lj_safepoint_checkstop(L, actions | close_actions | LJ_GC2_HS_STOPREQ);
    } else {
      lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
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
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
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
  int had_stopreq = lj_safepoint_had_stopreq(L);
  ptrdiff_t oldtop = savestack(L, L->top);
  const char *extname = clib_extname(L, name);
  void *h = clib_native_dlopen(L, extname,
			       RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL),
			       &actions);
  L->top = restorestack(L, oldtop);
  if (!h) {
    const char *e, *err = dlerror();
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
    if (err && *err == '/' && (e = strchr(err, ':'))) {
      GCstr *path;
      oldtop = savestack(L, L->top);
      lj_state_checkstack(L, 1);
      path = lj_str_new(L, err, e-err);
      setstrV(L, L->top, path);
      lj_state_stack_pubtv(L, L, L->top);
      L->top++;  /* Root through the native fopen. */
      name = clib_resolve_lds(L, strdata(path));
      if (name) {
	had_stopreq = lj_safepoint_had_stopreq(L);
	h = clib_native_dlopen(L, name,
			       RTLD_LAZY | (global?RTLD_GLOBAL:RTLD_LOCAL),
			       &actions);
	L->top = restorestack(L, oldtop);
	if (h) {
	  if (lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
	    uint32_t close_actions = clib_native_dlclose(L, h);
	    lj_safepoint_checkstop(L, actions | close_actions |
				   LJ_GC2_HS_STOPREQ);
	  }
	  return h;
	}
	err = dlerror();
	lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
      } else {
	L->top = restorestack(L, oldtop);
      }
    }
    if (!err) err = "dlopen failed";
    lj_err_callermsg(L, err);
  } else if (lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t close_actions = clib_native_dlclose(L, h);
    lj_safepoint_checkstop(L, actions | close_actions | LJ_GC2_HS_STOPREQ);
  }
  return h;
}

static int clib_handle_closeable(void *handle)
{
  return handle && handle != CLIB_DEFHANDLE;
}

static uint32_t clib_unloadhandle(lua_State *L, void *handle)
{
  return clib_handle_closeable(handle) ?
    clib_native_dlclose(L, handle) : 0;
}

static void *clib_getsym(lua_State *L, CLibrary *cl, const char *name)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  void *p = clib_native_dlsym(L, lj_clib_handle_acq(cl), name, &actions);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
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
    lj_state_stack_pubtv(L, L, L->top-1);
  }
  return name;
}

static void *clib_native_loadlib(lua_State *L, const char *name,
				 DWORD *errp, uint32_t *actionsp)
{
  void *h;
  lj_native_enter(L2TG(L));
  h = LJ_WIN_LOADLIBA(name);
  *errp = h ? 0 : GetLastError();
  *actionsp = lj_native_leave(L);
  return h;
}

static uint32_t clib_native_freelib(lua_State *L, void *handle)
{
  uint32_t actions = 0;
  if (L) lj_native_enter(L2TG(L));
  FreeLibrary((HINSTANCE)handle);
  if (L) actions = lj_native_leave(L);
  return actions;
}

static void *clib_loadlib(lua_State *L, const char *name, int global)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  ptrdiff_t oldtop = savestack(L, L->top);
  DWORD oldwerr = GetLastError(), err = 0;
  void *h;
  name = clib_extname(L, name);
  h = clib_native_loadlib(L, name, &err, &actions);
  if (!h) {
    lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
    SetLastError(err);
    clib_error(L, "cannot load module " LUA_QS ": %s", name);
  }
  L->top = restorestack(L, oldtop);
  if (lj_safepoint_fresh_stopreq(L, actions, had_stopreq)) {
    uint32_t close_actions = clib_native_freelib(L, h);
    lj_safepoint_checkstop(L, actions | close_actions | LJ_GC2_HS_STOPREQ);
  }
  SetLastError(oldwerr);
  UNUSED(global);
  return h;
}

static int clib_handle_closeable(void *handle)
{
  /* Default handles are process-global and shared by all Lua universes. OS
  ** process teardown owns them until a separate exact global refcount exists. */
  return handle != NULL && handle != CLIB_DEFHANDLE;
}

static uint32_t clib_unloadhandle(lua_State *L, void *handle)
{
  uint32_t actions = 0;
  if (clib_handle_closeable(handle)) {
    actions |= clib_native_freelib(L, handle);
  }
  return actions;
}

#if LJ_TARGET_UWP
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#endif

static void *clib_getsym_raw(CLibrary *cl, const char *name)
{
  void *handle = lj_clib_handle_acq(cl);
  void *p = NULL;
  if (handle == CLIB_DEFHANDLE) {  /* Search default libraries. */
    MSize i;
    for (i = 0; i < CLIB_HANDLE_MAX; i++) {
      HINSTANCE h = (HINSTANCE)la_loadptr_acq(&clib_def_handle[i]);
      if (!(void *)h) {  /* Resolve default library handles (once). */
        int owns_ref = 0;
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
	case CLIB_HANDLE_KERNEL32:
	  h = LJ_WIN_LOADLIBA("kernel32.dll"); owns_ref = 1; break;
	case CLIB_HANDLE_USER32:
	  h = LJ_WIN_LOADLIBA("user32.dll"); owns_ref = 1; break;
	case CLIB_HANDLE_GDI32:
	  h = LJ_WIN_LOADLIBA("gdi32.dll"); owns_ref = 1; break;
	}
	if (!h) continue;
#endif
	{
	  void *expect = NULL;
	  if (!la_casptr(&clib_def_handle[i], &expect, (void *)h,
			 LA_REL, LA_ACQ)) {
	    if (owns_ref)
	      (void)FreeLibrary(h);  /* Drop the losing LoadLibrary reference. */
	    h = (HINSTANCE)expect;
	  }
	}
      }
      p = (void *)GetProcAddress(h, name);
      if (p) break;
    }
  } else {
    p = (void *)GetProcAddress((HINSTANCE)handle, name);
  }
  return p;
}

static void *clib_getsym(lua_State *L, CLibrary *cl, const char *name)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  DWORD err = 0;
  void *p;
  lj_native_enter(L2TG(L));
  p = clib_getsym_raw(cl, name);
  if (!p) err = GetLastError();
  actions = lj_native_leave(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
  if (!p) SetLastError(err);
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

static int clib_handle_closeable(void *handle)
{
  UNUSED(handle);
  return 0;
}

static uint32_t clib_unloadhandle(lua_State *L, void *handle)
{
  UNUSED(L); UNUSED(handle);
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

static int clib_reader_enter(CLibrary *cl)
{
  uint32_t state = lj_clib_lifecycle_acq(cl);
  for (;;) {
    uint32_t readers = state & LJ_CLIB_READER_MASK;
    if (state & LJ_CLIB_CLOSING)
      return 0;
    if (LJ_UNLIKELY(readers == LJ_CLIB_READER_MASK))
      abort();
    if (la_cas32(&cl->lifecycle, &state, state + 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static int clib_cache_snapshot_held(lua_State *L, CLibrary *cl, GCstr *name,
				    TValue *out)
{
  CLibCacheEntry *head = lj_clib_cache_head_acq(cl);
  CLibCacheEntry *e = clib_cache_find(head, name);
  TValue tmp;
  if (!e) {
    setnilV(out);
    return 0;
  }
  lj_clib_cache_val_acq(&tmp, e);
  copyTVrel(L, out, &tmp);
  return !tvisnil(out);
}

static void clib_reader_leave(lua_State *L, global_State *g,
			      CLibrary *cl, int *closingp);

#if defined(LJ_CLIB_TEST_HELPERS)
static uint32_t clib_test_publish_armed;
static uint32_t clib_test_publish_is_paused;
static uint32_t clib_test_publish_do_release;
static uint32_t clib_test_retired_handle_count;
static uint32_t clib_test_native_close_count;

LUA_API void lj_clib_test_publish_pause(void)
{
  la_store32_rel(&clib_test_publish_do_release, 0);
  la_store32_rel(&clib_test_publish_is_paused, 0);
  la_store32_rel(&clib_test_publish_armed, 1);
}

LUA_API uint32_t lj_clib_test_publish_paused(void)
{
  return la_load32_acq(&clib_test_publish_is_paused);
}

LUA_API void lj_clib_test_publish_release(void)
{
  la_store32_rel(&clib_test_publish_do_release, 1);
}

LUA_API void lj_clib_test_counters_reset(void)
{
  la_store32_rel(&clib_test_publish_armed, 0);
  la_store32_rel(&clib_test_publish_is_paused, 0);
  la_store32_rel(&clib_test_publish_do_release, 0);
  la_store32_rel(&clib_test_retired_handle_count, 0);
  la_store32_rel(&clib_test_native_close_count, 0);
}

LUA_API uint32_t lj_clib_test_retired_handles(void)
{
  return la_load32_acq(&clib_test_retired_handle_count);
}

LUA_API uint32_t lj_clib_test_native_closes(void)
{
  return la_load32_acq(&clib_test_native_close_count);
}

static void clib_test_cache_publish_pause(lua_State *L)
{
  uint32_t armed = 1;
  if (!la_cas32(&clib_test_publish_armed, &armed, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&clib_test_publish_is_paused, 1);
  while (!la_load32_acq(&clib_test_publish_do_release))
    (void)lj_thr_retry_yield(L);
}
#else
#define clib_test_cache_publish_pause(L)	((void)0)
#endif

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

static CLibHandleRetire *clib_handle_retired_head_acq(global_State *g)
{
  return (CLibHandleRetire *)gc2_clib_handle_retired_acq(g);
}

static void clib_handle_retired_push(global_State *g,
				     CLibHandleRetire *retire)
{
  CLibHandleRetire *head = clib_handle_retired_head_acq(g);
  do {
    la_storeptr_rel((void **)&retire->next, head);
  } while (!gc2_clib_handle_retired_cas(g, (void **)&head, retire));
#if defined(LJ_CLIB_TEST_HELPERS)
  (void)la_add32_acqrel(&clib_test_retired_handle_count, 1);
#endif
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

/* Preflight before mutating a detached cache chain. In a singly linked list a
** duplicate entry necessarily closes a cycle; Floyd detection prevents both
** suffix truncation and a second free of the same cache entry. */
static int clib_cache_retired_chain_valid(global_State *g,
					   CLibCacheEntry *head,
					   int reclaim_held)
{
  CLibCacheEntry *slow = head, *fast = head;
  while (fast) {
    if (slow) {
      if (!(reclaim_held ?
	    lj_gc2_mem_registered_known_reclaim_held(g, slow) :
	    lj_gc2_mem_registered_known(g, slow)))
	return 0;
      slow = lj_clib_cache_retired_next_acq(slow);
    }
    if (!(reclaim_held ?
	  lj_gc2_mem_registered_known_reclaim_held(g, fast) :
	  lj_gc2_mem_registered_known(g, fast)))
      return 0;
    fast = lj_clib_cache_retired_next_acq(fast);
    if (fast) {
      if (!(reclaim_held ?
	    lj_gc2_mem_registered_known_reclaim_held(g, fast) :
	    lj_gc2_mem_registered_known(g, fast)))
	return 0;
      fast = lj_clib_cache_retired_next_acq(fast);
    }
    if (fast && slow == fast)
      return 0;
  }
  return 1;
}

uint32_t lj_clib_cache_reclaim_retired(global_State *g,
				       uint64_t completed_epoch)
{
  CLibCacheEntry *entry;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  /* The generic GC2 owner retains its exact current-thread SMR capability
  ** across this detach and every entry validation below. */
  entry = clib_cache_retired_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!clib_cache_retired_chain_valid(g, entry, 1))) {
    lj_assertG(0, "invalid/cyclic detached CLibrary cache retire chain");
    abort();
  }
  while (entry) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, entry))) {
      lj_assertG(0, "invalid detached CLibrary cache retire entry");
      abort();
    }
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
  CLibHandleRetire *retire;
  if (!g)
    return;
  entry = clib_cache_retired_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!clib_cache_retired_chain_valid(g, entry, 0))) {
    lj_assertG(0, "invalid/cyclic terminal CLibrary cache retire chain");
    abort();
  }
  while (entry) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, entry))) {
      lj_assertG(0, "invalid terminal CLibrary cache retire entry");
      abort();
    }
    CLibCacheEntry *next = lj_clib_cache_retired_next_acq(entry);
    lj_mem_freet(g, entry);
    entry = next;
  }
  /* close_state calls this after lj_trace_freestate(). No generated CALLXS or
  ** recorder constant can still execute a pointer from these handles. */
  retire = (CLibHandleRetire *)
    gc2_clib_handle_retired_xchg_acqrel(g, NULL);
  while (retire) {
    CLibHandleRetire *next = (CLibHandleRetire *)
      la_loadptr_acq((void *const *)&retire->next);
    (void)clib_unloadhandle(NULL, retire->handle);
#if defined(LJ_CLIB_TEST_HELPERS)
    (void)la_add32_acqrel(&clib_test_native_close_count, 1);
#endif
    free(retire);
    retire = next;
  }
}

static void clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name,
			       cTValue *val, TValue *out)
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
      TValue tmp;
      lj_clib_cache_val_acq(&tmp, old);
      copyTVrel(L, out, &tmp);
      lj_state_stack_pubtv(L, L, out);
      lj_mem_freet(G(L), e);
      return;
    }
    lj_clib_cache_next_rel(e, head);
    clib_test_cache_publish_pause(L);
    expect = head;
    if (lj_clib_cache_head_cas_rel(cl, &expect, e)) {
      lj_gc_arena_markmem(G(L), e);  /* 11.7 side-entry publish barrier. */
      lj_gc_pubroot(L, &key);  /* Publish-race barrier, see 11.7. */
      lj_gc_pubroot(L, &e->val);
      if (out != val) {
	copyTVrel(L, out, val);
	lj_state_stack_pubtv(L, L, out);
      }
      return;
    }
    /* A losing prepend has made no semantic claim. Re-read the new head and
    ** retry directly; no publisher-dependent wait edge is required. */
  }
}

static int clib_env_get(lua_State *L, GCtab *env, GCstr *name, TValue *out)
{
  TValue key;
  if (!env) {
    setnilV(out);
    return 0;
  }
  setstrV(L, &key, name);
  (void)lj_tab_gettv_forjit(L, env, &key, out);
  lj_state_stack_pubtv(L, L, out);
  return !tvisnil(out);
}

static void clib_env_publish(lua_State *L, GCtab *env, GCstr *name,
			     cTValue *val, TValue *out)
{
  TValue keytv, old, *dst;
  if (!env) {
    lj_tv_load_acq(out, val);
    lj_state_stack_pubtv(L, L, out);
    return;
  }
  setstrV(L, &keytv, name);
  lj_gc_pubroot(L, &keytv);
  lj_gc_pubroot(L, val);
  for (;;) {
    int rc;
    if (clib_env_get(L, env, name, out))
      return;
    dst = lj_tab_setstr(L, env, name);
    rc = lj_tab_trysetnil_cas_keyed(L, env, dst, &keytv, val, &old);
    if (rc == LJ_TAB_STORE_CAS_OK) {
      lj_gc_pubtab(L, env);
      lj_tv_load_acq(out, val);
      lj_gc_pubroot(L, out);
      lj_state_stack_pubtv(L, L, out);
      return;
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

/* The unique zero-reader close owner detaches semantic cache state and moves
** the native handle to a preallocated terminal list. Physical unload here
** would invalidate pointers already embedded in traces or escaped cdata. */
static void clib_close_cleanup(lua_State *L, global_State *g, CLibrary *cl)
{
  CLibHandleRetire *retire;
  void *handle;
  clib_cache_free(L, g, cl);
  handle = lj_clib_handle_xchg_acqrel(cl, NULL);
  retire = lj_clib_handle_retire_xchg_acqrel(cl, NULL);
  if (clib_handle_closeable(handle)) {
    if (LJ_UNLIKELY(!retire)) {
      lj_assertG(0, "CLibrary close lost preallocated handle record");
      abort();
    }
    retire->handle = handle;
    clib_handle_retired_push(g, retire);
  } else {
    free(retire);
  }
}

static void clib_reader_leave(lua_State *L, global_State *g, CLibrary *cl,
			      int *closingp)
{
  uint32_t old = la_sub32_acqrel(&cl->lifecycle, 1);
  if (LJ_UNLIKELY((old & LJ_CLIB_READER_MASK) == 0)) {
    lj_assertG(0, "CLibrary reader count underflow");
    abort();
  }
  if (old == (LJ_CLIB_CLOSING | 1u))
    clib_close_cleanup(L, g, cl);
  if (closingp)
    *closingp = (old & LJ_CLIB_CLOSING) != 0 ||
      (lj_clib_lifecycle_acq(cl) & LJ_CLIB_CLOSING) != 0;
}

int lj_clib_cache_snapshot(lua_State *L, CLibrary *cl, GCstr *name,
			   TValue *out)
{
  global_State *g = G(L);
  int found, closing = 0;
  if (!clib_reader_enter(cl)) {
    setnilV(out);
    return -1;
  }
  found = clib_cache_snapshot_held(L, cl, name, out);
  if (found)
    lj_gc_pubroot(L, out);
  clib_reader_leave(L, g, cl, &closing);
  if (closing) {
    setnilV(out);
    return -1;
  }
  return found;
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

typedef struct CLibIndexCtx {
  CLibrary *cl;
  GCtab *cache_env;
  ptrdiff_t keyofs;
  ptrdiff_t outofs;
} CLibIndexCtx;

static TValue *clib_index_miss_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  CLibIndexCtx *ctx = (CLibIndexCtx *)ud;
  ptrdiff_t sourceofs, resultofs;
  TValue *key, *source, *result, *out;
  GCstr *name;
  UNUSED(dummy);
  lj_state_checkstack(L, 2);
  key = restorestack(L, ctx->keyofs);
  name = strV(key);
  source = L->top;
  sourceofs = savestack(L, source);
  setnilV(source);
  lj_state_stack_pubtv(L, L, source);
  L->top++;
  result = L->top;
  resultofs = savestack(L, result);
  setnilV(result);
  lj_state_stack_pubtv(L, L, result);
  L->top++;

  if (clib_cache_snapshot_held(L, ctx->cl, name, source)) {
    lj_state_stack_pubtv(L, L, source);
    clib_env_publish(L, ctx->cache_env, name, source, result);
  } else {
    CTState *cts = ctype_cts(L);
    CType snap, *ct = &snap;
    CTypeID id;
    GCstr *symname = name;
    TValue tmp;
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
    /* Reserve the eventual cache-publication anchor before constructing a
    ** symbol cdata. No stack growth or other poll then separates READY return
    ** from copyTV() into the semantic Lua root below. */
    lj_state_checkstack(L, 1);
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
      void *p = clib_getsym(L, ctx->cl, sym);
      GCcdata *cd;
      lj_assertCTS(ctype_isfunc(info) || ctype_isextern(info),
		   "unexpected ctype %08x in clib", info);
#if LJ_TARGET_X86 && LJ_ABI_WIN
      /* Retry with decorated name for fastcall/stdcall functions. */
      if (!p && ctype_isfunc(info)) {
	CTInfo cconv = ctype_cconv(info);
	if (cconv == CTCC_FASTCALL || cconv == CTCC_STDCALL) {
	  CTSize sz = clib_func_argsize(cts, ct);
	  ptrdiff_t oldtop = savestack(L, L->top);
	  const char *symd = lj_strfmt_pushf(L,
			       cconv == CTCC_FASTCALL ? "@%s@%d" : "_%s@%d",
			       sym, sz);
	  lj_state_stack_pubtv(L, L, L->top-1);
	  p = clib_getsym(L, ctx->cl, symd);
	  L->top = restorestack(L, oldtop);
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
    source = restorestack(L, sourceofs);
    copyTV(L, source, &tmp);  /* Root tmp before cache entry allocation. */
    lj_state_stack_pubtv(L, L, source);
    clib_cache_publish(L, ctx->cl, name, source, source);
    source = restorestack(L, sourceofs);
    result = restorestack(L, resultofs);
    clib_env_publish(L, ctx->cache_env, name, source, result);
  }

  result = restorestack(L, resultofs);
  out = restorestack(L, ctx->outofs);
  copyTVrel(L, out, result);
  lj_state_stack_pubtv(L, L, out);
  L->top = restorestack(L, sourceofs);
  return NULL;
}

static LJ_NORET void clib_closed_error(lua_State *L)
{
  lj_err_callermsg(L, "attempt to use a closed C library");
}

/* Index a C library by name. Fast environment hits only need a short reader;
** miss/fill work is protected so every throwing edge releases its reader. */
TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name, TValue *out)
{
  CLibIndexCtx ctx;
  GCtab *cache_env = lj_clib_cache_env_acq(cl);
  ptrdiff_t outofs = savestack(L, out);
  ptrdiff_t keyofs;
  TValue *key;
  int closing = 0, errcode;

  lj_state_checkstack(L, 1);
  key = L->top;
  keyofs = savestack(L, key);
  setstrV(L, key, name);
  lj_state_stack_pubtv(L, L, key);
  L->top++;
  out = restorestack(L, outofs);
  if (clib_env_get(L, cache_env, name, out)) {
    if (!clib_reader_enter(cl)) {
      L->top = restorestack(L, keyofs);
      clib_closed_error(L);
    }
    clib_reader_leave(L, G(L), cl, &closing);
    L->top = restorestack(L, keyofs);
    if (closing)
      clib_closed_error(L);
    return restorestack(L, outofs);
  }

  if (!clib_reader_enter(cl)) {
    L->top = restorestack(L, keyofs);
    clib_closed_error(L);
  }
  ctx.cl = cl;
  ctx.cache_env = cache_env;
  ctx.keyofs = keyofs;
  ctx.outofs = outofs;
  errcode = lj_vm_cpcall(L, NULL, &ctx, clib_index_miss_cp);
  clib_reader_leave(L, G(L), cl, &closing);
  L->top = restorestack(L, keyofs);
  if (LJ_UNLIKELY(errcode))
    lj_err_throw(L, errcode);
  if (closing)
    clib_closed_error(L);
  return restorestack(L, outofs);
}

/* -- C library management ------------------------------------------------ */

/* Create a new CLibrary object and push it on the stack. */
static CLibrary *clib_new(lua_State *L, GCtab *mt)
{
  LJUdataRoot root;
  CLibHandleRetire *retire;
  GCtab *t;
  GCudata *ud;
  CLibrary *cl;
  lj_state_checkstack(L, 1);
  t = lj_tab_new(L, 0, 0);
  /* The cache table is private and is not a registry root.  Publish it in the
  ** eventual result slot before any userdata/FINREG allocation can yield. */
  settabV(L, L->top, t);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  ud = lj_udata_newrooted(L, sizeof(CLibrary), t, &root);
  cl = (CLibrary *)uddata(ud);
  lj_clib_handle_rel(cl, NULL);
  lj_clib_cache_env_rel(cl, t);
  la_storeptr_rel((void **)&cl->cache_head, NULL);
  la_storeptr_rel((void **)&cl->handle_retire, NULL);
  la_store32_rel(&cl->lifecycle, 0);
  lj_gc_pubobjobj(L, ud, t);
  lj_udata_metatable_rel(ud, mt);
  lj_gc_pubobjobj(L, ud, mt);
  /* Keep the object generic and constructor-rooted across the throwing raw
  ** FINREG-node allocation.  handle/cache fields are now destructor-safe. */
  lj_udata_finreg_mt_rooted(L, ud, mt, &root);
  /* Semantic close can race an admitted index reader and is not allowed to
  ** allocate. Preallocate its raw terminal-handle record before publishing
  ** the specialized userdata tag. */
  retire = (CLibHandleRetire *)malloc(sizeof(*retire));
  if (LJ_UNLIKELY(!retire)) {
    lj_udata_root_release(&root);
    lj_err_mem(L);
  }
  retire->next = NULL;
  retire->handle = NULL;
  la_storeptr_rel((void **)&cl->handle_retire, retire);
  lj_udata_specialize(L, ud, UDTYPE_FFI_CLIB);
  /* Replace the temporary cache-table root with the public CLibrary result.
  ** The constructor anchor overlaps this root transition. */
  setudataV(L, L->top-1, ud);
  lj_state_stack_pubtv(L, L, L->top-1);
  lj_udata_root_release(&root);
  return cl;
}

/* Load a C library. */
void lj_clib_load(lua_State *L, GCtab *mt, GCstr *name, int global)
{
  CLibrary *cl = clib_new(L, mt);
  /* Construct/root/register first.  Any loader error now unwinds an object
  ** whose NULL handle is safe to finalize, and successful native handles are
  ** never stranded by a later userdata allocation failure. */
  lj_clib_handle_rel(cl, clib_loadlib(L, strdata(name), global));
}

/* Unload a C library. */
void lj_clib_unload(lua_State *L, global_State *g, CLibrary *cl)
{
  uint32_t state = lj_clib_lifecycle_acq(cl);
  for (;;) {
    uint32_t closed = state | LJ_CLIB_CLOSING;
    if (state & LJ_CLIB_CLOSING)
      return;
    if (la_cas32(&cl->lifecycle, &state, closed,
		 LA_ACQ_REL, LA_ACQ)) {
      /* Unload never waits for admitted users. The last reader owns cleanup;
      ** with no readers this CAS winner is the cleanup owner itself. */
      if ((state & LJ_CLIB_READER_MASK) == 0)
	clib_close_cleanup(L, g, cl);
      break;
    }
  }
}

/* Create the default C library object. */
void lj_clib_default(lua_State *L, GCtab *mt)
{
  CLibrary *cl = clib_new(L, mt);
  lj_clib_handle_rel(cl, CLIB_DEFHANDLE);
}

#endif
