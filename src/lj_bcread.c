/*
** Bytecode reader.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_bcread_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_bc.h"
#include "lj_debug.h"
#include "lj_frame.h"
#include "lj_vm.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lualib.h"
#endif
#include "lj_lex.h"
#include "lj_bcdump.h"
#include "lj_state.h"
#include "lj_strfmt.h"

/* Reuse some lexer fields for our own purposes. */
#define bcread_flags(ls)	ls->level
#define BCREAD_VERSION_SHIFT	24
#define BCREAD_FLAG_MASK	((1u << BCREAD_VERSION_SHIFT)-1u)
#define bcread_dumpflags(ls)	(bcread_flags(ls) & BCREAD_FLAG_MASK)
#define bcread_version(ls) \
  ((bcread_flags(ls) >> BCREAD_VERSION_SHIFT) ? \
   (bcread_flags(ls) >> BCREAD_VERSION_SHIFT) : BCDUMP_VERSION)
#define bcread_saveflags(ls, flags, version) \
  (bcread_flags(ls) = (flags) | ((uint32_t)(version) << BCREAD_VERSION_SHIFT))
#define bcread_swap(ls) \
  ((bcread_dumpflags(ls) & BCDUMP_F_BE) != LJ_BE*BCDUMP_F_BE)
#define bcread_oldtop(L, ls)	restorestack(L, ls->lastline)
#define bcread_savetop(L, ls, top) \
  ls->lastline = (BCLine)savestack(L, (top))

/* -- Input buffer handling ----------------------------------------------- */

/* Throw reader error. */
static LJ_NOINLINE void bcread_error(LexState *ls, ErrMsg em)
{
  lua_State *L = ls->L;
  const char *name = ls->chunkarg;
  if (*name == BCDUMP_HEAD1) name = "(binary)";
  else if (*name == '@' || *name == '=') name++;
  lj_strfmt_pushf(L, "%s: %s", name, err2msg(em));
  lj_err_throw(L, LUA_ERRSYNTAX);
}

/* Refill buffer. */
static LJ_NOINLINE void bcread_fill(LexState *ls, MSize len, int need)
{
  lj_assertLS(len != 0, "empty refill");
  if (len > LJ_MAX_BUF || ls->c < 0)
    bcread_error(ls, LJ_ERR_BCBAD);
  do {
    const char *buf;
    size_t sz;
    char *p = ls->sb.b;
    MSize n = (MSize)(ls->pe - ls->p);
    if (n) {  /* Copy remainder to buffer. */
      if (sbuflen(&ls->sb)) {  /* Move down in buffer. */
	lj_assertLS(ls->pe == ls->sb.w, "bad buffer pointer");
	if (ls->p != p) memmove(p, ls->p, n);
      } else {  /* Copy from buffer provided by reader. */
	p = lj_buf_need(&ls->sb, len);
	memcpy(p, ls->p, n);
      }
      ls->p = p;
      ls->pe = p + n;
    }
    ls->sb.w = p + n;
    buf = ls->rfunc(ls->L, ls->rdata, &sz);  /* Get more data from reader. */
    if (buf == NULL || sz == 0) {  /* EOF? */
      if (need) bcread_error(ls, LJ_ERR_BCBAD);
      ls->c = -1;  /* Only bad if we get called again. */
      break;
    }
    if (sz >= LJ_MAX_BUF - n) lj_err_mem(ls->L);
    if (n) {  /* Append to buffer. */
      n += (MSize)sz;
      p = lj_buf_need(&ls->sb, n < len ? len : n);
      memcpy(ls->sb.w, buf, sz);
      ls->sb.w = p + n;
      ls->p = p;
      ls->pe = p + n;
    } else {  /* Return buffer provided by reader. */
      ls->p = buf;
      ls->pe = buf + sz;
    }
  } while ((MSize)(ls->pe - ls->p) < len);
}

/* Need a certain number of bytes. */
static LJ_AINLINE void bcread_need(LexState *ls, MSize len)
{
  if (LJ_UNLIKELY((MSize)(ls->pe - ls->p) < len))
    bcread_fill(ls, len, 1);
}

/* Want to read up to a certain number of bytes, but may need less. */
static LJ_AINLINE void bcread_want(LexState *ls, MSize len)
{
  if (LJ_UNLIKELY((MSize)(ls->pe - ls->p) < len))
    bcread_fill(ls, len, 0);
}

/* Reject malformed fields at the declared prototype boundary, rather than
** letting the final exact-length check observe an overrun after the read. */
static LJ_AINLINE void bcread_checkmem(LexState *ls, MSize len)
{
  const char *end = ls->bcend ? ls->bcend : ls->pe;
  if (LJ_UNLIKELY(ls->p > end || len > (MSize)(end - ls->p)))
    bcread_error(ls, LJ_ERR_BCBAD);
}

/* Return memory block from buffer. */
static LJ_AINLINE uint8_t *bcread_mem(LexState *ls, MSize len)
{
  uint8_t *p = (uint8_t *)ls->p;
  bcread_checkmem(ls, len);
  ls->p += len;
  return p;
}

/* Copy memory block from buffer. */
static void bcread_block(LexState *ls, void *q, MSize len)
{
  memcpy(q, bcread_mem(ls, len), len);
}

/* Read byte from buffer. */
static LJ_AINLINE uint32_t bcread_byte(LexState *ls)
{
  bcread_checkmem(ls, 1);
  return (uint32_t)(uint8_t)*ls->p++;
}

/* Read ULEB128 value from buffer. */
static LJ_AINLINE uint32_t bcread_uleb128(LexState *ls)
{
  const uint8_t *p = (const uint8_t *)ls->p;
  uint32_t v = 0;
  int sh;
  for (sh = 0; sh <= 28; sh += 7) {
    uint32_t b;
    bcread_checkmem(ls, (MSize)((const char *)p - ls->p + 1));
    b = *p++;
    if (LJ_UNLIKELY(sh == 28 && (b & 0x70u) != 0))
      bcread_error(ls, LJ_ERR_BCBAD);
    v |= (b & 0x7fu) << sh;
    if ((b & 0x80u) == 0) {
      ls->p = (const char *)p;
      return v;
    }
  }
  bcread_error(ls, LJ_ERR_BCBAD);
  return 0;
}

/* Read top 32 bits of 33 bit ULEB128 value from buffer. */
static uint32_t bcread_uleb128_33(LexState *ls)
{
  const uint8_t *p = (const uint8_t *)ls->p;
  uint32_t b, v;
  int sh;
  bcread_checkmem(ls, 1);
  b = *p++;
  v = (b & 0x7fu) >> 1;
  if (b & 0x80u) {
    for (sh = 6; sh <= 27; sh += 7) {
      bcread_checkmem(ls, (MSize)((const char *)p - ls->p + 1));
      b = *p++;
      if (LJ_UNLIKELY(sh == 27 && (b & 0x60u) != 0))
	bcread_error(ls, LJ_ERR_BCBAD);
      v |= (b & 0x7fu) << sh;
      if ((b & 0x80u) == 0) {
	ls->p = (const char *)p;
	return v;
      }
    }
    bcread_error(ls, LJ_ERR_BCBAD);
  }
  ls->p = (const char *)p;
  return v;
}

/* -- Bytecode reader ----------------------------------------------------- */

/* Read debug info of a prototype. */
static void bcread_dbg(LexState *ls, GCproto *pt, MSize sizedbg)
{
  void *lineinfo = (void *)proto_lineinfo(pt);
  bcread_block(ls, lineinfo, sizedbg);
  /* Swap lineinfo if the endianess differs. */
  if (bcread_swap(ls) && pt->numline >= 256) {
    MSize i, n = pt->sizebc-1;
    if (pt->numline < 65536) {
      uint16_t *p = (uint16_t *)lineinfo;
      for (i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8)|(p[i] << 8));
    } else {
      uint32_t *p = (uint32_t *)lineinfo;
      for (i = 0; i < n; i++) p[i] = lj_bswap(p[i]);
    }
  }
}

static const uint8_t *bcread_dbguleb(LexState *ls, const uint8_t *p,
				     const uint8_t *end)
{
  int i;
  for (i = 0; i < 5; i++) {
    uint32_t b;
    if (LJ_UNLIKELY(p >= end))
      bcread_error(ls, LJ_ERR_BCBAD);
    b = *p++;
    if (LJ_UNLIKELY(i == 4 && (b & 0x70u) != 0))
      bcread_error(ls, LJ_ERR_BCBAD);
    if ((b & 0x80u) == 0)
      return p;
  }
  bcread_error(ls, LJ_ERR_BCBAD);
  return NULL;
}

/* Find and validate the colocated uvinfo/varinfo split. */
static const void *bcread_varinfo(LexState *ls, GCproto *pt,
				  const uint8_t *end)
{
  const uint8_t *p = proto_uvinfo(pt);
  MSize n = pt->sizeuv;
  while (n) {
    if (LJ_UNLIKELY(p >= end))
      bcread_error(ls, LJ_ERR_BCBAD);
    if (*p++ == 0)
      n--;
  }
  {
    const uint8_t *varinfo = p;
    for (;;) {
      uint32_t vn;
      if (LJ_UNLIKELY(p >= end))
	bcread_error(ls, LJ_ERR_BCBAD);
      vn = *p;
      if (vn == VARNAME_END)
	return varinfo;
      if (vn >= VARNAME__MAX) {
	do {
	  if (LJ_UNLIKELY(p >= end))
	    bcread_error(ls, LJ_ERR_BCBAD);
	} while (*p++ != 0);
      } else {
	p++;
      }
      p = bcread_dbguleb(ls, p, end);
      p = bcread_dbguleb(ls, p, end);
    }
  }
}

/* Read a single constant key/value of a template table. */
static void bcread_ktabk(LexState *ls, TValue *o, GCtab *t)
{
  MSize tp = bcread_uleb128(ls);
  if (tp >= BCDUMP_KTAB_STR) {
    MSize len = tp - BCDUMP_KTAB_STR;
    const char *p = (const char *)bcread_mem(ls, len);
    GCstr *str = lj_str_new(ls->L, p, len);
    setstrV(ls->L, o, str);
    lj_gc_pubobjroot(ls->L, obj2gco(str));
  } else if (tp == BCDUMP_KTAB_INT) {
    setintV(o, (int32_t)bcread_uleb128(ls));
  } else if (tp == BCDUMP_KTAB_NUM) {
    o->u32.lo = bcread_uleb128(ls);
    o->u32.hi = bcread_uleb128(ls);
  } else if (t && tp == BCDUMP_KTAB_NIL) { /* Restore nil value marker. */
    settabV(ls->L, o, t);
  } else {
    lj_assertLS(tp <= BCDUMP_KTAB_TRUE, "bad constant type %d", tp);
    setpriV(o, ~tp);
  }
}

/* Read a template table. */
static GCtab *bcread_ktab(LexState *ls, TValue *anchor)
{
  lua_State *L = ls->L;
  TGState *tg = L2TG(L);
  MSize narray = bcread_uleb128(ls);
  MSize nhash = bcread_uleb128(ls);
  GCtab *t = lj_tab_new(L, narray, hsize2hbits(nhash));
  TValue nilv, tv;
  TValue *keyanchor, *valanchor;
  uint32_t keyidx, validx;
  lj_gc_pubobjroot(L, obj2gco(t));
  settabV(L, &tv, t);
  copyTVrel(L, anchor, &tv);
  lj_gc_pubroot(L, anchor);
  /* Reusable operand anchors span value parsing and any table resize/allocation
  ** in the eventual store. Outer loader cleanup owns exceptional unwinding. */
  setnilV(&nilv);
  keyanchor = lj_tg_root_anchor_push(L, tg, &nilv, &keyidx);
  if (LJ_UNLIKELY(keyanchor == NULL))
    lj_err_mem(L);
  valanchor = lj_tg_root_anchor_push(L, tg, &nilv, &validx);
  if (LJ_UNLIKELY(valanchor == NULL))
    lj_err_mem(L);
  if (narray) {  /* Read array entries. */
    MSize i;
    TValue *o;
    (void)lj_tab_array_snapshot_acq(t, &o);
    for (i = 0; i < narray; i++, o++) {
      TValue tv;
      bcread_ktabk(ls, &tv, NULL);
      copyTVrel(L, valanchor, &tv);
      lj_gc_pubroot(L, valanchor);
      lj_tab_storetv(L, o, &tv);
      lj_gc_pubtabtv(L, t, &tv);
      copyTVrel(L, valanchor, &nilv);
    }
  }
  if (nhash) {  /* Read hash entries. */
    MSize i;
    for (i = 0; i < nhash; i++) {
      TValue key, tv;
      bcread_ktabk(ls, &key, NULL);
      lj_assertLS(!tvisnil(&key), "nil key");
      copyTVrel(L, keyanchor, &key);
      lj_gc_pubroot(L, keyanchor);
      bcread_ktabk(ls, &tv, t);
      copyTVrel(L, valanchor, &tv);
      lj_gc_pubroot(L, valanchor);
      lj_tab_storetv(L, lj_tab_set(L, t, &key), &tv);
      lj_gc_pubtabkey(L, t, &key);
      lj_gc_pubtabtv(L, t, &tv);
      copyTVrel(L, valanchor, &nilv);
      copyTVrel(L, keyanchor, &nilv);
    }
  }
  lj_tg_root_anchor_pop(tg, validx);
  lj_tg_root_anchor_pop(tg, keyidx);
  return t;
}

/* Read GC constants of a prototype. */
static uint32_t bcread_kgc(LexState *ls, GCproto *pt, MSize sizekgc)
{
  lua_State *L = ls->L;
  TGState *tg = L2TG(L);
  uint32_t anchor_base = lj_tg_root_anchor_top_acq(tg);
  MSize i;
  GCRef *kr = mref(pt->k, GCRef) - (ptrdiff_t)sizekgc;
  for (i = 0; i < sizekgc; i++, kr++) {
    TValue nilv, rootv;
    TValue *anchor;
    GCobj *o = NULL;
    MSize tp = bcread_uleb128(ls);
    setnilV(&nilv);
    anchor = lj_tg_root_anchor_push(L, tg, &nilv, NULL);
    if (LJ_UNLIKELY(anchor == NULL))
      lj_err_mem(L);
    if (tp >= BCDUMP_KGC_STR) {
      MSize len = tp - BCDUMP_KGC_STR;
      const char *p = (const char *)bcread_mem(ls, len);
      o = obj2gco(lj_str_new(L, p, len));
      lj_gc_pubobjroot(L, o);
    } else if (tp == BCDUMP_KGC_TAB) {
      o = obj2gco(bcread_ktab(ls, anchor));
#if LJ_HASFFI
    } else if (tp != BCDUMP_KGC_CHILD) {
      CTypeID id = tp == BCDUMP_KGC_COMPLEX ? CTID_COMPLEX_DOUBLE :
		   tp == BCDUMP_KGC_I64 ? CTID_INT64 : CTID_UINT64;
      CTSize sz = tp == BCDUMP_KGC_COMPLEX ? 16 : 8;
      GCcdata *cd = lj_cdata_new_(L, id, sz);
      TValue *p = (TValue *)cdataptr(cd);
      o = obj2gco(cd);
      lj_gc_pubobjroot(L, o);
      setcdataV(L, &rootv, cd);
      copyTVrel(L, anchor, &rootv);
      lj_gc_pubroot(L, anchor);
      p[0].u32.lo = bcread_uleb128(ls);
      p[0].u32.hi = bcread_uleb128(ls);
      if (tp == BCDUMP_KGC_COMPLEX) {
	p[1].u32.lo = bcread_uleb128(ls);
	p[1].u32.hi = bcread_uleb128(ls);
      }
#endif
    } else {
      GCproto *child;
      lj_assertLS(tp == BCDUMP_KGC_CHILD, "bad constant type %d", tp);
      if (L->top <= bcread_oldtop(L, ls))  /* Stack underflow? */
	bcread_error(ls, LJ_ERR_BCBAD);
      child = protoV(L->top-1);
      o = obj2gco(child);
      setprotoV(L, &rootv, child);
      copyTVrel(L, anchor, &rootv);
      lj_gc_pubroot(L, anchor);
      L->top--;
    }
    lj_assertLS(o != NULL, "missing bytecode GC constant");
    if (tvisnil(anchor)) {
      setgcV(L, &rootv, o, (uint32_t)(uint8_t)~o->gch.gct);
      copyTVrel(L, anchor, &rootv);
      lj_gc_pubroot(L, anchor);
    }
    setgcrefrel(*kr, o);
  }
  return anchor_base;
}

/* Read number constants of a prototype. */
static void bcread_knum(LexState *ls, GCproto *pt, MSize sizekn)
{
  MSize i;
  TValue *o = mref(pt->k, TValue);
  for (i = 0; i < sizekn; i++, o++) {
    bcread_checkmem(ls, 1);
    int isnum = (ls->p[0] & 1);
    uint32_t lo = bcread_uleb128_33(ls);
    if (isnum) {
      o->u32.lo = lo;
      o->u32.hi = bcread_uleb128(ls);
    } else {
      setintV(o, lo);
    }
  }
}

/* Read bytecode instructions. */
static void bcread_bytecode(LexState *ls, GCproto *pt, MSize sizebc)
{
  BCIns *bc = proto_bc(pt);
  BCIns op;
  if (ls->fr2 != LJ_FR2) op = BC_NOT;  /* Mark non-native prototype. */
  else if ((pt->flags & PROTO_VARARG)) op = BC_FUNCV;
  else op = BC_FUNCF;
  bc[0] = BCINS_AD(op, pt->framesize, 0);
  bcread_block(ls, bc+1, (sizebc-1)*(MSize)sizeof(BCIns));
  /* Swap bytecode instructions if the endianess differs. */
  if (bcread_swap(ls)) {
    MSize i;
    for (i = 1; i < sizebc; i++) bc[i] = lj_bswap(bc[i]);
  }
}

/* Verify bytecode instructions after endian normalization. */
enum {
  BCREAD_CELL_ACCESS = 0x01,
  BCREAD_CELL_CNEW = 0x02
};

static int bcread_verify_bytecode(LexState *ls, GCproto *pt)
{
  BCIns *bc = proto_bc(pt);
  MSize i;
  int cellops = 0;

  for (i = 1; i < pt->sizebc; i++) {
    BCIns ins = bc[i];
    BCOp op = bc_op(ins);
    if (op >= BC__MAX)
      bcread_error(ls, LJ_ERR_BCBAD);
    if (bcread_version(ls) != BCDUMP_VERSION_LOCKLESS && op >= BC_CNEW)
      bcread_error(ls, LJ_ERR_BCBAD);
    switch (op) {
    case BC_CNEW:
      cellops |= BCREAD_CELL_CNEW;
      if (bc_a(ins) >= pt->framesize)
	bcread_error(ls, LJ_ERR_BCBAD);
      break;
    case BC_CGET:
    case BC_CSET:
      cellops |= BCREAD_CELL_ACCESS;
      if (bc_a(ins) >= pt->framesize || bc_d(ins) >= pt->framesize)
	bcread_error(ls, LJ_ERR_BCBAD);
      if (bc_a(ins) == bc_d(ins))
	bcread_error(ls, LJ_ERR_BCBAD);
      break;
    default:
      break;
    }
  }

  if (cellops) {
    for (i = 1; i < pt->sizebc; i++) {
      BCIns ins = bc[i];
      if (bc_op(ins) == BC_UCLO && bc_a(ins) != 0)
	bcread_error(ls, LJ_ERR_BCBAD);
    }
  }
  return cellops;
}

/* Read upvalue refs. */
static void bcread_uv(LexState *ls, GCproto *pt, MSize sizeuv)
{
  if (sizeuv) {
    uint16_t *uv = proto_uv(pt);
    bcread_block(ls, uv, sizeuv*2);
    /* Swap upvalue refs if the endianess differs. */
    if (bcread_swap(ls)) {
      MSize i;
      for (i = 0; i < sizeuv; i++)
	uv[i] = (uint16_t)((uv[i] >> 8)|(uv[i] << 8));
    }
  }
}

static int bcread_uv_haslocal(GCproto *pt)
{
  MSize i, sizeuv = pt->sizeuv;
  uint16_t *uv = proto_uv(pt);
  for (i = 0; i < sizeuv; i++)
    if (uv[i] & PROTO_UV_LOCAL)
      return 1;
  return 0;
}

typedef struct BCReadProtoCtx {
  LexState *ls;
  GCproto *pt;
  MSize sizept;
  uint32_t anchoridx;
  int published;
} BCReadProtoCtx;

/* Read a prototype body under the protected cancellation wrapper below. */
static GCproto *bcread_proto_body(LexState *ls, uint32_t *anchoridx,
				  BCReadProtoCtx *ctx)
{
  lua_State *L = ls->L;
  TGState *tg = L2TG(L);
  TValue nilv, ptv;
  TValue *anchor;
  uint32_t anchor_index, kgc_anchor_base;
  GCproto *pt;
  MSize framesize, numparams, flags, sizeuv, sizekgc, sizekn, sizebc, sizept;
  MSize ofsk, ofsuv, ofsdbg;
  uint64_t sizept64;
  uint32_t dump_proto_flags, allowed_flags;
  MSize sizedbg = 0;
  int cellops;
  BCLine firstline = 0, numline = 0;
  lj_assertLS(anchoridx != NULL, "missing bytecode construction anchor output");

  /* Read prototype header. */
  dump_proto_flags = bcread_byte(ls);
  allowed_flags = PROTO_CHILD|PROTO_VARARG|PROTO_FFI;
  if (bcread_version(ls) == BCDUMP_VERSION_LOCKLESS)
    allowed_flags |= BCDUMP_PF_LEGACYUV;
  if ((dump_proto_flags & ~allowed_flags) != 0)
    bcread_error(ls, LJ_ERR_BCBAD);
  flags = dump_proto_flags & (PROTO_CHILD|PROTO_VARARG|PROTO_FFI);
  numparams = bcread_byte(ls);
  framesize = bcread_byte(ls);
  sizeuv = bcread_byte(ls);
  sizekgc = bcread_uleb128(ls);
  sizekn = bcread_uleb128(ls);
  sizebc = bcread_uleb128(ls);
  if (LJ_UNLIKELY(sizebc == ~(MSize)0))
    bcread_error(ls, LJ_ERR_BCBAD);
  sizebc++;
  if (!(bcread_flags(ls) & BCDUMP_F_STRIP)) {
    sizedbg = bcread_uleb128(ls);
    if (sizedbg) {
      firstline = bcread_uleb128(ls);
      numline = bcread_uleb128(ls);
    }
  }

  /* Every declared element consumes at least this much serialized body. This
  ** rejects hostile counts before either size arithmetic or allocation. */
  if (ls->bcend) {
    uint64_t minbody = (uint64_t)(sizebc - 1u) * sizeof(BCIns) +
		       (uint64_t)sizeuv * 2u + sizekgc + sizekn + sizedbg;
    if (LJ_UNLIKELY(ls->p > ls->bcend ||
	minbody > (uint64_t)(ls->bcend - ls->p)))
      bcread_error(ls, LJ_ERR_BCBAD);
  }

  /* Calculate total size of prototype including all colocated arrays in a
  ** wide type. GCproto offsets are MSize even on GC64 builds. */
  sizept64 = (uint64_t)sizeof(GCproto) +
	     (uint64_t)sizebc * sizeof(BCIns) +
	     (uint64_t)sizekgc * sizeof(GCRef);
  sizept64 = (sizept64 + sizeof(TValue)-1u) &
	      ~((uint64_t)sizeof(TValue)-1u);
  ofsk = (MSize)sizept64;
  sizept64 += (uint64_t)sizekn * sizeof(TValue);
  ofsuv = (MSize)sizept64;
  sizept64 += (uint64_t)((sizeuv+1u)&~1u) * 2u;
  ofsdbg = (MSize)sizept64;
  sizept64 += sizedbg;
  if (LJ_UNLIKELY(sizept64 > LJ_MAX_MEM32))
    bcread_error(ls, LJ_ERR_BCBAD);
  sizept = (MSize)sizept64;

  /* Reserve the construction root before allocating the pending object. This
  ** may allocate an anchor block and throw, but cannot abandon a GC header. */
  setnilV(&nilv);
  anchor = lj_tg_root_anchor_push(L, tg, &nilv, &anchor_index);
  if (LJ_UNLIKELY(!anchor))
    lj_err_mem(L);

  /* Allocate prototype object and initialize its fields. */
  pt = (GCproto *)lj_mem_newgco_unlinked(ls->L, (MSize)sizept);
  ctx->pt = pt;
  ctx->sizept = sizept;
  pt->gct = ~LJ_TPROTO;
  /* Arena reuse does not zero raw constructor storage. Reset all GC flags now
  ** so a prior object's FIXED/FINREG/NEEDSCAN bits cannot survive the READY
  ** publication of a bytecode-loaded prototype. */
  newwhite(G(L), pt);
  pt->numparams = (uint8_t)numparams;
  pt->framesize = (uint8_t)framesize;
  pt->sizebc = sizebc;
  setmref(pt->k, (char *)pt + ofsk);
  setmref(pt->uv, (char *)pt + ofsuv);
  proto_sizekgc_rel(pt, 0);  /* Set to zero until fully initialized. */
  pt->sizekn = sizekn;
  pt->sizept = sizept;
  pt->sizeuv = (uint8_t)sizeuv;
  pt->flags = (uint8_t)flags;
  proto_initflags2(pt);
  if (bcread_version(ls) == BCDUMP_VERSION_LEGACY ||
      (bcread_version(ls) == BCDUMP_VERSION_LOCKLESS &&
       (dump_proto_flags & BCDUMP_PF_LEGACYUV)))
    proto_setlegacyuv(pt);
  pt->trace = 0;
  setgcrefrel(pt->chunkname, obj2gco(ls->chunkname));

  /* Close potentially uninitialized gap between bc and kgc. */
  *(uint32_t *)((char *)pt + ofsk - sizeof(GCRef)*(sizekgc+1)) = 0;

  /* Put the pending identity in a semantic anchor while keeping READY=0.
  ** Scanners reject the opaque body and pending sweep pins its allocation until
  ** bytecode, constants and debug data are all complete. */
  setprotoV(L, &ptv, pt);
  copyTVrel(L, anchor, &ptv);

  /* Read bytecode instructions and upvalue refs. */
  bcread_bytecode(ls, pt, sizebc);
  cellops = bcread_verify_bytecode(ls, pt);
  if (cellops) {
    if (proto_legacyuv(pt))
      bcread_error(ls, LJ_ERR_BCBAD);
    proto_setcellops(pt);
    proto_setcelluv(pt);
  }
  bcread_uv(ls, pt, sizeuv);
  if (bcread_version(ls) == BCDUMP_VERSION_LOCKLESS &&
      !proto_legacyuv(pt) && bcread_uv_haslocal(pt))
    proto_setcelluv(pt);
  /* Read constants. */
  kgc_anchor_base = bcread_kgc(ls, pt, sizekgc);
  proto_sizekgc_rel(pt, sizekgc);
  bcread_knum(ls, pt, sizekn);

  /* Read and initialize debug info. */
  pt->firstline = firstline;
  pt->numline = numline;
  if (sizedbg) {
    uint32_t lishift = numline < 256 ? 0u : numline < 65536 ? 1u : 2u;
    uint64_t sizeli64 = (uint64_t)(sizebc - 1u) << lishift;
    MSize sizeli;
    const uint8_t *dbgend = (const uint8_t *)pt + ofsdbg + sizedbg;
    if (LJ_UNLIKELY(sizeli64 > sizedbg))
      bcread_error(ls, LJ_ERR_BCBAD);
    sizeli = (MSize)sizeli64;
    setmref(pt->lineinfo, (char *)pt + ofsdbg);
    setmref(pt->uvinfo, (char *)pt + ofsdbg + sizeli);
    bcread_dbg(ls, pt, sizedbg);
    setmref(pt->varinfo, bcread_varinfo(ls, pt, dbgend));
  } else {
    setmref(pt->lineinfo, NULL);
    setmref(pt->uvinfo, NULL);
    setmref(pt->varinfo, NULL);
  }
  /* READY is the full-body publication point. The retained KGC anchors close
  ** child lifetimes while the parent is opaque; the post-READY root barrier
  ** transfers those leases to the complete prototype before they are popped. */
  lj_gc_publishobj_header(G(L), obj2gco(pt));
  lj_gc_pubroot(L, anchor);
  lj_gc_linkobj_new(G(L), obj2gco(pt));
  lj_gc_pubobjobj(L, pt, ls->chunkname);
  ctx->published = 1;
  while (lj_tg_root_anchor_top_acq(tg) > kgc_anchor_base)
    lj_tg_root_anchor_pop(tg, lj_tg_root_anchor_top_acq(tg) - 1u);
  *anchoridx = anchor_index;
  return pt;
}

static TValue *bcread_proto_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  BCReadProtoCtx *ctx = (BCReadProtoCtx *)ud;
  UNUSED(dummy);
  cframe_errfunc(L->cframe) = -1;
  ctx->pt = bcread_proto_body(ctx->ls, &ctx->anchoridx, ctx);
  return NULL;
}

GCproto *lj_bcread_proto(LexState *ls, uint32_t *anchoridx)
{
  lua_State *L = ls->L;
  TGState *tg = L2TG(L);
  BCReadProtoCtx ctx;
  uint32_t anchor_base = lj_tg_root_anchor_top_acq(tg);
  int errcode;
  lj_assertLS(anchoridx != NULL, "missing bytecode construction anchor output");
  ctx.ls = ls;
  ctx.pt = NULL;
  ctx.sizept = 0;
  ctx.anchoridx = 0;
  ctx.published = 0;
  errcode = lj_vm_cpcall(L, NULL, &ctx, bcread_proto_cp);
  if (LJ_UNLIKELY(errcode)) {
    /* A malformed/truncated body must not strand an opaque READY=0 allocation,
    ** which sweep deliberately pins. The body was never discoverable, so this
    ** protected owner can cancel it directly before dropping child anchors. */
    if (ctx.pt != NULL && !ctx.published)
      lj_mem_free(G(L), ctx.pt, ctx.sizept);
    while (lj_tg_root_anchor_top_acq(tg) > anchor_base)
      lj_tg_root_anchor_pop(tg,
			    lj_tg_root_anchor_top_acq(tg) - 1u);
    lj_err_throw(L, errcode);
  }
  lj_assertLS(ctx.published && ctx.pt != NULL &&
	      lj_tg_root_anchor_top_acq(tg) == anchor_base + 1u,
	      "bad bytecode prototype anchor handoff");
  *anchoridx = ctx.anchoridx;
  return ctx.pt;
}

/* Read and check header of bytecode dump. */
static int bcread_header(LexState *ls, TValue *chunkanchor)
{
  TValue chunkv;
  uint32_t flags, version;
  bcread_want(ls, 3+5+5);
  if (bcread_byte(ls) != BCDUMP_HEAD2 ||
      bcread_byte(ls) != BCDUMP_HEAD3) return 0;
  version = bcread_byte(ls);
  if (version != BCDUMP_VERSION_LEGACY &&
      version != BCDUMP_VERSION_TRANS &&
      version != BCDUMP_VERSION)
    return 0;
  flags = bcread_uleb128(ls);
  if ((flags & ~(BCDUMP_F_KNOWN)) != 0) return 0;
  bcread_saveflags(ls, flags, version);
  if ((flags & BCDUMP_F_FR2) != (uint32_t)ls->fr2*BCDUMP_F_FR2) return 0;
  if ((flags & BCDUMP_F_FFI)) {
#if LJ_HASFFI
    lua_State *L = ls->L;
    ctype_loadffi(L);
#else
    return 0;
#endif
  }
  if ((flags & BCDUMP_F_STRIP)) {
    ls->chunkname = lj_str_newz(ls->L, *ls->chunkarg == BCDUMP_HEAD1 ? "=?" : ls->chunkarg);
  } else {
    MSize len = bcread_uleb128(ls);
    bcread_need(ls, len);
    ls->chunkname = lj_str_new(ls->L, (const char *)bcread_mem(ls, len), len);
  }
  setstrV(ls->L, &chunkv, ls->chunkname);
  copyTVrel(ls->L, chunkanchor, &chunkv);
  lj_gc_pubroot(ls->L, chunkanchor);
  return 1;  /* Ok. */
}

/* Read a bytecode dump. */
GCproto *lj_bcread(LexState *ls, uint32_t *anchoridx)
{
  lua_State *L = ls->L;
  TGState *tg = L2TG(L);
  TValue nilv, ptv;
  TValue *chunkanchor;
  uint32_t chunkanchoridx;
  GCproto *result;
  lj_assertLS(anchoridx != NULL, "missing bytecode result anchor output");
  lj_assertLS(ls->c == BCDUMP_HEAD1, "bad bytecode header");
  bcread_savetop(L, ls, L->top);
  lj_buf_reset(&ls->sb);
  /* Reserve the result/chunk-name root before reading anything which can
  ** allocate. On success this exact slot is handed to cpparser; lua_loadx
  ** drains it on every protected error path. */
  setnilV(&nilv);
  chunkanchor = lj_tg_root_anchor_push(L, tg, &nilv, &chunkanchoridx);
  if (LJ_UNLIKELY(chunkanchor == NULL))
    lj_err_mem(L);
  /* Check for a valid bytecode dump header. */
  if (!bcread_header(ls, chunkanchor))
    bcread_error(ls, LJ_ERR_BCFMT);
  for (;;) {  /* Process all prototypes in the bytecode dump. */
    GCproto *pt;
    TValue stackptv;
    uint32_t anchoridx;
    MSize len;
    const char *startp;
    /* Read length. */
    if (ls->p < ls->pe && ls->p[0] == 0) {  /* Shortcut EOF. */
      ls->p++;
      break;
    }
    bcread_want(ls, 5);
    len = bcread_uleb128(ls);
    if (!len) break;  /* EOF */
    bcread_need(ls, len);
    startp = ls->p;
    ls->bcend = startp + len;
    pt = lj_bcread_proto(ls, &anchoridx);
    if (ls->p != ls->bcend)
      bcread_error(ls, LJ_ERR_BCBAD);
    ls->bcend = NULL;
    setprotoV(L, &stackptv, pt);
    copyTVrel(L, L->top, &stackptv);
    lj_state_stack_pubtv(L, L, L->top);
    incr_top(L);
    lj_tg_root_anchor_pop(L2TG(L), anchoridx);
  }
  if ((ls->pe != ls->p && !ls->endmark) || L->top-1 != bcread_oldtop(L, ls))
    bcread_error(ls, LJ_ERR_BCBAD);
  /* Transfer the final stack root into the caller-visible construction anchor
  ** before removing it from the Lua stack. */
  result = protoV(L->top-1);
  setprotoV(L, &ptv, result);
  copyTVrel(L, chunkanchor, &ptv);
  lj_gc_pubroot(L, chunkanchor);
  L->top--;
  *anchoridx = chunkanchoridx;
  return result;
}
