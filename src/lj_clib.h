/*
** FFI C library loader.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CLIB_H
#define _LJ_CLIB_H

#include "lj_obj.h"

#if LJ_HASFFI

/* Namespace for C library indexing. */
#define CLNS_INDEX	((1u<<CT_FUNC)|(1u<<CT_EXTERN)|(1u<<CT_CONSTVAL))

typedef struct CLibCacheEntry {
  struct CLibCacheEntry *next;
  GCstr *name;
  TValue val;
} CLibCacheEntry;

static LJ_AINLINE CLibCacheEntry *lj_clib_cache_next_acq(
  const CLibCacheEntry *e)
{
  return (CLibCacheEntry *)la_loadptr_acq((void *const *)&e->next);
}

static LJ_AINLINE void lj_clib_cache_next_rel(CLibCacheEntry *e,
					      CLibCacheEntry *next)
{
  la_storeptr_rel((void **)&e->next, (void *)next);
}

static LJ_AINLINE GCstr *lj_clib_cache_name_acq(const CLibCacheEntry *e)
{
  return (GCstr *)la_loadptr_acq((void *const *)&e->name);
}

static LJ_AINLINE void lj_clib_cache_name_rel(CLibCacheEntry *e, GCstr *name)
{
  la_storeptr_rel((void **)&e->name, (void *)name);
}

static LJ_AINLINE void lj_clib_cache_val_acq(TValue *dst,
					     const CLibCacheEntry *e)
{
  lj_tv_load_acq(dst, &e->val);
}

static LJ_AINLINE void lj_clib_cache_val_rel(lua_State *L, CLibCacheEntry *e,
					     cTValue *val)
{
  copyTVrel(L, &e->val, val);
}

/* C library namespace. */
typedef struct CLibrary {
  void *handle;		/* Opaque handle for dynamic library loader. */
  GCtab *cache;		/* Legacy env anchor; miss cache lives in cache_head. */
  CLibCacheEntry *cache_head;	/* 11.7 side cache, CAS-prepended. */
} CLibrary;

static LJ_AINLINE CLibCacheEntry *lj_clib_cache_head_acq(const CLibrary *cl)
{
  return (CLibCacheEntry *)la_loadptr_acq((void *const *)&cl->cache_head);
}

static LJ_AINLINE int lj_clib_cache_head_cas_rel(CLibrary *cl,
						 CLibCacheEntry **expect,
						 CLibCacheEntry *entry)
{
  void *old = (void *)*expect;
  int ok = la_casptr((void **)&cl->cache_head, &old, entry, LA_REL, LA_ACQ);
  if (!ok)
    *expect = (CLibCacheEntry *)old;
  return ok;
}

static LJ_AINLINE CLibCacheEntry *lj_clib_cache_head_xchg_acqrel(
  CLibrary *cl, CLibCacheEntry *head)
{
  return (CLibCacheEntry *)la_xchgptr_acqrel((void **)&cl->cache_head, head);
}

LJ_FUNC cTValue *lj_clib_cache_get(CLibrary *cl, GCstr *name);
LJ_FUNC TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name);
LJ_FUNC void lj_clib_load(lua_State *L, GCtab *mt, GCstr *name, int global);
LJ_FUNC void lj_clib_unload(global_State *g, CLibrary *cl);
LJ_FUNC void lj_clib_default(lua_State *L, GCtab *mt);

#endif

#endif
