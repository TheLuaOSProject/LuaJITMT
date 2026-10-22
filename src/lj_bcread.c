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
      if (tvisnil(&key)) bcread_error(ls, LJ_ERR_BCBAD);
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
      /* Self-CGET unboxes a closing local without clobbering multires slots. */
      if (op == BC_CSET && bc_a(ins) == bc_d(ins))
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

/* -- Legacy local-cell normalization ------------------------------------- */

/* Stock bytecode refers to captured locals directly until UCLO closes them.
** Convert those accesses to the same cells used by the source compiler, before
** publishing any prototype. This preserves owner/closure aliasing in MT.
**
** The forward analysis distinguishes actual captures from cells the recorder
** may promote speculatively at trace entry. Raw definitions can discard the
** latter, but only UCLO ends an actual capture's lifetime. A backward scan
** bounds those speculative promotions by the recorder's definition rules.
** CGET accepts raw values, so joins may conservatively include cell states.
*/
typedef struct BCCellBits {
  uint64_t w[4];
} BCCellBits;
typedef struct BCCellIns {
  BCIns ins;
  BCPos origin, target;
} BCCellIns;
typedef struct BCCellCtx {
  LexState *ls;
  GCproto *pt;
  BCCellBits *active, *pending, *possible;
  uint32_t *queue, *map;
  uint8_t *seen, *queued;
  BCCellIns *out;
  MSize cap, nout;
  SBuf dbg;
} BCCellCtx;

static int bccell_has(const BCCellBits *s, BCReg r)
{
  return r < 256 && ((s->w[r >> 6] >> (r & 63)) & 1u);
}
static void bccell_set(BCCellBits *s, BCReg r)
{
  s->w[r >> 6] |= (uint64_t)1 << (r & 63);
}
static void bccell_clear(BCCellBits *s, BCReg r)
{
  s->w[r >> 6] &= ~((uint64_t)1 << (r & 63));
}
static void bccell_clear_from(BCCellBits *s, BCReg r)
{
  for (; r < 256; r++) s->w[r >> 6] &= ~((uint64_t)1 << (r & 63));
}
static int bccell_any_from(const BCCellBits *s, BCReg r)
{
  for (; r < 256; r++)
    if (bccell_has(s, r)) return 1;
  return 0;
}
static void bccell_free(BCCellCtx *c)
{
  global_State *g;
  MSize n;
  if (!c) return;
  g = G(c->ls->L);
  n = c->pt->sizebc;
  if (c->active) lj_mem_freevec(g, c->active, n, BCCellBits);
  if (c->pending) lj_mem_freevec(g, c->pending, n, BCCellBits);
  if (c->possible) lj_mem_freevec(g, c->possible, n, BCCellBits);
  if (c->queue) lj_mem_freevec(g, c->queue, n, uint32_t);
  if (c->map) lj_mem_freevec(g, c->map, n + 1, uint32_t);
  if (c->seen) lj_mem_freevec(g, c->seen, n, uint8_t);
  if (c->queued) lj_mem_freevec(g, c->queued, n, uint8_t);
  if (c->out) lj_mem_freevec(g, c->out, c->cap, BCCellIns);
  lj_buf_free(g, &c->dbg);
  lj_mem_freet(g, c);
}
static BCPos bccell_target(BCCellCtx *c, BCPos pc, BCIns ins)
{
  ptrdiff_t target = (ptrdiff_t)pc + 1 + bc_j(ins);
  if (target < 1 || target >= c->pt->sizebc) bcread_error(c->ls, LJ_ERR_BCBAD);
  return (BCPos)target;
}
static GCproto *bccell_child(BCCellCtx *c, BCIns ins)
{
  GCobj *o;
  if (bc_d(ins) >= c->pt->sizekgc) bcread_error(c->ls, LJ_ERR_BCBAD);
  o = proto_kgc(c->pt, ~(ptrdiff_t)bc_d(ins));
  if (o->gch.gct != ~LJ_TPROTO) bcread_error(c->ls, LJ_ERR_BCBAD);
  return gco2pt(o);
}
static void bccell_capture(BCCellCtx *c, BCIns ins, BCCellBits *s)
{
  GCproto *ch = bccell_child(c, ins);
  MSize i;
  for (i = 0; i < ch->sizeuv; i++) {
    uint32_t v = proto_uv(ch)[i];
    if (v & PROTO_UV_LOCAL) {
      if ((v & 255u) >= c->pt->framesize) bcread_error(c->ls, LJ_ERR_BCBAD);
      bccell_set(s, v & 255u);
    }
  }
}
static void bccell_merge(BCCellCtx *c, BCPos pc, const BCCellBits *s,
                         const BCCellBits *p, MSize *tail, MSize *pending)
{
  MSize i;
  int changed;
  if (pc < 1 || pc >= c->pt->sizebc) bcread_error(c->ls, LJ_ERR_BCBAD);
  changed = !c->seen[pc];
  c->seen[pc] = 1;
  for (i = 0; i < 4; i++) {
    uint64_t v = c->active[pc].w[i] | s->w[i];
    changed |= v != c->active[pc].w[i];
    c->active[pc].w[i] = v;
    v = c->possible[pc].w[i] | p->w[i];
    changed |= v != c->possible[pc].w[i];
    c->possible[pc].w[i] = v;
  }
  if (changed && !c->queued[pc]) {
    c->queued[pc] = 1;
    c->queue[*tail] = pc;
    *tail = (*tail + 1) % c->pt->sizebc;
    ++*pending;
  }
}
/* Validate every register window consumed by normalization, including dead
** instructions. Legacy readers historically accepted unchecked operands. */
static void bccell_validate(BCCellCtx *c)
{
  BCPos pc;
  BCReg frame = c->pt->framesize;
  for (pc = 1; pc < c->pt->sizebc; pc++) {
    BCIns ins = proto_bc(c->pt)[pc];
    BCOp op = bc_op(ins);
    BCReg a = bc_a(ins), b = bc_b(ins), d = bc_d(ins), cv = bc_c(ins);
    BCMode am = bcmode_a(op), bm = bcmode_b(op), dm = bcmode_d(op);
    if (((am == BCMvar || am == BCMdst) && a >= frame) ||
        ((am == BCMbase || am == BCMrbase) && a > frame) ||
        (bm == BCMvar && b >= frame) ||
        (dm == BCMvar && (bm == BCMnone ? d : cv) >= frame))
      bcread_error(c->ls, LJ_ERR_BCBAD);
    if ((op == BC_KNIL && (a > d || d >= frame)) ||
        (op == BC_CAT && (b > cv || cv >= frame)) ||
        ((op == BC_FORI || op == BC_FORL) && a + 3 >= frame) ||
        ((op == BC_ITERL || op == BC_ISNEXT || op == BC_ITERC ||
          op == BC_ITERN) &&
         a < 3) ||
        ((op == BC_ITERC || op == BC_ITERN) &&
         (a + b - 1 > frame || a > frame)) ||
        (op == BC_TSETM && a < 1) ||
        ((op == BC_CALL || op == BC_CALLM) &&
         (a >= frame || (b && a + b - 1 > frame) || a + cv + c->ls->fr2 > frame)) ||
        (op == BC_VARG && b && a + b - 1 > frame) ||
        (op == BC_RET1 && a >= frame) ||
        (op == BC_RET && (d < 1 || a + d - 1 > frame)))
      bcread_error(c->ls, LJ_ERR_BCBAD);
    /* Dumps contain the original interpreter opcodes, never trace IDs. */
    if (op == BC_JFORI || op == BC_IFORL || op == BC_JFORL || op == BC_IITERL ||
        op == BC_JITERL || op == BC_ILOOP || op == BC_JLOOP || op >= BC_FUNCF)
      bcread_error(c->ls, LJ_ERR_BCBAD);
  }
}
static void bccell_analyze(BCCellCtx *c)
{
  MSize n = c->pt->sizebc, head = 0, tail = 0, pending = 0;
  BCCellBits empty = {{0, 0, 0, 0}};
  bccell_merge(c, 1, &empty, &empty, &tail, &pending);
  while (pending) {
    BCPos pc = c->queue[head];
    BCIns ins = proto_bc(c->pt)[pc];
    BCOp op = bc_op(ins);
    BCCellBits s = c->active[pc], p = c->possible[pc];
    MSize i;
    for (i = 0; i < 4; i++) p.w[i] |= c->pending[pc].w[i];
    c->possible[pc] = p;
    head = (head + 1) % n;
    --pending;
    c->queued[pc] = 0;
    if (op == BC_FNEW) {
      bccell_capture(c, ins, &s);
      bccell_capture(c, ins, &p);
    }
    if (op == BC_UCLO) {
      bccell_clear_from(&s, bc_a(ins));
      bccell_clear_from(&p, bc_a(ins));
    } else if (bcmode_a(op) == BCMdst && op >= BC_MOV) {
      if (!bccell_has(&s, bc_a(ins))) bccell_clear(&p, bc_a(ins));
    } else if (op == BC_KNIL) {
      BCReg r;
      for (r = bc_a(ins); r <= bc_d(ins); r++)
        if (!bccell_has(&s, r)) bccell_clear(&p, r);
    } else if (op == BC_CALL || op == BC_CALLM || op == BC_ITERC ||
               op == BC_ITERN) {
      bccell_clear_from(&p, bc_a(ins));
    } else if (op == BC_VARG) {
      BCReg r, top = bc_b(ins) ? bc_a(ins) + bc_b(ins) - 1 : c->pt->framesize;
      for (r = bc_a(ins); r < top; r++) bccell_clear(&p, r);
    } else if (op == BC_FORI || op == BC_FORL) {
      BCReg r;
      for (r = bc_a(ins); r < bc_a(ins) + 4; r++) bccell_clear(&p, r);
    }

    if (op == BC_RET || op == BC_RET0 || op == BC_RET1 || op == BC_RETM ||
        op == BC_CALLT || op == BC_CALLMT)
      continue;
    if (op < BC_MOV) {
      bccell_merge(c, pc + 1, &s, &p, &tail, &pending);
      bccell_merge(c, pc + 2, &s, &p, &tail, &pending);
    } else if (op == BC_JMP || op == BC_UCLO || op == BC_ISNEXT) {
      bccell_merge(c, bccell_target(c, pc, ins), &s, &p, &tail, &pending);
    } else {
      bccell_merge(c, pc + 1, &s, &p, &tail, &pending);
      if (op == BC_FORI || op == BC_FORL || op == BC_ITERL)
        bccell_merge(c, bccell_target(c, pc, ins), &s, &p, &tail, &pending);
    }
  }
}
/* Trace entry may promote a future FNEW capture before its first use. Reads
** before that FNEW must use CGET too, as the source parser does. Match the
** recorder's pending scan, including its intervening-definition boundaries. */
static void bccell_pending(BCCellCtx *c)
{
  BCCellBits pending = {{0, 0, 0, 0}};
  BCPos pc = c->pt->sizebc;
  while (--pc) {
    BCIns ins = proto_bc(c->pt)[pc];
    BCOp op = bc_op(ins);
    BCReg r;
    MSize i;
    if (op == BC_FORL || op == BC_ITERL || op == BC_LOOP ||
        bc_isret_or_tail(op) || op == BC_UCLO)
      memset(&pending, 0, sizeof(pending));
    if (op == BC_FNEW) bccell_capture(c, ins, &pending);
    if (bcmode_a(op) == BCMdst) {
      r = bc_a(ins);
      pending.w[r >> 6] &= ~((uint64_t)1 << (r & 63));
    } else if (op == BC_KNIL) {
      for (r = bc_a(ins); r <= bc_d(ins); r++)
        pending.w[r >> 6] &= ~((uint64_t)1 << (r & 63));
    } else if (op == BC_CALL || op == BC_CALLM || op == BC_VARG ||
               op == BC_ITERC || op == BC_ITERN) {
      BCReg top = op == BC_VARG && bc_b(ins) ? bc_a(ins) + bc_b(ins) - 1
                                             : c->pt->framesize;
      for (r = bc_a(ins); r < top; r++)
        pending.w[r >> 6] &= ~((uint64_t)1 << (r & 63));
    } else if (op == BC_FORI || op == BC_FORL) {
      for (r = bc_a(ins); r < bc_a(ins) + 4; r++)
        pending.w[r >> 6] &= ~((uint64_t)1 << (r & 63));
    } else if (op == BC_ITERL || op == BC_ISNEXT) {
      r = bc_a(ins) - 1;
      pending.w[r >> 6] &= ~((uint64_t)1 << (r & 63));
    }
    for (i = 0; i < 4; i++) c->pending[pc].w[i] = pending.w[i];
  }
}
static void bccell_emit(BCCellCtx *c, BCIns ins, BCPos origin, BCPos target)
{
  if (c->nout == c->cap) {
    MSize cap = c->cap ? c->cap * 2 : 64;
    if (cap > LJ_MAX_BCINS || cap > LJ_MAX_MEM32 / sizeof(BCCellIns))
      bcread_error(c->ls, LJ_ERR_BCBAD);
    c->out = (BCCellIns *)lj_mem_realloc(
        c->ls->L, c->out, c->cap * sizeof(BCCellIns), cap * sizeof(BCCellIns));
    c->cap = cap;
  }
  c->out[c->nout].ins = ins;
  c->out[c->nout].origin = origin;
  c->out[c->nout++].target = target;
}
static void bccell_unbox(BCCellCtx *c, BCPos pc, const BCCellBits *s,
                         BCReg from, BCReg to)
{
  BCReg r;
  for (r = from; r < to; r++)
    if (bccell_has(s, r)) {
      bccell_emit(c, BCINS_AD(BC_CGET, r, r), pc, 0);
    }
}
static BCReg bccell_read(BCCellCtx *c, BCPos pc, const BCCellBits *s, BCReg r,
                         BCReg *scratch)
{
  if (r >= c->pt->framesize) bcread_error(c->ls, LJ_ERR_BCBAD);
  if (bccell_has(s, r)) {
    BCReg tmp = (*scratch)++;
    bccell_emit(c, BCINS_AD(BC_CGET, tmp, r), pc, 0);
    return tmp;
  }
  return r;
}
static void bccell_lower(BCCellCtx *c)
{
  BCPos pc;
  MSize i;
  BCReg tmp = c->pt->framesize;
  bccell_emit(c, proto_bc(c->pt)[0], 0, 0);
  c->map[0] = 0;
  for (pc = 1; pc < c->pt->sizebc; pc++) {
    BCIns ins = proto_bc(c->pt)[pc];
    BCOp op = bc_op(ins);
    BCCellBits s = c->active[pc], after = s, reads = c->possible[pc];
    BCReg a = bc_a(ins), scratch = tmp;
    int write = 0;
    c->map[pc] = c->nout;
    if (op == BC_UCLO) {
      bccell_unbox(c, pc, &reads, a, c->pt->framesize);
      bccell_emit(c, BCINS_AJ(BC_JMP, a, 0), pc, bccell_target(c, pc, ins));
      continue;
    }
    if (!c->seen[pc]) { /* Unreachable instructions cannot expose cells. */
      bccell_emit(c, ins, pc,
                  bcmode_d(op) == BCMjump ? bccell_target(c, pc, ins) : 0);
      continue;
    }
    if (op == BC_RET || op == BC_RET1 || op == BC_RETM || op == BC_CALLT ||
        op == BC_CALLMT) {
      bccell_unbox(c, pc, &reads, a, c->pt->framesize);
      bccell_emit(c, ins, pc, 0);
      continue;
    }
    if (op == BC_CALL || op == BC_CALLM || op == BC_VARG || op == BC_ITERC ||
        op == BC_ITERN || op == BC_TSETM) {
      BCReg floor = (op == BC_ITERC || op == BC_ITERN) ? a - 3
                    : op == BC_TSETM                   ? a - 1
                                                       : a;
      if (bccell_any_from(&s, floor)) bcread_error(c->ls, LJ_ERR_BCFMT);
      /* Discard speculative cells only where this instruction reads raw
      ** operands. Result slots may still be uninitialized at this boundary. */
      if (op == BC_CALL || op == BC_CALLM) {
        bccell_unbox(c, pc, &reads, a, a + 1);
        bccell_unbox(c, pc, &reads, a + 1 + c->ls->fr2, a + bc_c(ins) + c->ls->fr2);
      } else if (op == BC_ITERC || op == BC_ITERN) {
        bccell_unbox(c, pc, &reads, a - 3, a);
      } else if (op == BC_TSETM) {
        bccell_unbox(c, pc, &reads, a - 1, a);
      }
      bccell_emit(c, ins, pc, 0);
      continue;
    }
    if (op == BC_FORI || op == BC_FORL || op == BC_ITERL || op == BC_ISNEXT) {
      BCReg hi = op == BC_FORI || op == BC_FORL ? a + 4 : a + 1, r;
      BCReg lo = op == BC_ITERL || op == BC_ISNEXT ? a-1 : a;
      for (r = lo; r < hi; r++)
        if (bccell_has(&s, r)) bcread_error(c->ls, LJ_ERR_BCFMT);
      if (op == BC_FORI || op == BC_FORL) bccell_unbox(c, pc, &reads, a, a + 3);
      else if (op == BC_ISNEXT) bccell_unbox(c, pc, &reads, a - 3, a);
      else bccell_unbox(c, pc, &reads, a, a + 1);
      bccell_emit(c, ins, pc, bccell_target(c, pc, ins));
      continue;
    }
    if (op == BC_CAT) {
      BCReg r;
      int any = 0;
      for (r = bc_b(ins); r <= bc_c(ins); r++) any |= bccell_has(&reads, r);
      /* No contiguous raw scratch range is reserved. */
      if (any) bcread_error(c->ls, LJ_ERR_BCFMT);
    }
    if (op == BC_KNIL) {
      BCReg r;
      for (r = a; r <= bc_d(ins); r++) {
        if (bccell_has(&s, r)) {
          bccell_emit(c, BCINS_AD(BC_KPRI, tmp, ~LJ_TNIL), pc, 0);
          bccell_emit(c, BCINS_AD(BC_CSET, r, tmp), pc, 0);
        } else bccell_emit(c, BCINS_AD(BC_KPRI, r, ~LJ_TNIL), pc, 0);
      }
      continue;
    }
    if (op == BC_FNEW) {
      bccell_capture(c, ins, &after);
      if (bccell_has(&after, a) && !bccell_has(&s, a))
        bccell_emit(c, BCINS_AD(BC_CNEW, a, 0), pc, 0);
    }
    if (bcmode_a(op) == BCMvar)
      setbc_a(&ins, bccell_read(c, pc, &reads, a, &scratch));
    if (bcmode_b(op) == BCMvar)
      setbc_b(&ins, bccell_read(c, pc, &reads, bc_b(ins), &scratch));
    if (bcmode_d(op) == BCMvar) {
      if (bcmode_hasd(op))
        setbc_d(&ins, bccell_read(c, pc, &reads, bc_d(ins), &scratch));
      else setbc_c(&ins, bccell_read(c, pc, &reads, bc_c(ins), &scratch));
    }
    if (bcmode_a(op) == BCMdst && bccell_has(&after, a)) {
      write = 1;
      setbc_a(&ins, scratch++);
    }
    if (scratch > tmp + 3) bcread_error(c->ls, LJ_ERR_BCBAD);
    bccell_emit(c, ins, pc,
                bcmode_d(op) == BCMjump ? bccell_target(c, pc, ins) : 0);
    if (op < BC_MOV) {
      if (write) {
        bccell_emit(c, BCINS_AJ(BC_JMP, 0, 0), pc, 0x80000000u | (c->nout + 2));
        bccell_emit(c, BCINS_AJ(BC_JMP, 0, 0), pc, pc + 2);
        bccell_emit(c, BCINS_AD(BC_CSET, a, bc_a(ins)), pc, 0);
        bccell_emit(c, BCINS_AJ(BC_JMP, 0, 0), pc, pc + 1);
      } else {
        bccell_emit(c, BCINS_AJ(BC_JMP, 0, 0), pc, pc + 1);
        bccell_emit(c, BCINS_AJ(BC_JMP, 0, 0), pc, pc + 2);
      }
    } else if (write) bccell_emit(c, BCINS_AD(BC_CSET, a, bc_a(ins)), pc, 0);
  }
  c->map[c->pt->sizebc] = c->nout;
  for (i = 1; i < c->nout; i++) {
    if (bc_op(c->out[i].ins) == BC_ITERN &&
        (i+1 == c->nout || bc_op(c->out[i+1].ins) != BC_ITERL))
      bcread_error(c->ls, LJ_ERR_BCFMT);
  }
  for (i = 1; i < c->nout; i++)
    if (c->out[i].target) {
      BCPos t = c->out[i].target;
      ptrdiff_t delta =
          (ptrdiff_t)((t & 0x80000000u) ? t & 0x7fffffffu : c->map[t]) - i - 1;
      if (delta < -BCBIAS_J || delta >= BCBIAS_J)
        bcread_error(c->ls, LJ_ERR_XJUMP);
      setbc_j(&c->out[i].ins, delta);
    }
}
static uint32_t bccell_uleb(const uint8_t **pp)
{
  const uint8_t *p = *pp;
  uint32_t v = 0, shift = 0, b;
  do {
    b = *p++;
    v |= (b & 127u) << shift;
    shift += 7;
  } while (b & 128);
  *pp = p;
  return v;
}
static MSize bccell_debug(BCCellCtx *c)
{
  GCproto *pt = c->pt;
  MSize i, lishift = pt->numline < 256 ? 0 : pt->numline < 65536 ? 1 : 2, uvlen;
  const uint8_t *v = proto_varinfo(pt);
  BCPos oldlast = 0, newlast = 0;
  if (!proto_lineinfo(pt)) return 0;
  for (i = 1; i < c->nout; i++) {
    const char *p =
        (const char *)proto_lineinfo(pt) + ((c->out[i].origin - 1) << lishift);
    lj_buf_putmem(&c->dbg, p, (MSize)1 << lishift);
  }
  uvlen = (MSize)(v - proto_uvinfo(pt));
  lj_buf_putmem(&c->dbg, proto_uvinfo(pt), uvlen);
  while (*v) {
    const uint8_t *name = v;
    BCPos start, len, newstart, newend;
    char *p;
    if (*v < VARNAME__MAX) v++;
    else
      while (*v++);
    lj_buf_putmem(&c->dbg, name, (MSize)(v - name));
    start = bccell_uleb(&v);
    if (start > pt->sizebc-oldlast) bcread_error(c->ls, LJ_ERR_BCBAD);
    start += oldlast;
    len = bccell_uleb(&v);
    if (start > pt->sizebc || len > pt->sizebc - start)
      bcread_error(c->ls, LJ_ERR_BCBAD);
    newstart = c->map[start];
    newend = c->map[start + len];
    p = lj_buf_more(&c->dbg, 10);
    p = lj_strfmt_wuleb128(p, newstart - newlast);
    p = lj_strfmt_wuleb128(p, newend - newstart);
    c->dbg.w = p;
    oldlast = start;
    newlast = newstart;
  }
  lj_buf_putb(&c->dbg, 0);
  return sbuflen(&c->dbg);
}
static GCproto *bccell_normalize(LexState *ls, GCproto *pt, TValue *anchor,
                                 BCCellCtx **workp, GCproto **pending,
                                 MSize *pendingsz)
{
  BCCellCtx *c;
  BCCellBits captured = {{0, 0, 0, 0}};
  GCproto *np;
  lua_State *L = ls->L;
  MSize n = pt->sizebc, i, dbgsize, ofsk, ofsuv, ofsdbg, sizept,
        kgc = pt->sizekgc;
  uint64_t wide;
  int own = 0;
  if (!proto_legacyuv(pt) && bcread_version(ls) != BCDUMP_VERSION_TRANS)
    return pt;
  pt->flags2 &= ~PROTO2_LEGACYUV;
  if (bcread_uv_haslocal(pt)) proto_setcelluv(pt);
  for (i = 1; i < n; i++)
    if (bc_op(proto_bc(pt)[i]) == BC_FNEW) {
      own = 1;
      break;
    }
  if (!own) return pt;
  c = lj_mem_newt(L, sizeof(BCCellCtx), BCCellCtx);
  memset(c, 0, sizeof(*c));
  *workp = c;
  c->ls = ls;
  c->pt = pt;
  lj_buf_init(L, &c->dbg);
  for (i = 1; i < n; i++)
    if (bc_op(proto_bc(pt)[i]) == BC_FNEW)
      bccell_capture(c, proto_bc(pt)[i], &captured);
  if (!bccell_any_from(&captured, 0)) return pt;
  if (pt->framesize > LJ_MAX_SLOTS - 3) bcread_error(ls, LJ_ERR_XSLOTS);
  if (n > LJ_MAX_MEM32 / sizeof(BCCellBits)) bcread_error(ls, LJ_ERR_BCBAD);
  c->active = lj_mem_newvec(L, n, BCCellBits);
  memset(c->active, 0, n * sizeof(BCCellBits));
  c->pending = lj_mem_newvec(L, n, BCCellBits);
  c->possible = lj_mem_newvec(L, n, BCCellBits);
  memset(c->possible, 0, n * sizeof(BCCellBits));
  c->queue = lj_mem_newvec(L, n, uint32_t);
  c->map = lj_mem_newvec(L, n + 1, uint32_t);
  c->seen = lj_mem_newvec(L, n, uint8_t);
  memset(c->seen, 0, n);
  c->queued = lj_mem_newvec(L, n, uint8_t);
  memset(c->queued, 0, n);
  bccell_validate(c);
  bccell_pending(c);
  bccell_analyze(c);
  bccell_lower(c);
  dbgsize = bccell_debug(c);
  wide = sizeof(GCproto) + (uint64_t)c->nout * 2 * sizeof(BCIns) +
         (uint64_t)kgc * sizeof(GCRef);
  wide = (wide + sizeof(TValue) - 1) & ~((uint64_t)sizeof(TValue) - 1);
  ofsk = (MSize)wide;
  wide += (uint64_t)pt->sizekn * sizeof(TValue);
  ofsuv = (MSize)wide;
  wide += (uint64_t)((pt->sizeuv + 1u) & ~1u) * 2;
  ofsdbg = (MSize)wide;
  wide += dbgsize;
  if (wide > LJ_MAX_MEM32) bcread_error(ls, LJ_ERR_BCBAD);
  sizept = (MSize)wide;
  np = (GCproto *)lj_mem_newgco_unlinked(L, sizept);
  memcpy(np, pt, sizeof(GCproto));
  np->sizept = sizept;
  np->sizebc = c->nout;
  np->framesize = pt->framesize + 3;
  proto_setcellops(np);
  for (i = 0; i < c->nout; i++) proto_bc(np)[i] = c->out[i].ins;
  setbc_a(proto_bc(np), np->framesize);
  setmref(np->jit_startins, proto_bc(np) + c->nout);
  memset(proto_jit_startins(np), 0, c->nout * sizeof(BCIns));
  setmref(np->k, (char *)np + ofsk);
  setmref(np->uv, (char *)np + ofsuv);
  *(uint32_t *)((char *)np + ofsk - sizeof(GCRef) * (kgc + 1)) = 0;
  memcpy(mref(np->k, GCRef) - kgc, mref(pt->k, GCRef) - kgc,
         kgc * sizeof(GCRef));
  memcpy(mref(np->k, TValue), mref(pt->k, TValue), pt->sizekn * sizeof(TValue));
  memcpy(proto_uv(np), proto_uv(pt), pt->sizeuv * 2);
  if (dbgsize) {
    MSize shift = pt->numline < 256 ? 0 : pt->numline < 65536 ? 1 : 2;
    MSize uvlen = (MSize)(proto_varinfo(pt) - proto_uvinfo(pt));
    memcpy((char *)np + ofsdbg, c->dbg.b, dbgsize);
    setmref(np->lineinfo, (char *)np + ofsdbg);
    setmref(np->uvinfo, (char *)np + ofsdbg + ((c->nout - 1) << shift));
    setmref(np->varinfo, proto_uvinfo(np) + uvlen);
  } else {
    setmref(np->lineinfo, NULL);
    setmref(np->uvinfo, NULL);
    setmref(np->varinfo, NULL);
  }
  {
    TValue value;
    setprotoV(L, &value, np);
    copyTVrel(L, anchor, &value);
  }
  *pending = np;
  *pendingsz = sizept;
  /* Keep old allocation alive until work arrays have used its original size. */
  bccell_free(c);
  *workp = NULL;
  lj_mem_freegco_unpublished(G(L), pt, pt->sizept);
  return np;
}

typedef struct BCReadProtoCtx {
  LexState *ls;
  GCproto *pt;
  MSize sizept;
  uint32_t anchoridx;
  int published;
  BCCellCtx *cell;
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
	     (uint64_t)sizebc * 2u * sizeof(BCIns) +
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
  setmref(pt->jit_startins, proto_bc(pt) + sizebc);
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
  memset(proto_jit_startins(pt), 0, sizebc * sizeof(BCIns));
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
  pt = bccell_normalize(ls, pt, anchor, &ctx->cell, &ctx->pt, &ctx->sizept);
  bccell_free(ctx->cell);
  ctx->cell = NULL;
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
  ctx.cell = NULL;
  errcode = lj_vm_cpcall(L, NULL, &ctx, bcread_proto_cp);
  if (LJ_UNLIKELY(errcode)) {
    /* A malformed/truncated body must not strand an opaque READY=0 allocation,
    ** which sweep deliberately pins. The body was never discoverable, so this
    ** protected owner can cancel it directly before dropping child anchors. */
    bccell_free(ctx.cell);
    if (ctx.pt != NULL && !ctx.published)
      lj_mem_freegco_unpublished(G(L), ctx.pt, ctx.sizept);
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
