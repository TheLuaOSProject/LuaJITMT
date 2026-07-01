/*
** Metamethod handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_meta_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_lib.h"

/* -- Metamethod handling ------------------------------------------------- */

/* String interning of metamethod names for fast indexing. */
void lj_meta_init(lua_State *L)
{
#define MMNAME(name)	"__" #name
  const char *metanames = MMDEF(MMNAME);
#undef MMNAME
  global_State *g = G(L);
  const char *p, *q;
  uint32_t mm;
  for (mm = 0, p = metanames; *p; mm++, p = q) {
    GCstr *s;
    for (q = p+2; *q && *q != '_'; q++) ;
    s = lj_str_new(L, p, (size_t)(q-p));
    /* NOBARRIER: g->gcroot[] is a GC root. */
    lj_gcroot_rel(g, (GCRootID)(GCROOT_MMNAME+mm), obj2gco(s));
  }
}

/* Negative caching of a few fast metamethods. See the lj_meta_fast() macro. */
cTValue *lj_meta_cache(GCtab *mt, MMS mm, GCstr *name)
{
  cTValue *mo = lj_tab_getstr(mt, name);
  lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
  if (!mo || tvisnil(mo)) {  /* No metamethod? */
    return NULL;
  }
  return mo;
}

cTValue *lj_meta_cachetv(GCtab *mt, MMS mm, GCstr *name, TValue *out)
{
  cTValue *mo = lj_tab_getstr(mt, name);
  lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
  if (!mo)
    return NULL;
  lj_tv_load_acq(out, mo);
  return tvisnil(out) ? NULL : out;
}

/* Lookup metamethod for object. */
cTValue *lj_meta_lookup(lua_State *L, cTValue *o, MMS mm)
{
  GCtab *mt;
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt) {
    cTValue *mo = lj_tab_getstr(mt, mmname_str(G(L), mm));
    if (mo)
      return mo;
  }
  return niltv(L);
}

cTValue *lj_meta_lookuptv(lua_State *L, TValue *out, cTValue *o, MMS mm)
{
  GCtab *mt;
  if (tvistab(o))
    mt = lj_tab_metatable_acq(tabV(o));
  else if (tvisudata(o))
    mt = lj_udata_metatable_acq(udataV(o));
  else
    mt = lj_basemt_obj_acq(G(L), o);
  if (mt) {
    cTValue *mo = lj_tab_getstr(mt, mmname_str(G(L), mm));
    if (mo) {
      lj_tv_load_acq(out, mo);
      return out;
    }
  }
  setnilV(out);
  return out;
}

#if LJ_HASFFI
/* Tailcall from C function. */
int lj_meta_tailcall(lua_State *L, cTValue *tv)
{
  TValue *base = L->base;
  TValue *top = L->top;
  const BCIns *pc = frame_pc(base-1);  /* Preserve old PC from frame. */
  copyTV(L, base-1-LJ_FR2, tv);  /* Replace frame with new object. */
  if (LJ_FR2)
    (top++)->u64 = LJ_CONT_TAILCALL;
  else
    top->u32.lo = LJ_CONT_TAILCALL;
  setframe_pc(top++, pc);
  setframe_gc(top, obj2gco(L), LJ_TTHREAD);  /* Dummy frame object. */
  if (LJ_FR2) top++;
  setframe_ftsz(top, ((char *)(top+1) - (char *)base) + FRAME_CONT);
  L->base = L->top = top+1;
  /*
  ** before:   [old_mo|PC]    [... ...]
  **                         ^base     ^top
  ** after:    [new_mo|itype] [... ...] [NULL|PC] [dummy|delta]
  **                                                           ^base/top
  ** tailcall: [new_mo|PC]    [... ...]
  **                         ^base     ^top
  */
  return 0;
}
#endif

/* Setup call to metamethod to be run by Assembler VM. */
static TValue *mmcall(lua_State *L, ASMFunction cont, cTValue *mo,
		    cTValue *a, cTValue *b)
{
  /*
  **           |-- framesize -> top       top+1       top+2 top+3
  ** before:   [func slots ...]
  ** mm setup: [func slots ...] [cont|?]  [mo|tmtype] [a]   [b]
  ** in asm:   [func slots ...] [cont|PC] [mo|delta]  [a]   [b]
  **           ^-- func base                          ^-- mm base
  ** after mm: [func slots ...]           [result]
  **                ^-- copy to base[PC_RA] --/     for lj_cont_ra
  **                          istruecond + branch   for lj_cont_cond*
  **                                       ignore   for lj_cont_nop
  ** next PC:  [func slots ...]
  */
  TValue *top = L->top;
  if (curr_funcisL(L)) top = curr_topL(L);
  setcont(top++, cont);  /* Assembler VM stores PC in upper word or FR2. */
  if (LJ_FR2) setnilV(top++);
  copyTV(L, top++, mo);  /* Store metamethod and two arguments. */
  if (LJ_FR2) setnilV(top++);
  copyTV(L, top, a);
  copyTV(L, top+1, b);
  return top;  /* Return new base. */
}

/* -- C helpers for some instructions, called from assembler VM ----------- */

/* Helper for TGET*. __index chain and metamethod. */
cTValue *lj_meta_tget(lua_State *L, cTValue *o, cTValue *k)
{
  TValue motv;
  int loop;
  for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
    cTValue *mo;
    if (LJ_LIKELY(tvistab(o))) {
      GCtab *t = tabV(o);
      cTValue *tv = lj_tab_get(L, t, k);
      if (!tvisnil(tv) ||
	  !(mo = lj_meta_fasttv(G(L), lj_tab_metatable_acq(t),
				MM_index, &motv)))
	return tv;
    } else if (tvisnil(mo = lj_meta_lookuptv(L, &motv, o, MM_index))) {
      lj_err_optype(L, o, LJ_ERR_OPINDEX);
      return NULL;  /* unreachable */
    }
    if (tvisfunc(mo)) {
      L->top = mmcall(L, lj_cont_ra, mo, o, k);
      return NULL;  /* Trigger metamethod call. */
    }
    o = mo;
  }
  lj_err_msg(L, LJ_ERR_GETLOOP);
  return NULL;  /* unreachable */
}

static TValue *meta_tset(lua_State *L, cTValue *o, cTValue *k, GCtab **owner)
{
  TValue tmp, motv;
  int loop;
  if (owner)
    *owner = NULL;
  for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
    cTValue *mo;
    if (LJ_LIKELY(tvistab(o))) {
      GCtab *t = tabV(o);
      cTValue *tv = lj_tab_get(L, t, k);
      if (LJ_LIKELY(!tvisnil(tv))) {
	lj_tab_nomm_rel(t, 0);  /* Invalidate negative metamethod cache. */
	lj_gc2_barrier_weak_key(L, t, k);
	lj_gc_pubtab(L, t);
	if (owner)
	  *owner = t;
	return (TValue *)tv;
      } else if (!(mo = lj_meta_fasttv(G(L), lj_tab_metatable_acq(t),
				       MM_newindex, &motv))) {
	lj_tab_nomm_rel(t, 0);  /* Invalidate negative metamethod cache. */
	if (tv != niltv(L))
	  lj_gc2_barrier_weak_key(L, t, k);
	lj_gc_pubtab(L, t);
	if (tv != niltv(L)) {
	  if (owner)
	    *owner = t;
	  return (TValue *)tv;
	}
	if (tvisnil(k)) lj_err_msg(L, LJ_ERR_NILIDX);
	else if (tvisint(k)) { setnumV(&tmp, (lua_Number)intV(k)); k = &tmp; }
	else if (tvisnum(k) && tvisnan(k)) lj_err_msg(L, LJ_ERR_NANIDX);
	if (owner)
	  *owner = t;
	return lj_tab_newkey(L, t, k);
      }
    } else if (tvisnil(mo = lj_meta_lookuptv(L, &motv, o, MM_newindex))) {
      lj_err_optype(L, o, LJ_ERR_OPINDEX);
      return NULL;  /* unreachable */
    }
    if (tvisfunc(mo)) {
      L->top = mmcall(L, lj_cont_nop, mo, o, k);
      /* L->top+2 = v filled in by caller. */
      return NULL;  /* Trigger metamethod call. */
    }
    copyTV(L, &tmp, mo);
    o = &tmp;
  }
  lj_err_msg(L, LJ_ERR_SETLOOP);
  return NULL;  /* unreachable */
}

/* Helper for TSET*. __newindex chain and metamethod. */
TValue *lj_meta_tset(lua_State *L, cTValue *o, cTValue *k)
{
  return meta_tset(L, o, k, NULL);
}

/* C API variant that reports the resolved target table for post-store barriers. */
TValue *lj_meta_tset_owner(lua_State *L, cTValue *o, cTValue *k, GCtab **owner)
{
  return meta_tset(L, o, k, owner);
}

/* VM helper that resolves the target table, stores the value and barriers it. */
TValue *lj_meta_tsettv_pair(lua_State *L, cTValue *o, cTValue *k, cTValue *v)
{
  for (;;) {
    GCtab *owner = NULL;
    TValue *dst = meta_tset(L, o, k, &owner);
    int weakwr;
    int rc;
    if (!dst)
      return NULL;
    weakwr = lj_gc2_weak_write_begin(L, owner);
    if (weakwr) {
      lj_gc2_barrier_weak_key(L, owner, k);
      lj_gc2_barrier_weak_value(L, owner, v);
    }
    rc = lj_tab_trystoretv_cas_keyed(L, owner, dst, k, v);
    if (weakwr) {
      lj_gc2_barrier_weak_key(L, owner, k);
      lj_gc2_barrier_weak_value(L, owner, v);
      lj_gc2_barrier_tv_pair(L, owner ? obj2gco(owner) : NULL, v);
      lj_gc2_weak_write_end(L, weakwr);
    }
    if (rc == LJ_TAB_STORE_CAS_OK) {
      if (!weakwr) {
	lj_gc2_barrier_weak_value(L, owner, v);
	lj_gc2_barrier_tv_pair(L, owner ? obj2gco(owner) : NULL, v);
      }
      return dst;
    }
    lj_tab_store_wait_l(L);  /* Slot became stale/FORWARD; re-resolve. */
  }
}

static cTValue *str2num(cTValue *o, TValue *n)
{
  if (tvisnum(o))
    return o;
  else if (tvisint(o))
    return (setnumV(n, (lua_Number)intV(o)), n);
  else if (tvisstr(o) && lj_strscan_num(strV(o), n))
    return n;
  else
    return NULL;
}

/* Helper for arithmetic instructions. Coercion, metamethod. */
TValue *lj_meta_arith(lua_State *L, TValue *ra, cTValue *rb, cTValue *rc,
		      BCReg op)
{
  MMS mm = bcmode_mm(op);
  TValue tempb, tempc, motv;
  cTValue *b, *c;
  if ((b = str2num(rb, &tempb)) != NULL &&
      (c = str2num(rc, &tempc)) != NULL) {  /* Try coercion first. */
    setnumV(ra, lj_vm_foldarith(numV(b), numV(c), (int)mm-MM_add));
    return NULL;
  } else {
    cTValue *mo = lj_meta_lookuptv(L, &motv, rb, mm);
    if (tvisnil(mo)) {
      mo = lj_meta_lookuptv(L, &motv, rc, mm);
      if (tvisnil(mo)) {
	if (str2num(rb, &tempb) == NULL) rc = rb;
	lj_err_optype(L, rc, LJ_ERR_OPARITH);
	return NULL;  /* unreachable */
      }
    }
    return mmcall(L, lj_cont_ra, mo, rb, rc);
  }
}

/* Helper for CAT. Coercion, iterative concat, __concat metamethod. */
TValue *lj_meta_cat(lua_State *L, TValue *top, int left)
{
  TValue motv;
  int fromc = 0;
  if (left < 0) { left = -left; fromc = 1; }
  do {
    if (!(tvisstr(top) || tvisnumber(top) || tvisbuf(top)) ||
	!(tvisstr(top-1) || tvisnumber(top-1) || tvisbuf(top-1))) {
      cTValue *mo = lj_meta_lookuptv(L, &motv, top-1, MM_concat);
      if (tvisnil(mo)) {
	mo = lj_meta_lookuptv(L, &motv, top, MM_concat);
	if (tvisnil(mo)) {
	  if (tvisstr(top-1) || tvisnumber(top-1)) top++;
	  lj_err_optype(L, top-1, LJ_ERR_OPCAT);
	  return NULL;  /* unreachable */
	}
      }
      /* One of the top two elements is not a string, call __cat metamethod:
      **
      ** before:    [...][CAT stack .........................]
      **                                 top-1     top         top+1 top+2
      ** pick two:  [...][CAT stack ...] [o1]      [o2]
      ** setup mm:  [...][CAT stack ...] [cont|?]  [mo|tmtype] [o1]  [o2]
      ** in asm:    [...][CAT stack ...] [cont|PC] [mo|delta]  [o1]  [o2]
      **            ^-- func base                              ^-- mm base
      ** after mm:  [...][CAT stack ...] <--push-- [result]
      ** next step: [...][CAT stack .............]
      */
      copyTV(L, top+2*LJ_FR2+2, top);  /* Carefully ordered stack copies! */
      copyTV(L, top+2*LJ_FR2+1, top-1);
      copyTV(L, top+LJ_FR2, mo);
      setcont(top-1, lj_cont_cat);
      if (LJ_FR2) { setnilV(top); setnilV(top+2); top += 2; }
      return top+1;  /* Trigger metamethod call. */
    } else {
      /* Pick as many strings as possible from the top and concatenate them:
      **
      ** before:    [...][CAT stack ...........................]
      ** pick str:  [...][CAT stack ...] [...... strings ......]
      ** concat:    [...][CAT stack ...] [result]
      ** next step: [...][CAT stack ............]
      */
      TValue *e, *o = top;
      uint64_t tlen = tvisstr(o) ? strV(o)->len :
		      tvisbuf(o) ? sbufxlen(bufV(o)) : STRFMT_MAXBUF_NUM;
      SBuf *sb;
      do {
	o--; tlen += tvisstr(o) ? strV(o)->len :
		     tvisbuf(o) ? sbufxlen(bufV(o)) : STRFMT_MAXBUF_NUM;
      } while (--left > 0 && (tvisstr(o-1) || tvisnumber(o-1)));
      if (tlen >= LJ_MAX_STR) lj_err_msg(L, LJ_ERR_STROV);
      sb = lj_buf_tmp_(L);
      lj_buf_more(sb, (MSize)tlen);
      for (e = top, top = o; o <= e; o++) {
	if (tvisstr(o)) {
	  GCstr *s = strV(o);
	  MSize len = s->len;
	  lj_buf_putmem(sb, strdata(s), len);
	} else if (tvisbuf(o)) {
	  SBufExt *sbx = bufV(o);
	  MSize len;
	  const char *p = lj_bufx_data_acq(sbx, &len);
	  lj_buf_putmem(sb, p, len);
	} else if (tvisint(o)) {
	  lj_strfmt_putint(sb, intV(o));
	} else {
	  lj_strfmt_putfnum(sb, STRFMT_G14, numV(o));
	}
      }
      setstrV(L, top, lj_buf_str(L, sb));
    }
  } while (left >= 1);
  if (LJ_UNLIKELY(lj_gc_should_step(G(L)))) {
    if (!fromc) L->top = curr_topL(L);
    lj_gc_step_top(L);
  }
  return NULL;
}

/* Helper for LEN. __len metamethod. */
TValue * LJ_FASTCALL lj_meta_len(lua_State *L, cTValue *o)
{
  TValue motv;
  cTValue *mo = lj_meta_lookuptv(L, &motv, o, MM_len);
  if (tvisnil(mo)) {
    if (!(LJ_52 && tvistab(o)))
      lj_err_optype(L, o, LJ_ERR_OPLEN);
    return NULL;
  }
  return mmcall(L, lj_cont_ra, mo, o, LJ_52 ? o : niltv(L));
}

/* Helper for equality comparisons. __eq metamethod. */
TValue *lj_meta_equal(lua_State *L, GCobj *o1, GCobj *o2, int ne)
{
  /* Field metatable must be at same offset for GCtab and GCudata! */
  TValue motv, motv2;
  GCtab *mt1 = lj_obj_metatable_acq(o1);
  GCtab *mt2 = lj_obj_metatable_acq(o2);
  cTValue *mo = lj_meta_fasttv(G(L), mt1, MM_eq, &motv);
  if (mo) {
    TValue *top;
    uint32_t it;
    if (mt1 != mt2) {
      cTValue *mo2 = lj_meta_fasttv(G(L), mt2, MM_eq, &motv2);
      if (mo2 == NULL || !lj_obj_equal(mo, mo2))
	return (TValue *)(intptr_t)ne;
    }
    top = curr_top(L);
    setcont(top++, ne ? lj_cont_condf : lj_cont_condt);
    if (LJ_FR2) setnilV(top++);
    copyTV(L, top++, mo);
    if (LJ_FR2) setnilV(top++);
    it = ~(uint32_t)o1->gch.gct;
    setgcV(L, top, o1, it);
    setgcV(L, top+1, o2, it);
    return top;  /* Trigger metamethod call. */
  }
  return (TValue *)(intptr_t)ne;
}

#if LJ_HASFFI
TValue * LJ_FASTCALL lj_meta_equal_cd(lua_State *L, BCIns ins)
{
  ASMFunction cont = (bc_op(ins) & 1) ? lj_cont_condf : lj_cont_condt;
  int op = (int)bc_op(ins) & ~1;
  TValue tv, motv;
  cTValue *mo, *o2, *o1 = &L->base[bc_a(ins)];
  cTValue *o1mm = o1;
  if (op == BC_ISEQV) {
    o2 = &L->base[bc_d(ins)];
    if (!tviscdata(o1mm)) o1mm = o2;
  } else if (op == BC_ISEQS) {
    setstrV(L, &tv,
	    gco2str(proto_kgc_acq(curr_proto(L), ~(ptrdiff_t)bc_d(ins))));
    o2 = &tv;
  } else if (op == BC_ISEQN) {
    proto_knumtv_load_acq(&tv, curr_proto(L), bc_d(ins));
    o2 = &tv;
  } else {
    lj_assertL(op == BC_ISEQP, "bad bytecode op %d", op);
    setpriV(&tv, ~bc_d(ins));
    o2 = &tv;
  }
  mo = lj_meta_lookuptv(L, &motv, o1mm, MM_eq);
  if (LJ_LIKELY(!tvisnil(mo)))
    return mmcall(L, cont, mo, o1, o2);
  else
    return (TValue *)(intptr_t)(bc_op(ins) & 1);
}
#endif

/* Helper for ordered comparisons. String compare, __lt/__le metamethods. */
TValue *lj_meta_comp(lua_State *L, cTValue *o1, cTValue *o2, int op)
{
  if (LJ_HASFFI && (tviscdata(o1) || tviscdata(o2))) {
    ASMFunction cont = (op & 1) ? lj_cont_condf : lj_cont_condt;
    MMS mm = (op & 2) ? MM_le : MM_lt;
    TValue motv;
    cTValue *mo = lj_meta_lookuptv(L, &motv, tviscdata(o1) ? o1 : o2, mm);
    if (LJ_UNLIKELY(tvisnil(mo))) goto err;
    return mmcall(L, cont, mo, o1, o2);
  } else if (LJ_52 || itype(o1) == itype(o2)) {
    /* Never called with two numbers. */
    if (tvisstr(o1) && tvisstr(o2)) {
      int32_t res = lj_str_cmp(strV(o1), strV(o2));
      return (TValue *)(intptr_t)(((op&2) ? res <= 0 : res < 0) ^ (op&1));
    } else {
    trymt:
      while (1) {
	ASMFunction cont = (op & 1) ? lj_cont_condf : lj_cont_condt;
	MMS mm = (op & 2) ? MM_le : MM_lt;
	TValue motv;
	cTValue *mo = lj_meta_lookuptv(L, &motv, o1, mm);
#if LJ_52
	if (tvisnil(mo) &&
	    tvisnil((mo = lj_meta_lookuptv(L, &motv, o2, mm))))
#else
	TValue motv2;
	cTValue *mo2 = lj_meta_lookuptv(L, &motv2, o2, mm);
	if (tvisnil(mo) || !lj_obj_equal(mo, mo2))
#endif
	{
	  if (op & 2) {  /* MM_le not found: retry with MM_lt. */
	    cTValue *ot = o1; o1 = o2; o2 = ot;  /* Swap operands. */
	    op ^= 3;  /* Use LT and flip condition. */
	    continue;
	  }
	  goto err;
	}
	return mmcall(L, cont, mo, o1, o2);
      }
    }
  } else if (tvisbool(o1) && tvisbool(o2)) {
    goto trymt;
  } else {
  err:
    lj_err_comp(L, o1, o2);
    return NULL;
  }
}

/* Helper for ISTYPE and ISNUM. Implicit coercion or error. */
void lj_meta_istype(lua_State *L, BCReg ra, BCReg tp)
{
  L->top = curr_topL(L);
  ra++; tp--;
  lj_assertL(LJ_DUALNUM || tp != ~LJ_TNUMX, "bad type for ISTYPE");
  if (LJ_DUALNUM && tp == ~LJ_TNUMX) lj_lib_checkint(L, ra);
  else if (tp == ~LJ_TNUMX+1) lj_lib_checknum(L, ra);
  else if (tp == ~LJ_TSTR) lj_lib_checkstr(L, ra);
  else lj_err_argtype(L, ra, lj_obj_itypename[tp]);
}

/* Helper for calls. __call metamethod. */
void lj_meta_call(lua_State *L, TValue *func, TValue *top)
{
  TValue motv;
  cTValue *mo = lj_meta_lookuptv(L, &motv, func, MM_call);
  TValue *p;
  if (!tvisfunc(mo))
    lj_err_optype_call(L, func);
  for (p = top; p > func+2*LJ_FR2; p--) copyTV(L, p, p-1);
  if (LJ_FR2) copyTV(L, func+2, func);
  copyTV(L, func, mo);
}

/* Helper for FORI. Coercion. */
void LJ_FASTCALL lj_meta_for(lua_State *L, TValue *o)
{
  if (!lj_strscan_numberobj(o)) lj_err_msg(L, LJ_ERR_FORINIT);
  if (!lj_strscan_numberobj(o+1)) lj_err_msg(L, LJ_ERR_FORLIM);
  if (!lj_strscan_numberobj(o+2)) lj_err_msg(L, LJ_ERR_FORSTEP);
  if (LJ_DUALNUM) {
    /* Ensure all slots are integers or all slots are numbers. */
    int32_t k[3];
    int nint = 0;
    ptrdiff_t i;
    for (i = 0; i <= 2; i++) {
      if (tvisint(o+i)) {
	k[i] = intV(o+i); nint++;
      } else {
	int64_t i64;
	if (lj_num2int_check(numV(o+i), i64, k[i])) nint++;
      }
    }
    if (nint == 3) {  /* Narrow to integers. */
      setintV(o, k[0]);
      setintV(o+1, k[1]);
      setintV(o+2, k[2]);
    } else if (nint != 0) {  /* Widen to numbers. */
      if (tvisint(o)) setnumV(o, (lua_Number)intV(o));
      if (tvisint(o+1)) setnumV(o+1, (lua_Number)intV(o+1));
      if (tvisint(o+2)) setnumV(o+2, (lua_Number)intV(o+2));
    }
  }
}
