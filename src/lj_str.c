/*
** String handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"
#include "lj_prng.h"
#include "lj_thr.h"

/* -- String helpers ------------------------------------------------------ */

/* Ordered compare of strings. Assumes string data is 4-byte aligned. */
int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b)
{
  MSize i, n = a->len > b->len ? b->len : a->len;
  for (i = 0; i < n; i += 4) {
    /* Note: innocuous access up to end of string + 3. */
    uint32_t va = *(const uint32_t *)(strdata(a)+i);
    uint32_t vb = *(const uint32_t *)(strdata(b)+i);
    if (va != vb) {
#if LJ_LE
      va = lj_bswap(va); vb = lj_bswap(vb);
#endif
      i -= n;
      if ((int32_t)i >= -3) {
	va >>= 32+(i<<3); vb >>= 32+(i<<3);
	if (va == vb) break;
      }
      return va < vb ? -1 : 1;
    }
  }
  return (int32_t)(a->len - b->len);
}

/* Find fixed string p inside string s. */
const char *lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      plen--; slen -= plen;
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
      }
    }
  }
  return NULL;
}

/* Check whether a string has a pattern matching character. */
int lj_str_haspattern(GCstr *s)
{
  const char *p = strdata(s), *q = p + s->len;
  while (p < q) {
    int c = *(const uint8_t *)p++;
    if (lj_char_ispunct(c) && strchr("^$*+?.([%-", c))
      return 1;  /* Found a pattern matching char. */
  }
  return 0;  /* No pattern matching chars found. */
}

/* -- String hashing ------------------------------------------------------ */

/* Keyed sparse ARX string hash. Constant time. */
static StrHash hash_sparse(uint64_t seed, const char *str, MSize len)
{
  /* Constants taken from lookup3 hash by Bob Jenkins. */
  StrHash a, b, h = len ^ (StrHash)seed;
  if (len >= 4) {  /* Caveat: unaligned access! */
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  }
  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);
  return h;
}

#if LUAJIT_SECURITY_STRHASH
/* Keyed dense ARX string hash. Linear time. */
static LJ_NOINLINE StrHash hash_dense(uint64_t seed, StrHash h,
				      const char *str, MSize len)
{
  StrHash b = lj_bswap(lj_rol(h ^ (StrHash)(seed >> 32), 4));
  if (len > 12) {
    StrHash a = (StrHash)seed;
    const char *pe = str+len-12, *p = pe, *q = str;
    do {
      a += lj_getu32(p);
      b += lj_getu32(p+4);
      h += lj_getu32(p+8);
      p = q; q += 12;
      h ^= b; h -= lj_rol(b, 14);
      a ^= h; a -= lj_rol(h, 11);
      b ^= a; b -= lj_rol(a, 25);
    } while (p < pe);
    h ^= b; h -= lj_rol(b, 16);
    a ^= h; a -= lj_rol(h, 4);
    b ^= a; b -= lj_rol(a, 14);
  }
  return b;
}
#endif

/* -- String interning ---------------------------------------------------- */

#define LJ_STR_MAXCOLL		32
#define LJ_STRTAB_RESIZE	((MSize)0x80000000u)
#define LJ_STRTAB_ACTIVE_MASK	(~LJ_STRTAB_RESIZE)

static void strtab_wait(lua_State *L)
{
  /*
  ** String-table claim/enter waits are reached from string interning, resize,
  ** and secondary rehash paths with a current Lua state. Wait as native time
  ** for that TG so safepoint handshakes can observe threads contending on the
  ** pin-and-drain resize bit.
  */
  (void)lj_thr_sleep_ns(L, 1000000);
}

static LJ_AINLINE int strref_cas_rel(GCRef *r, uintptr_t *expect, uintptr_t want)
{
#if LJ_GC64
  uint64_t exp = (uint64_t)*expect;
  int ok = la_cas64(&r->gcptr64, &exp, (uint64_t)want, LA_REL, LA_ACQ);
  *expect = (uintptr_t)exp;
  return ok;
#else
  uint32_t exp = (uint32_t)*expect;
  int ok = la_cas32(&r->gcptr32, &exp, (uint32_t)want, LA_REL, LA_ACQ);
  *expect = (uintptr_t)exp;
  return ok;
#endif
}

static void strtab_retired_push(global_State *g, StrTabHdr *hdr)
{
  StrTabHdr *head = lj_str_retired_head_acq(g);
  /* 06 section 6.5: RCU retire. */
  do {
    lj_str_retired_next_rel(hdr, head);
  } while (!lj_str_retired_head_cas(g, &head, hdr));
}

static void strtab_retire(global_State *g, StrTabHdr *hdr)
{
  lj_str_retire_epoch_rel(hdr, lj_gc2_retire_epoch(g));
  strtab_retired_push(g, hdr);
}

static int strtab_claim(lua_State *L, StrTabHdr *hdr)
{
  for (;;) {
    MSize state = la_load32_acq(&hdr->resize);
    MSize expect = state;
    if (state & LJ_STRTAB_RESIZE)
      return 0;
    if (la_cas32(&hdr->resize, &expect, state | LJ_STRTAB_RESIZE,
		 LA_ACQ_REL, LA_ACQ))
      break;
  }
  while (la_load32_acq(&hdr->resize) & LJ_STRTAB_ACTIVE_MASK)
    strtab_wait(L);
  return 1;
}

static void strtab_release(StrTabHdr *hdr)
{
  la_store32_rel(&hdr->resize, 0);
}

static LJ_AINLINE void strtab_leave(StrTabHdr *hdr)
{
  MSize old = la_sub32_acqrel(&hdr->resize, 1);
  lj_assertX((old & LJ_STRTAB_ACTIVE_MASK) != 0,
	     "bad string table active count");
  UNUSED(old);
}

static StrTabHdr *strtab_enter(lua_State *L, global_State *g)
{
  for (;;) {
    StrTabHdr *hdr = lj_str_tabh_acq(g);
    MSize state, expect;
    if (hdr == NULL)
      return NULL;
    state = la_load32_acq(&hdr->resize);  /* 06 section 6.5 RCU header pin. */
    if (state & LJ_STRTAB_RESIZE) {
      strtab_wait(L);
      continue;
    }
    if (state == LJ_STRTAB_ACTIVE_MASK) {
      strtab_wait(L);
      continue;
    }
    expect = state;
    if (la_cas32(&hdr->resize, &expect, state + 1u, LA_ACQ_REL, LA_ACQ)) {
      if (LJ_LIKELY(lj_str_tabh_acq(g) == hdr))
	return hdr;
      strtab_leave(hdr);
    }
  }
}

/* Resize the string interning hash table (grow and shrink). */
void lj_str_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  StrTabHdr *newhdr;
  StrTabHdr *oldhdr = lj_str_tabh_acq(g);
  GCRef *newtab, *oldtab = oldhdr ? oldhdr->bucket : NULL;
  GCSize newsize;
  MSize i, oldmask = oldhdr ? oldhdr->mask : ~(MSize)0;

  /* No resizing during GC traversal or if already too big. */
  if (g->gc.state == GCSsweepstring || newmask >= LJ_MAX_STRTAB-1)
    return;

  if (oldhdr) {
    if (!strtab_claim(L, oldhdr))
      return;
  }

  newsize = lj_str_tabsize(newmask);
  newhdr = (StrTabHdr *)lj_mem_new(L, newsize);
  memset(newhdr, 0, newsize);
  newhdr->mask = newmask;
  newtab = newhdr->bucket;

#if LUAJIT_SECURITY_STRHASH
  /* Check which chains need secondary hashes. */
  if (g->str.second) {
    int newsecond = 0;
    /* Compute primary chain lengths. */
    for (i = oldmask; i != ~(MSize)0; i--) {
      GCobj *o = lj_str_hashhead_acq(&oldtab[i]);
      while (o) {
	GCstr *s = gco2str(o);
	MSize hash = s->hashalg ? hash_sparse(g->str.seed, strdata(s), s->len) :
				  s->hash;
	hash &= newmask;
	lj_str_ref_store_rel(&newtab[hash],
			     lj_str_ref_load_acq(&newtab[hash]) + 1u);
	o = lj_str_next_acq(o);
      }
    }
    /* Mark secondary chains. */
    for (i = newmask; i != ~(MSize)0; i--) {
      int secondary = lj_str_ref_load_acq(&newtab[i]) > LJ_STR_MAXCOLL;
      newsecond |= secondary;
      lj_str_ref_store_rel(&newtab[i],
			   secondary ? LJ_STRHASH_SECONDARY : (uintptr_t)0);
    }
    g->str.second = newsecond;
  }
#endif

  /* Reinsert all strings from the old table into the new table. */
  for (i = oldmask; i != ~(MSize)0; i--) {
    GCobj *o = lj_str_hashhead_acq(&oldtab[i]);
    while (o) {
      GCobj *next = lj_str_next_acq(o);
      GCstr *s = gco2str(o);
      MSize hash = s->hash;
#if LUAJIT_SECURITY_STRHASH
      uintptr_t u;
      if (LJ_LIKELY(!s->hashalg)) {  /* String hashed with primary hash. */
	hash &= newmask;
	u = lj_str_ref_load_acq(&newtab[hash]);
	if (LJ_UNLIKELY(u & LJ_STRHASH_SECONDARY)) {  /* Switch string to secondary hash. */
	  s->hash = hash = hash_dense(g->str.seed, s->hash, strdata(s), s->len);
	  s->hashalg = 1;
	  hash &= newmask;
	  u = lj_str_ref_load_acq(&newtab[hash]);
	}
      } else {  /* String hashed with secondary hash. */
	MSize shash = hash_sparse(g->str.seed, strdata(s), s->len);
	u = lj_str_ref_load_acq(&newtab[shash & newmask]);
	if (u & LJ_STRHASH_SECONDARY) {
	  hash &= newmask;
	  u = lj_str_ref_load_acq(&newtab[hash]);
	} else {  /* Revert string back to primary hash. */
	  s->hash = shash;
	  s->hashalg = 0;
	  hash = (shash & newmask);
	}
      }
      /* NOBARRIER: The string table is a GC root. */
      lj_str_next_store_rel(o, u);
      lj_str_bucket_store_rel(&newtab[hash], o, u);
#else
      hash &= newmask;
      u = lj_str_ref_load_acq(&newtab[hash]);
      /* NOBARRIER: The string table is a GC root. */
      lj_str_next_store_rel(o, u);
      lj_str_bucket_store_rel(&newtab[hash], o, 0);
#endif
      o = next;
    }
  }

  /* Retire old table and replace with new table. */
  g->str.mask = newmask;
  lj_str_tabh_rel(g, newhdr);
  if (oldhdr)
    strtab_retire(g, oldhdr);
}

#if LUAJIT_SECURITY_STRHASH
/* Rehash and rechain all strings in a chain. */
static LJ_NOINLINE GCstr *lj_str_rehash_chain(lua_State *L, StrHash hashc,
					      const char *str, MSize len)
{
  global_State *g = G(L);
  int ow = g->gc.state == GCSsweepstring ? otherwhite(g) : 0;  /* Sweeping? */
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  GCRef *strtab;
  MSize strmask;
  GCobj *o;
  if (!hdr || !strtab_claim(L, hdr))
    return lj_str_new(L, str, len);
  if (lj_str_tabh_acq(g) != hdr) {
    strtab_release(hdr);
    return lj_str_new(L, str, len);
  }
  strtab = hdr->bucket;
  strmask = hdr->mask;
  o = lj_str_hashhead_acq(&strtab[hashc & strmask]);
  lj_str_ref_store_rel(&strtab[hashc & strmask], LJ_STRHASH_SECONDARY);
  g->str.second = 1;
  while (o) {
    uintptr_t u;
    GCobj *next = lj_str_next_acq(o);
    GCstr *s = gco2str(o);
    StrHash hash;
    if (ow) {  /* Must sweep while rechaining. */
      if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* String alive? */
	lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		   "sweep of undead string");
	makewhite(g, o);
      } else {  /* Free dead string. */
	lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		   "sweep of unlive string");
	lj_str_free(g, s);
	o = next;
	continue;
      }
    }
    hash = s->hash;
    if (!s->hashalg) {  /* Rehash with secondary hash. */
      hash = hash_dense(g->str.seed, hash, strdata(s), s->len);
      s->hash = hash;
      s->hashalg = 1;
    }
    /* Rechain. */
    hash &= strmask;
    u = lj_str_ref_load_acq(&strtab[hash]);
    lj_str_next_store_rel(o, u);
    lj_str_bucket_store_rel(&strtab[hash], o, u);
    o = next;
  }
  strtab_release(hdr);
  /* Try to insert the pending string again. */
  return lj_str_new(L, str, len);
}
#endif

/* Allocate a new, unpublished string object. */
static GCstr *lj_str_alloc(lua_State *L, const char *str, MSize len,
			   StrHash hash, int hashalg)
{
  GCstr *s = lj_mem_newt(L, lj_str_size(len), GCstr);
  global_State *g = G(L);
  newwhite(g, s);
  s->gct = ~LJ_TSTR;
  s->len = len;
  s->hash = hash;
  s->sid = (StrID)la_add32_rlx(&g->str.id, 1);
  s->reserved = 0;
  s->hashalg = (uint8_t)hashalg;
  /* Clear last 4 bytes of allocated memory. Implies zero-termination, too. */
  *(uint32_t *)(strdatawr(s)+(len & ~(MSize)3)) = 0;
  memcpy(strdatawr(s), str, len);
  return s;
}

/* Intern a string and return string object. */
GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
  global_State *g = G(L);
  if (lenx-1 < LJ_MAX_STR-1) {
    MSize len = (MSize)lenx;
    StrHash hash = hash_sparse(g->str.seed, str, len);
    GCstr *news = NULL;
    int hashalg = 0;
    for (;;) {
      StrTabHdr *hdr = strtab_enter(L, g);
      GCRef *strtab;
      GCobj *o;
      MSize mask, coll = 0;
      uintptr_t u;
      int grow = 0;
      if (LJ_UNLIKELY(hdr == NULL)) {
	if (news)
	  lj_mem_free(g, news, lj_str_size(news->len));
	return lj_str_alloc(L, str, len, hash, 0);
      }

      mask = hdr->mask;
      strtab = hdr->bucket;
      hashalg = 0;
      hash = hash_sparse(g->str.seed, str, len);
      u = lj_str_ref_load_acq(&strtab[hash & mask]);
      o = lj_str_hashhead_u(u);
#if LUAJIT_SECURITY_STRHASH
      if (LJ_UNLIKELY(u & LJ_STRHASH_SECONDARY)) {
	hashalg = 1;
	hash = hash_dense(g->str.seed, hash, str, len);
	u = lj_str_ref_load_acq(&strtab[hash & mask]);
	o = lj_str_hashhead_u(u);
      }
#endif
      while (o != NULL) {
	GCstr *sx = gco2str(o);
	if (sx->hash == hash && sx->len == len) {
	  if (memcmp(str, strdata(sx), len) == 0) {
	    if (isdead(g, o)) {  /* Resurrect if dead. */
	      flipwhite(o);
	      lj_gc_arena_markobj(g, o);
	    }
	    strtab_leave(hdr);
	    if (news)
	      lj_mem_free(g, news, lj_str_size(news->len));
	    return sx;  /* Return existing string. */
	  }
	  coll++;
	}
	coll++;
	o = lj_str_next_acq(o);
      }
#if LUAJIT_SECURITY_STRHASH
      /* Rehash chain if there are too many collisions. */
      if (LJ_UNLIKELY(coll > LJ_STR_MAXCOLL) && !hashalg) {
	strtab_leave(hdr);
	if (news)
	  lj_mem_free(g, news, lj_str_size(news->len));
	return lj_str_rehash_chain(L, hash, str, len);
      }
#endif
      if (news == NULL) {
	strtab_leave(hdr);
	news = lj_str_alloc(L, str, len, hash, hashalg);
	continue;
      }

      news->hash = hash;
      news->hashalg = (uint8_t)hashalg;
      for (;;) {
	GCRef *head = &strtab[hash & mask];
	uintptr_t want;
	u = lj_str_ref_load_acq(head);  /* 06 section 6.5 bucket snapshot. */
	o = lj_str_hashhead_u(u);
	while (o != NULL) {
	  GCstr *sx = gco2str(o);
	  if (sx->hash == hash && sx->len == len &&
	      memcmp(str, strdata(sx), len) == 0) {
	    if (isdead(g, o)) {
	      flipwhite(o);
	      lj_gc_arena_markobj(g, o);
	    }
	    strtab_leave(hdr);
	    lj_mem_free(g, news, lj_str_size(news->len));
	    return sx;
	  }
	  o = lj_str_next_acq(o);
	}
	lj_str_next_store_rel(obj2gco(news), u);
	want = (uintptr_t)news | (u & LJ_STRHASH_SECONDARY);
	if (strref_cas_rel(head, &u, want)) {  /* 06 section 6.5 intern linearization. */
	  MSize n = la_add32_rlx(&g->str.num, 1) + 1u;
	  grow = n > mask;
	  break;
	}
      }
      strtab_leave(hdr);
      if (grow)
	lj_str_resize(L, (mask << 1) + 1u);  /* Grow string table. */
      return news;  /* Return newly interned string. */
    }
  } else {
    if (lenx)
      lj_err_msg(L, LJ_ERR_STROV);
    return &g->strempty;
  }
}

void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s)
{
  (void)la_sub32_acqrel(&g->str.num, 1);
  lj_mem_free(g, s, lj_str_size(s->len));
}

void LJ_FASTCALL lj_str_init(lua_State *L)
{
  global_State *g = G(L);
  g->str.seed = lj_prng_u64(&g->prng);
  lj_str_resize(L, LJ_MIN_STRTAB-1);
}

uint32_t lj_str_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  StrTabHdr *hdr;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  hdr = lj_str_retired_head_xchg_acqrel(g, NULL);
  while (hdr) {
    StrTabHdr *next = lj_str_retired_next_acq(hdr);
    lj_str_retired_next_rel(hdr, NULL);
    if (lj_str_retire_epoch_acq(hdr) < completed_epoch) {
      lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
      reclaimed++;
    } else {
      strtab_retired_push(g, hdr);
    }
    hdr = next;
  }
  return reclaimed;
}

void lj_str_freetab(global_State *g)
{
  StrTabHdr *hdr = lj_str_tabh_xchg_acqrel(g, NULL);
  if (hdr) {
    lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
  }
  hdr = lj_str_retired_head_xchg_acqrel(g, NULL);
  while (hdr) {
    StrTabHdr *next = lj_str_retired_next_acq(hdr);
    lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
    hdr = next;
  }
}
