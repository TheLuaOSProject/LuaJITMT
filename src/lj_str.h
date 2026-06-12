/*
** String handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_STR_H
#define _LJ_STR_H

#include <stdarg.h>

#include "lj_obj.h"

#define LJ_STRHASH_DEAD		((uintptr_t)1)
#define LJ_STRHASH_SECONDARY	((uintptr_t)2)
#define LJ_STRHASH_LINKMASK	(LJ_STRHASH_DEAD|LJ_STRHASH_SECONDARY)

#define lj_str_hashhead(r) \
  ((GCobj *)(void *)(gcrefu((r)) & ~(uintptr_t)LJ_STRHASH_LINKMASK))
#define lj_str_hashflags(r)	(gcrefu((r)) & LJ_STRHASH_LINKMASK)
#define lj_str_hashsecondary(r)	(gcrefu((r)) & LJ_STRHASH_SECONDARY)
#define lj_str_buckets(g)	((g)->str.tabh->bucket)
#define lj_str_tabsize(mask) \
  ((mask) == ~(MSize)0 ? (GCSize)0 : \
   (GCSize)offsetof(StrTabHdr, bucket) + \
   (((GCSize)(mask) + 1u) * (GCSize)sizeof(GCRef)))
#define lj_str_tabbytes(tabh) \
  ((tabh) ? lj_str_tabsize((tabh)->mask) : (GCSize)0)

/* String helpers. */
LJ_FUNC int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b);
LJ_FUNC const char *lj_str_find(const char *s, const char *f,
				MSize slen, MSize flen);
LJ_FUNC int lj_str_haspattern(GCstr *s);

/* String interning. */
LJ_FUNC void lj_str_resize(lua_State *L, MSize newmask);
LJ_FUNCA GCstr *lj_str_new(lua_State *L, const char *str, size_t len);
LJ_FUNC void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s);
LJ_FUNC void LJ_FASTCALL lj_str_init(lua_State *L);
LJ_FUNC uint32_t lj_str_reclaim_retired(global_State *g,
					uint64_t completed_epoch);
LJ_FUNC void lj_str_freetab(global_State *g);

#define lj_str_newz(L, s)	(lj_str_new(L, s, strlen(s)))
#define lj_str_newlit(L, s)	(lj_str_new(L, "" s, sizeof(s)-1))
#define lj_str_size(len)	(sizeof(GCstr) + (((len)+4) & ~(MSize)3))

#endif
