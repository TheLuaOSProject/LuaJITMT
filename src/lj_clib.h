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
  struct CLibCacheEntry *retired_next;
  uint64_t retire_epoch;
  GCstr *name;
  TValue val;
} CLibCacheEntry;

/* Native handles outlive semantic namespace close. Recorded FFI calls may
** embed a dlsym/GetProcAddress result without retaining the originating
** CLibrary, so physical unload is only safe after joined-world trace teardown.
** One node is preallocated per namespace; close therefore never allocates. */
typedef struct CLibHandleRetire {
  struct CLibHandleRetire *next;
  void *handle;
} CLibHandleRetire;

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

static LJ_AINLINE CLibCacheEntry *lj_clib_cache_retired_next_acq(
  const CLibCacheEntry *e)
{
  return (CLibCacheEntry *)la_loadptr_acq((void *const *)&e->retired_next);
}

static LJ_AINLINE void lj_clib_cache_retired_next_rel(CLibCacheEntry *e,
						      CLibCacheEntry *next)
{
  la_storeptr_rel((void **)&e->retired_next, (void *)next);
}

static LJ_AINLINE uint64_t lj_clib_cache_retire_epoch_acq(
  const CLibCacheEntry *e)
{
  return la_load64_acq(&e->retire_epoch);
}

static LJ_AINLINE void lj_clib_cache_retire_epoch_rel(CLibCacheEntry *e,
						      uint64_t epoch)
{
  la_store64_rel(&e->retire_epoch, epoch);
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
  GCRef cache_env;	/* Original cache table, stock debug env behavior. */
  CLibCacheEntry *cache_head;	/* 11.7 side cache, CAS-prepended. */
  CLibHandleRetire *handle_retire;  /* Preallocated terminal-close record. */
  uint32_t lifecycle;  /* Closing bit plus admitted index/cache readers. */
} CLibrary;

#define LJ_CLIB_CLOSING		0x80000000u
#define LJ_CLIB_READER_MASK	0x7fffffffu

static LJ_AINLINE void *lj_clib_handle_acq(const CLibrary *cl)
{
  return la_loadptr_acq((void *const *)&cl->handle);
}

static LJ_AINLINE void lj_clib_handle_rel(CLibrary *cl, void *handle)
{
  la_storeptr_rel((void **)&cl->handle, handle);
}

static LJ_AINLINE void *lj_clib_handle_xchg_acqrel(CLibrary *cl, void *handle)
{
  return la_xchgptr_acqrel((void **)&cl->handle, handle);
}

static LJ_AINLINE uint32_t lj_clib_lifecycle_acq(const CLibrary *cl)
{
  return la_load32_acq(&cl->lifecycle);
}

static LJ_AINLINE CLibHandleRetire *
lj_clib_handle_retire_xchg_acqrel(CLibrary *cl, CLibHandleRetire *retire)
{
  return (CLibHandleRetire *)
    la_xchgptr_acqrel((void **)&cl->handle_retire, retire);
}

static LJ_AINLINE GCtab *lj_clib_cache_env_acq(const CLibrary *cl)
{
  return tabref_acq(cl->cache_env);
}

static LJ_AINLINE void lj_clib_cache_env_rel(CLibrary *cl, GCtab *env)
{
  setgcrefrel(cl->cache_env, obj2gco(env));
}

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

/* Copy a side-cache value under the namespace lifecycle protocol. Returns
** one for a hit, zero for a miss, and -1 once semantic close has begun. */
LJ_FUNC int lj_clib_cache_snapshot(lua_State *L, CLibrary *cl, GCstr *name,
				   TValue *out);
LJ_FUNC CLibCacheEntry *lj_clib_cache_retired_head_acq(global_State *g);
/* Runtime drain only: caller holds GC2's exact-thread exclusive-reclaimer
** scope. Joined-world close uses lj_clib_cache_freeretired(). */
LJ_FUNC uint32_t lj_clib_cache_reclaim_retired(global_State *g,
					       uint64_t completed_epoch);
LJ_FUNC void lj_clib_cache_freeretired(global_State *g);
/* Resolve into an active Lua-stack slot and return its post-relocation
** address. Never exports a raw table-vector or side-cache slot. */
LJ_FUNC TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name,
			      TValue *out);
LJ_FUNC void lj_clib_load(lua_State *L, GCtab *mt, GCstr *name, int global);
LJ_FUNC void lj_clib_unload(lua_State *L, global_State *g, CLibrary *cl);
LJ_FUNC void lj_clib_default(lua_State *L, GCtab *mt);

#if defined(LJ_CLIB_TEST_HELPERS)
LUA_API void lj_clib_test_publish_pause(void);
LUA_API uint32_t lj_clib_test_publish_paused(void);
LUA_API void lj_clib_test_publish_release(void);
LUA_API void lj_clib_test_counters_reset(void);
LUA_API uint32_t lj_clib_test_retired_handles(void);
LUA_API uint32_t lj_clib_test_native_closes(void);
#endif

#endif

#endif
