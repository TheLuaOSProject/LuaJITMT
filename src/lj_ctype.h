/*
** C type management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CTYPE_H
#define _LJ_CTYPE_H

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_atomic.h"

#if LJ_HASFFI

/* -- C type definitions -------------------------------------------------- */

/* C type numbers. Highest 4 bits of C type info. ORDER CT. */
enum {
  /* Externally visible types. */
  CT_NUM,		/* Integer or floating-point numbers. */
  CT_STRUCT,		/* Struct or union. */
  CT_PTR,		/* Pointer or reference. */
  CT_ARRAY,		/* Array or complex type. */
  CT_MAYCONVERT = CT_ARRAY,
  CT_VOID,		/* Void type. */
  CT_ENUM,		/* Enumeration. */
  CT_HASSIZE = CT_ENUM,  /* Last type where ct->size holds the actual size. */
  CT_FUNC,		/* Function. */
  CT_TYPEDEF,		/* Typedef. */
  CT_ATTRIB,		/* Miscellaneous attributes. */
  /* Internal element types. */
  CT_FIELD,		/* Struct/union field or function parameter. */
  CT_BITFIELD,		/* Struct/union bitfield. */
  CT_CONSTVAL,		/* Constant value. */
  CT_EXTERN,		/* External reference. */
  CT_KW			/* Keyword. */
};

LJ_STATIC_ASSERT(((int)CT_PTR & (int)CT_ARRAY) == CT_PTR);
LJ_STATIC_ASSERT(((int)CT_STRUCT & (int)CT_ARRAY) == CT_STRUCT);

/*
**  ---------- info ------------
** |type      flags...  A   cid | size   |  sib  | next  | name  |
** +----------------------------+--------+-------+-------+-------+--
** |NUM       BFcvUL..  A       | size   |       | type  |       |
** |STRUCT    ..cvU..V  A       | size   | field | name? | name? |
** |PTR       ..cvR...  A   cid | size   |       | type  |       |
** |ARRAY     VCcv...V  A   cid | size   |       | type  |       |
** |VOID      ..cv....  A       | size   |       | type  |       |
** |ENUM                A   cid | size   | const | name? | name? |
** |FUNC      ....VS.. cc   cid | nargs  | field | name? | name? |
** |TYPEDEF                 cid |        |       | name  | name  |
** |ATTRIB        attrnum   cid | attr   | sib?  | type? |       |
** |FIELD               A   cid | offset | field |       | name? |
** |BITFIELD  B.cvU csz bsz pos | offset | field |       | name? |
** |CONSTVAL    c           cid | value  | const | name  | name  |
** |EXTERN                  cid |        | sib?  | name  | name  |
** |KW                      tok | size   |       | name  | name  |
** +----------------------------+--------+-------+-------+-------+--
**        ^^  ^^--- bits used for C type conversion dispatch
*/

/* C type info flags.     TFFArrrr  */
#define CTF_BOOL	0x08000000u	/* Boolean: NUM, BITFIELD. */
#define CTF_FP		0x04000000u	/* Floating-point: NUM. */
#define CTF_CONST	0x02000000u	/* Const qualifier. */
#define CTF_VOLATILE	0x01000000u	/* Volatile qualifier. */
#define CTF_UNSIGNED	0x00800000u	/* Unsigned: NUM, BITFIELD. */
#define CTF_LONG	0x00400000u	/* Long: NUM. */
#define CTF_VLA		0x00100000u	/* Variable-length: ARRAY, STRUCT. */
#define CTF_REF		0x00800000u	/* Reference: PTR. */
#define CTF_VECTOR	0x08000000u	/* Vector: ARRAY. */
#define CTF_COMPLEX	0x04000000u	/* Complex: ARRAY. */
#define CTF_UNION	0x00800000u	/* Union: STRUCT. */
#define CTF_VARARG	0x00800000u	/* Vararg: FUNC. */
#define CTF_SSEREGPARM	0x00400000u	/* SSE register parameters: FUNC. */

#define CTF_QUAL	(CTF_CONST|CTF_VOLATILE)
#define CTF_ALIGN	(CTMASK_ALIGN<<CTSHIFT_ALIGN)
#define CTF_UCHAR	((char)-1 > 0 ? CTF_UNSIGNED : 0)

/* Flags used in parser.  .F.Ammvf   cp->attr  */
#define CTFP_ALIGNED	0x00000001u	/* cp->attr + ALIGN */
#define CTFP_PACKED	0x00000002u	/* cp->attr */
/*                        ...C...f   cp->fattr */
#define CTFP_CCONV	0x00000001u	/* cp->fattr + CCONV/[SSE]REGPARM */

/* C type info bitfields. */
#define CTMASK_CID	0x0000ffffu	/* Max. 65536 type IDs. */
#define CTMASK_NUM	0xf0000000u	/* Max. 16 type numbers. */
#define CTSHIFT_NUM	28
#define CTMASK_ALIGN	15		/* Max. alignment is 2^15. */
#define CTSHIFT_ALIGN	16
#define CTMASK_ATTRIB	255		/* Max. 256 attributes. */
#define CTSHIFT_ATTRIB	16
#define CTMASK_CCONV	3		/* Max. 4 calling conventions. */
#define CTSHIFT_CCONV	16
#define CTMASK_REGPARM	3		/* Max. 0-3 regparms. */
#define CTSHIFT_REGPARM	18
/* Bitfields only used in parser. */
#define CTMASK_VSIZEP	15		/* Max. vector size is 2^15. */
#define CTSHIFT_VSIZEP	4
#define CTMASK_MSIZEP	255		/* Max. type size (via mode) is 128. */
#define CTSHIFT_MSIZEP	8

/* Info bits for BITFIELD. Max. size of bitfield is 64 bits. */
#define CTBSZ_MAX	32		/* Max. size of bitfield is 32 bit. */
#define CTBSZ_FIELD	127		/* Temp. marker for regular field. */
#define CTMASK_BITPOS	127
#define CTMASK_BITBSZ	127
#define CTMASK_BITCSZ	127
#define CTSHIFT_BITPOS	0
#define CTSHIFT_BITBSZ	8
#define CTSHIFT_BITCSZ	16

#define CTF_INSERT(info, field, val) \
  info = (info & ~(CTMASK_##field<<CTSHIFT_##field)) | \
	  (((CTSize)(val) & CTMASK_##field) << CTSHIFT_##field)

/* Calling conventions. ORDER CC */
enum { CTCC_CDECL, CTCC_THISCALL, CTCC_FASTCALL, CTCC_STDCALL };

/* Attribute numbers. */
enum {
  CTA_NONE,		/* Ignored attribute. Must be zero. */
  CTA_QUAL,		/* Unmerged qualifiers. */
  CTA_ALIGN,		/* Alignment override. */
  CTA_SUBTYPE,		/* Transparent sub-type. */
  CTA_REDIR,		/* Redirected symbol name. */
  CTA_BAD,		/* To catch bad IDs. */
  CTA__MAX
};

/* Special sizes. */
#define CTSIZE_INVALID	0xffffffffu

typedef uint32_t CTInfo;	/* Type info. */
typedef uint32_t CTSize;	/* Type size. */
typedef uint32_t CTypeID;	/* Type ID. */
typedef uint16_t CTypeID1;	/* Minimum-sized type ID. */

/* C type table element. */
typedef struct CType {
  CTInfo info;		/* Type info. */
  CTSize size;		/* Type size or other info. */
  CTypeID1 sib;		/* Sibling element. */
  CTypeID1 next;	/* Next element in hash chain. */
  GCRef name;		/* Element name (GCstr). */
} CType;

static LJ_AINLINE CTInfo ctype_info_acq(const CType *ct)
{
  return la_load32_acq(&ct->info);  /* 11.2: ctype record payload. */
}

static LJ_AINLINE void ctype_info_rel(CType *ct, CTInfo info)
{
  la_store32_rel(&ct->info, info);  /* 11.2: ctype record payload. */
}

static LJ_AINLINE CTSize ctype_size_acq(const CType *ct)
{
  return la_load32_acq(&ct->size);  /* 11.2: ctype record payload. */
}

static LJ_AINLINE void ctype_size_rel(CType *ct, CTSize size)
{
  la_store32_rel(&ct->size, size);  /* 11.2: ctype record payload. */
}

static LJ_AINLINE CTypeID ctype_next_acq(const CType *ct)
{
  return (CTypeID)la_load16_acq(&ct->next);  /* 11.2: ctype hash-chain link. */
}

static LJ_AINLINE void ctype_next_rel(CType *ct, CTypeID next)
{
  la_store16_rel(&ct->next, (CTypeID1)next);  /* 11.2: ctype hash-chain link. */
}

static LJ_AINLINE CTypeID ctype_sib_acq(const CType *ct)
{
  return (CTypeID)la_load16_acq(&ct->sib);  /* 11.2: ctype sibling link. */
}

static LJ_AINLINE void ctype_sib_rel(CType *ct, CTypeID sib)
{
  la_store16_rel(&ct->sib, (CTypeID1)sib);  /* 11.2: ctype sibling link. */
}

typedef struct CTypeTab {
  MSize sizetab;		/* Number of C type table slots. */
  uint64_t retire_epoch;	/* Safepoint epoch when retired. */
  struct CTypeTab *retired_next;  /* Retired C type tables awaiting SMR. */
  CType tab[1];			/* C type table slots. */
} CTypeTab;

static LJ_AINLINE MSize ctype_tab_sizetab_acq(const CTypeTab *tabh)
{
  return (MSize)la_load32_acq(&tabh->sizetab);
}

static LJ_AINLINE void ctype_tab_sizetab_rel(CTypeTab *tabh, MSize sizetab)
{
  la_store32_rel(&tabh->sizetab, sizetab);
}

static LJ_AINLINE CType *ctype_tab_slot(CTypeTab *tabh, CTypeID id)
{
  return &tabh->tab[id];
}

static LJ_AINLINE uint64_t ctype_tab_retire_epoch_acq(const CTypeTab *tabh)
{
  return la_load64_acq(&tabh->retire_epoch);
}

static LJ_AINLINE void ctype_tab_retire_epoch_rel(CTypeTab *tabh,
						  uint64_t epoch)
{
  la_store64_rel(&tabh->retire_epoch, epoch);
}

static LJ_AINLINE CTypeTab *ctype_tab_retired_next_acq(const CTypeTab *tabh)
{
  return (CTypeTab *)la_loadptr_acq((void *const *)&tabh->retired_next);
}

static LJ_AINLINE void ctype_tab_retired_next_rel(CTypeTab *tabh,
						  CTypeTab *next)
{
  la_storeptr_rel((void **)&tabh->retired_next, next);
}

typedef struct FinRegGen {
  GCtab *tab;			/* One hash-only FINREG table generation. */
  struct FinRegGen *next;	/* Older generation, searched after this one. */
} FinRegGen;

typedef struct FinRegOrderNode {
  GCRef obj;			/* Cdata object for this registration. */
  GCtab *tab;			/* FINREG generation containing this slot. */
  TValue *slot;			/* FINREG value slot for this registration. */
  struct FinRegOrderNode *next;	/* Older registration, newest-first list. */
  struct FinRegOrderNode *retired_next;
  uint32_t active;		/* 1 active, 2 retiring, 0 retired. */
} FinRegOrderNode;

static LJ_AINLINE GCtab *fin_gen_tab_acq(const FinRegGen *gen)
{
  return (GCtab *)la_loadptr_acq((void *const *)&gen->tab);
}

static LJ_AINLINE void fin_gen_tab_rel(FinRegGen *gen, GCtab *tab)
{
  la_storeptr_rel((void **)&gen->tab, tab);
}

static LJ_AINLINE void fin_gen_tab_enable_rel(GCtab *tab)
{
  setgcrefmt(tab->metatable, obj2gco(tab));
}

static LJ_AINLINE int fin_gen_tab_enabled_acq(const GCtab *tab)
{
  return gcref_acq(tab->metatable) != NULL;
}

static LJ_AINLINE void fin_gen_tab_disable_rel(GCtab *tab)
{
  setgcrefnullrel(tab->metatable);
}

static LJ_AINLINE FinRegGen *fin_gen_next_acq(const FinRegGen *gen)
{
  return (FinRegGen *)la_loadptr_acq((void *const *)&gen->next);
}

static LJ_AINLINE void fin_gen_next_rel(FinRegGen *gen, FinRegGen *next)
{
  la_storeptr_rel((void **)&gen->next, next);
}

static LJ_AINLINE FinRegOrderNode *
fin_order_next_acq(const FinRegOrderNode *ord)
{
  return (FinRegOrderNode *)la_loadptr_acq((void *const *)&ord->next);
}

static LJ_AINLINE void fin_order_next_rel(FinRegOrderNode *ord,
					  FinRegOrderNode *next)
{
  la_storeptr_rel((void **)&ord->next, next);
}

static LJ_AINLINE int fin_order_next_cas(FinRegOrderNode *ord,
					 FinRegOrderNode **oldp,
					 FinRegOrderNode *next)
{
  return la_casptr((void **)&ord->next, (void **)oldp, next,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE FinRegOrderNode *
fin_order_retired_next_acq(const FinRegOrderNode *ord)
{
  return (FinRegOrderNode *)la_loadptr_acq(
    (void *const *)&ord->retired_next);
}

static LJ_AINLINE void fin_order_retired_next_rel(FinRegOrderNode *ord,
						  FinRegOrderNode *next)
{
  la_storeptr_rel((void **)&ord->retired_next, next);
}

static LJ_AINLINE uint32_t fin_order_active_acq(const FinRegOrderNode *ord)
{
  return la_load32_acq(&ord->active);
}

static LJ_AINLINE void fin_order_active_rel(FinRegOrderNode *ord,
					    uint32_t active)
{
  la_store32_rel(&ord->active, active);
}

static LJ_AINLINE int fin_order_active_retiring(FinRegOrderNode *ord)
{
  uint32_t old = 1;
  return la_cas32(&ord->active, &old, 2, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE GCobj *fin_order_obj_acq(FinRegOrderNode *ord)
{
  return gcref_acq(ord->obj);
}

static LJ_AINLINE void fin_order_obj_rel(FinRegOrderNode *ord, GCobj *o)
{
  setgcrefrel(ord->obj, o);
}

static LJ_AINLINE void fin_order_obj_clear(FinRegOrderNode *ord)
{
  setgcrefnullrel(ord->obj);
}

static LJ_AINLINE GCtab *fin_order_tab_acq(const FinRegOrderNode *ord)
{
  return (GCtab *)la_loadptr_acq((void *const *)&ord->tab);
}

static LJ_AINLINE void fin_order_tab_rel(FinRegOrderNode *ord, GCtab *tab)
{
  la_storeptr_rel((void **)&ord->tab, tab);
}

static LJ_AINLINE TValue *fin_order_slot_acq(const FinRegOrderNode *ord)
{
  return (TValue *)la_loadptr_acq((void *const *)&ord->slot);
}

static LJ_AINLINE void fin_order_slot_rel(FinRegOrderNode *ord, TValue *slot)
{
  la_storeptr_rel((void **)&ord->slot, slot);
}

#define CTHASH_SIZE	128	/* Number of hash anchors. */
#define CTHASH_MASK	(CTHASH_SIZE-1)

/* Simplify target-specific configuration. Checked in lj_ccall.h. */
#define CCALL_MAX_GPR		8
#define CCALL_MAX_FPR		8

typedef LJ_ALIGN(8) union FPRCBArg { double d; float f[2]; } FPRCBArg;

/* C callback state. Defined here, to avoid dragging in lj_ccall.h. */

#define CCALLBACK_MAX_NEST	LJ_MAX_XLEVEL

typedef struct CCallbackFrame {
  lua_State *L;			/* Callback carrier Lua state. */
  TValue *cont;			/* Continuation frame owning this entry. */
  uint8_t was_native;		/* Callback entered from a native region. */
  uint8_t auto_detach;		/* Scoped foreign-thread auto-attach. */
} CCallbackFrame;

typedef LJ_ALIGN(8) struct CCallbackRuntime {
  FPRCBArg fpr[CCALL_MAX_FPR];	/* Arguments/results in FPRs. */
  intptr_t gpr[CCALL_MAX_GPR];	/* Arguments/results in GPRs. */
  intptr_t *stack;		/* Pointer to arguments on stack. */
  lua_State *L;			/* Current callback carrier from the trampoline. */
  MSize slot;			/* Current callback slot. */
  MSize depth;			/* Active callback frames on this TG. */
  uint8_t auto_detach;		/* Pending scoped attach before frame push. */
  uint8_t native_had_stopreq;	/* Sticky STOPREQ before surrounding FFI call. */
  CCallbackFrame frame[CCALLBACK_MAX_NEST];  /* Per-callback return state. */
} CCallbackRuntime;

static LJ_AINLINE uint8_t
ccallback_native_had_stopreq_acq(const CCallbackRuntime *cb)
{
  /* 11.5 native callback STOPREQ snapshot. */
  return cb ? la_load8_acq(&cb->native_had_stopreq) : 0;
}

static LJ_AINLINE void
ccallback_native_had_stopreq_rel(CCallbackRuntime *cb, uint8_t had_stopreq)
{
  /* 11.5 native callback STOPREQ snapshot. */
  la_store8_rel(&cb->native_had_stopreq, had_stopreq);
}

static LJ_AINLINE uint8_t
ccallback_auto_detach_acq(const CCallbackRuntime *cb)
{
  /* 11.5 foreign callback carrier auto-detach handoff. */
  return cb ? la_load8_acq(&cb->auto_detach) : 0;
}

static LJ_AINLINE void
ccallback_auto_detach_rel(CCallbackRuntime *cb, uint8_t auto_detach)
{
  /* 11.5 foreign callback carrier auto-detach handoff. */
  la_store8_rel(&cb->auto_detach, auto_detach);
}

static LJ_AINLINE lua_State *ccallback_L_acq(const CCallbackRuntime *cb)
{
  /* 11.5 callback carrier Lua state handoff. */
  return cb ? (lua_State *)la_loadptr_acq((void *const *)&cb->L) : NULL;
}

static LJ_AINLINE void ccallback_L_rel(CCallbackRuntime *cb, lua_State *L)
{
  /* 11.5 callback carrier Lua state handoff. */
  la_storeptr_rel((void **)&cb->L, (void *)L);
}

static LJ_AINLINE MSize ccallback_depth_acq(const CCallbackRuntime *cb)
{
  /* 11.5 callback runtime depth observation for tests/diagnostics. */
  return cb ? (MSize)la_load32_acq(&cb->depth) : 0;
}

static LJ_AINLINE void ccallback_depth_rel(CCallbackRuntime *cb, MSize depth)
{
  /* 11.5 callback frame metadata publication. */
  la_store32_rel(&cb->depth, depth);
}

static LJ_AINLINE MSize ccallback_slot_acq(const CCallbackRuntime *cb)
{
  /* 11.5 native FFI/callback handoff slot marker. */
  return cb ? (MSize)la_load32_acq(&cb->slot) : 0;
}

static LJ_AINLINE void ccallback_slot_rel(CCallbackRuntime *cb, MSize slot)
{
  /* 11.5 native FFI/callback handoff slot marker. */
  la_store32_rel(&cb->slot, slot);
}

typedef LJ_ALIGN(8) struct CCallback {
  void *mcode;			/* Machine code for callback func. pointers. */
  CTypeID1 *cbid;		/* Callback type table. */
  lua_State **owner;		/* Callback slot owner Lua states. */
  TValue *func;			/* Callback function slots. */
  MSize sizeid;			/* Size of callback type table. */
} CCallback;

/* C type state. */
typedef struct CTState {
  CTypeTab *tabh;	/* RCU-published C type table header. */
  CTypeTab *retiredtab;  /* Retired C type tables awaiting SMR. */
  CTypeID top;		/* Current top of C type table. */
  global_State *g;	/* Global state. */
  GCtab *miscmap;	/* FFI function metatable/root table. */
  GCRef *metamap;	/* CAS-installed metatables by raw CTypeID. */
  MSize sizemeta;	/* Size of metatable side map. */
  uint64_t *cbblack;	/* Callback-calling C function blacklist set. */
  MSize sizecbblack;	/* Size of callback blacklist set. */
  uint32_t cbblack_all;	/* Conservative blacklist overflow flag. */
  CCallback cb;		/* Temporary callback state. */
  GCtab *pinmt;		/* ffi.pin() handle metatable/root. */
  uint32_t parse_token;	/* 11.2 cparse mutation sequence: even free, odd held. */
  FinRegGen *fin_head;	/* 11.4 CAS-published FINREG generation list. */
  FinRegOrderNode *fin_order_head;  /* 11.4 ordered FINREG registrations. */
  FinRegOrderNode *fin_order_retired;  /* Retired ordered FINREG nodes. */
  uint32_t hash[CTHASH_SIZE];  /* Hash anchors. Low 16 bits hold CTypeID. */
} CTState;

static LJ_AINLINE uint32_t ctype_parse_token_acq(const CTState *cts)
{
  return la_load32_acq(&cts->parse_token);
}

static LJ_AINLINE void ctype_parse_token_rel(CTState *cts, uint32_t seq)
{
  la_store32_rel(&cts->parse_token, seq);
}

static LJ_AINLINE int ctype_parse_token_cas(CTState *cts, uint32_t *oldp,
					    uint32_t seq)
{
  return la_cas32(&cts->parse_token, oldp, seq, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE int ctype_parse_token_wait(CTState *cts, uint32_t seq,
					     int64_t ns)
{
  return la_futex_wait(&cts->parse_token, seq, ns);
}

static LJ_AINLINE int ctype_parse_token_wake(CTState *cts, int n)
{
  return la_futex_wake(&cts->parse_token, n);
}

static LJ_AINLINE FinRegGen *fin_gen_head_acq(const CTState *cts)
{
  return (FinRegGen *)la_loadptr_acq((void *const *)&cts->fin_head);
}

static LJ_AINLINE void fin_gen_head_rel(CTState *cts, FinRegGen *gen)
{
  la_storeptr_rel((void **)&cts->fin_head, gen);
}

static LJ_AINLINE int fin_gen_head_cas(CTState *cts, FinRegGen **oldp,
				       FinRegGen *gen)
{
  return la_casptr((void **)&cts->fin_head, (void **)oldp, gen,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE FinRegGen *fin_gen_head_xchg_acqrel(CTState *cts,
						      FinRegGen *gen)
{
  return (FinRegGen *)la_xchgptr_acqrel((void **)&cts->fin_head, gen);
}

static LJ_AINLINE FinRegOrderNode *fin_order_head_acq(const CTState *cts)
{
  return (FinRegOrderNode *)la_loadptr_acq(
    (void *const *)&cts->fin_order_head);
}

static LJ_AINLINE int fin_order_head_cas(CTState *cts,
					 FinRegOrderNode **oldp,
					 FinRegOrderNode *ord)
{
  return la_casptr((void **)&cts->fin_order_head, (void **)oldp, ord,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE FinRegOrderNode *
fin_order_head_xchg_acqrel(CTState *cts, FinRegOrderNode *ord)
{
  return (FinRegOrderNode *)la_xchgptr_acqrel(
    (void **)&cts->fin_order_head, ord);
}

static LJ_AINLINE FinRegOrderNode *fin_order_retired_acq(const CTState *cts)
{
  return (FinRegOrderNode *)la_loadptr_acq(
    (void *const *)&cts->fin_order_retired);
}

static LJ_AINLINE int fin_order_retired_cas(CTState *cts,
					    FinRegOrderNode **oldp,
					    FinRegOrderNode *ord)
{
  return la_casptr((void **)&cts->fin_order_retired, (void **)oldp, ord,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE FinRegOrderNode *
fin_order_retired_xchg_acqrel(CTState *cts, FinRegOrderNode *ord)
{
  return (FinRegOrderNode *)la_xchgptr_acqrel(
    (void **)&cts->fin_order_retired, ord);
}

static LJ_AINLINE GCtab *ctype_miscmap_acq(const CTState *cts)
{
  return (GCtab *)la_loadptr_acq((void *const *)&cts->miscmap);
}

static LJ_AINLINE void ctype_miscmap_rel(CTState *cts, GCtab *miscmap)
{
  la_storeptr_rel((void **)&cts->miscmap, miscmap);
}

static LJ_AINLINE GCtab *ctype_pinmt_acq(const CTState *cts)
{
  return (GCtab *)la_loadptr_acq((void *const *)&cts->pinmt);
}

static LJ_AINLINE void ctype_pinmt_rel(CTState *cts, GCtab *pinmt)
{
  la_storeptr_rel((void **)&cts->pinmt, pinmt);
}

static LJ_AINLINE GCRef *ctype_metamap_acq(const CTState *cts)
{
  return (GCRef *)la_loadptr_acq((void *const *)&cts->metamap);
}

static LJ_AINLINE void ctype_metamap_rel(CTState *cts, GCRef *meta)
{
  la_storeptr_rel((void **)&cts->metamap, meta);
}

static LJ_AINLINE MSize ctype_metamap_size_acq(const CTState *cts)
{
  return (MSize)la_load32_acq(&cts->sizemeta);
}

static LJ_AINLINE void ctype_metamap_size_rel(CTState *cts, MSize size)
{
  la_store32_rel(&cts->sizemeta, size);
}

static LJ_AINLINE GCobj *ctype_metamap_obj_acq(const GCRef *meta, MSize id)
{
  return gcref_acq(meta[id]);
}

static LJ_AINLINE int ctype_metamap_obj_cas(GCRef *meta, MSize id, GCtab *mt)
{
#if LJ_GC64
  uint64_t expect = 0;
  return la_cas64(&meta[id].gcptr64, &expect,
		  (uint64_t)(uintptr_t)obj2gco(mt), LA_ACQ_REL, LA_ACQ);
#else
  uint32_t expect = 0;
  return la_cas32(&meta[id].gcptr32, &expect,
		  (uint32_t)(uintptr_t)obj2gco(mt), LA_ACQ_REL, LA_ACQ);
#endif
}

static LJ_AINLINE uint64_t *ctype_cbblack_acq(const CTState *cts)
{
  return (uint64_t *)la_loadptr_acq((void *const *)&cts->cbblack);
}

static LJ_AINLINE void ctype_cbblack_rel(CTState *cts, uint64_t *tab)
{
  la_storeptr_rel((void **)&cts->cbblack, tab);
}

static LJ_AINLINE MSize ctype_cbblack_size_acq(const CTState *cts)
{
  return (MSize)la_load32_acq(&cts->sizecbblack);
}

static LJ_AINLINE void ctype_cbblack_size_rel(CTState *cts, MSize size)
{
  la_store32_rel(&cts->sizecbblack, size);
}

static LJ_AINLINE uint32_t ctype_cbblack_all_acq(const CTState *cts)
{
  return la_load32_acq(&cts->cbblack_all);
}

static LJ_AINLINE void ctype_cbblack_all_rel(CTState *cts, uint32_t all)
{
  la_store32_rel(&cts->cbblack_all, all);
}

static LJ_AINLINE uint64_t ctype_cbblack_slot_acq(const uint64_t *tab,
						  MSize slot)
{
  return la_load64_acq(&tab[slot]);
}

static LJ_AINLINE int ctype_cbblack_slot_cas(uint64_t *tab, MSize slot,
					     uint64_t *oldp, uint64_t key)
{
  return la_cas64(&tab[slot], oldp, key, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE void *ctype_cb_mcode_acq(const CTState *cts)
{
  return la_loadptr_acq((void *const *)&cts->cb.mcode);
}

static LJ_AINLINE void ctype_cb_mcode_rel(CTState *cts, void *mcode)
{
  la_storeptr_rel((void **)&cts->cb.mcode, mcode);
}

static LJ_AINLINE CTypeID1 *ctype_cb_cbid_acq(const CTState *cts)
{
  return (CTypeID1 *)la_loadptr_acq((void *const *)&cts->cb.cbid);
}

static LJ_AINLINE void ctype_cb_cbid_rel(CTState *cts, CTypeID1 *cbid)
{
  la_storeptr_rel((void **)&cts->cb.cbid, cbid);
}

static LJ_AINLINE lua_State **ctype_cb_owner_acq(const CTState *cts)
{
  return (lua_State **)la_loadptr_acq((void *const *)&cts->cb.owner);
}

static LJ_AINLINE void ctype_cb_owner_rel(CTState *cts, lua_State **owner)
{
  la_storeptr_rel((void **)&cts->cb.owner, owner);
}

static LJ_AINLINE TValue *ctype_cb_func_acq(const CTState *cts)
{
  return (TValue *)la_loadptr_acq((void *const *)&cts->cb.func);
}

static LJ_AINLINE void ctype_cb_func_rel(CTState *cts, TValue *func)
{
  la_storeptr_rel((void **)&cts->cb.func, func);
}

static LJ_AINLINE MSize ctype_cb_sizeid_acq(const CTState *cts)
{
  return (MSize)la_load32_acq(&cts->cb.sizeid);
}

static LJ_AINLINE void ctype_cb_sizeid_rel(CTState *cts, MSize sizeid)
{
  la_store32_rel(&cts->cb.sizeid, sizeid);
}

static LJ_AINLINE CTypeID1 ctype_cb_cbid_slot_acq(const CTypeID1 *cbid,
						  MSize slot)
{
  return (CTypeID1)la_load16_acq(&cbid[slot]);
}

static LJ_AINLINE void ctype_cb_cbid_slot_rel(CTypeID1 *cbid, MSize slot,
					      CTypeID id)
{
  la_store16_rel(&cbid[slot], (CTypeID1)id);
}

static LJ_AINLINE lua_State *ctype_cb_owner_slot_acq(lua_State **owner,
						     MSize slot)
{
  return (lua_State *)la_loadptr_acq((void *const *)&owner[slot]);
}

static LJ_AINLINE void ctype_cb_owner_slot_rel(lua_State **owner, MSize slot,
					       lua_State *L)
{
  la_storeptr_rel((void **)&owner[slot], L);
}

static LJ_AINLINE int ctype_cb_owner_slot_claim(lua_State **owner, MSize slot,
						lua_State *L)
{
  void *expect = NULL;
  return la_casptr((void **)&owner[slot], &expect, L, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE int ctype_cb_owner_slot_clear(lua_State **owner, MSize slot,
						lua_State *L)
{
  void *expect = L;
  return la_casptr((void **)&owner[slot], &expect, NULL, LA_ACQ_REL, LA_ACQ);
}

#define CTINFO(ct, flags)	(((CTInfo)(ct) << CTSHIFT_NUM) + (flags))
#define CTALIGN(al)		((CTSize)(al) << CTSHIFT_ALIGN)
#define CTATTRIB(at)		((CTInfo)(at) << CTSHIFT_ATTRIB)

#define ctype_type(info)	((info) >> CTSHIFT_NUM)
#define ctype_cid(info)		((CTypeID)((info) & CTMASK_CID))
#define ctype_align(info)	(((info) >> CTSHIFT_ALIGN) & CTMASK_ALIGN)
#define ctype_attrib(info)	(((info) >> CTSHIFT_ATTRIB) & CTMASK_ATTRIB)
#define ctype_bitpos(info)	(((info) >> CTSHIFT_BITPOS) & CTMASK_BITPOS)
#define ctype_bitbsz(info)	(((info) >> CTSHIFT_BITBSZ) & CTMASK_BITBSZ)
#define ctype_bitcsz(info)	(((info) >> CTSHIFT_BITCSZ) & CTMASK_BITCSZ)
#define ctype_vsizeP(info)	(((info) >> CTSHIFT_VSIZEP) & CTMASK_VSIZEP)
#define ctype_msizeP(info)	(((info) >> CTSHIFT_MSIZEP) & CTMASK_MSIZEP)
#define ctype_cconv(info)	(((info) >> CTSHIFT_CCONV) & CTMASK_CCONV)

/* Simple type checks. */
#define ctype_isnum(info)	(ctype_type((info)) == CT_NUM)
#define ctype_isvoid(info)	(ctype_type((info)) == CT_VOID)
#define ctype_isptr(info)	(ctype_type((info)) == CT_PTR)
#define ctype_isarray(info)	(ctype_type((info)) == CT_ARRAY)
#define ctype_isstruct(info)	(ctype_type((info)) == CT_STRUCT)
#define ctype_isfunc(info)	(ctype_type((info)) == CT_FUNC)
#define ctype_isenum(info)	(ctype_type((info)) == CT_ENUM)
#define ctype_istypedef(info)	(ctype_type((info)) == CT_TYPEDEF)
#define ctype_isattrib(info)	(ctype_type((info)) == CT_ATTRIB)
#define ctype_isfield(info)	(ctype_type((info)) == CT_FIELD)
#define ctype_isbitfield(info)	(ctype_type((info)) == CT_BITFIELD)
#define ctype_isconstval(info)	(ctype_type((info)) == CT_CONSTVAL)
#define ctype_isextern(info)	(ctype_type((info)) == CT_EXTERN)
#define ctype_hassize(info)	(ctype_type((info)) <= CT_HASSIZE)

/* Combined type and flag checks. */
#define ctype_isinteger(info) \
  (((info) & (CTMASK_NUM|CTF_BOOL|CTF_FP)) == CTINFO(CT_NUM, 0))
#define ctype_isinteger_or_bool(info) \
  (((info) & (CTMASK_NUM|CTF_FP)) == CTINFO(CT_NUM, 0))
#define ctype_isbool(info) \
  (((info) & (CTMASK_NUM|CTF_BOOL)) == CTINFO(CT_NUM, CTF_BOOL))
#define ctype_isfp(info) \
  (((info) & (CTMASK_NUM|CTF_FP)) == CTINFO(CT_NUM, CTF_FP))

#define ctype_ispointer(info) \
  ((ctype_type(info) >> 1) == (CT_PTR >> 1))  /* Pointer or array. */
#define ctype_isref(info) \
  (((info) & (CTMASK_NUM|CTF_REF)) == CTINFO(CT_PTR, CTF_REF))

#define ctype_isrefarray(info) \
  (((info) & (CTMASK_NUM|CTF_VECTOR|CTF_COMPLEX)) == CTINFO(CT_ARRAY, 0))
#define ctype_isvector(info) \
  (((info) & (CTMASK_NUM|CTF_VECTOR)) == CTINFO(CT_ARRAY, CTF_VECTOR))
#define ctype_iscomplex(info) \
  (((info) & (CTMASK_NUM|CTF_COMPLEX)) == CTINFO(CT_ARRAY, CTF_COMPLEX))

#define ctype_isvltype(info) \
  (((info) & ((CTMASK_NUM|CTF_VLA) - (2u<<CTSHIFT_NUM))) == \
   CTINFO(CT_STRUCT, CTF_VLA))  /* VL array or VL struct. */
#define ctype_isvlarray(info) \
  (((info) & (CTMASK_NUM|CTF_VLA)) == CTINFO(CT_ARRAY, CTF_VLA))

#define ctype_isxattrib(info, at) \
  (((info) & (CTMASK_NUM|CTATTRIB(CTMASK_ATTRIB))) == \
   CTINFO(CT_ATTRIB, CTATTRIB(at)))
#define ctype_isabandoned(info)	ctype_isxattrib((info), CTA_BAD)

/* Target-dependent sizes and alignments. */
#if LJ_64
#define CTSIZE_PTR	8
#define CTALIGN_PTR	CTALIGN(3)
#else
#define CTSIZE_PTR	4
#define CTALIGN_PTR	CTALIGN(2)
#endif

#define CTINFO_REF(ref) \
  CTINFO(CT_PTR, (CTF_CONST|CTF_REF|CTALIGN_PTR) + (ref))

#define CT_MEMALIGN	3	/* Alignment guaranteed by memory allocator. */

#ifdef LUA_USE_ASSERT
#define lj_assertCTS(c, ...)	(lj_assertG_(cts->g, (c), __VA_ARGS__))
#else
#define lj_assertCTS(c, ...)	((void)cts)
#endif

/* -- Predefined types ---------------------------------------------------- */

/* Target-dependent types. */
#if LJ_TARGET_PPC
#define CTTYDEFP(_) \
  _(LINT32,		4,	CT_NUM, CTF_LONG|CTALIGN(2))
#else
#define CTTYDEFP(_)
#endif

#define CTF_LONG_IF8		(CTF_LONG * (sizeof(long) == 8))

/* Common types. */
#define CTTYDEF(_) \
  _(NONE,		0,	CT_ATTRIB, CTATTRIB(CTA_BAD)) \
  _(VOID,		-1,	CT_VOID, CTALIGN(0)) \
  _(CVOID,		-1,	CT_VOID, CTF_CONST|CTALIGN(0)) \
  _(BOOL,		1,	CT_NUM, CTF_BOOL|CTF_UNSIGNED|CTALIGN(0)) \
  _(CCHAR,		1,	CT_NUM, CTF_CONST|CTF_UCHAR|CTALIGN(0)) \
  _(INT8,		1,	CT_NUM, CTALIGN(0)) \
  _(UINT8,		1,	CT_NUM, CTF_UNSIGNED|CTALIGN(0)) \
  _(INT16,		2,	CT_NUM, CTALIGN(1)) \
  _(UINT16,		2,	CT_NUM, CTF_UNSIGNED|CTALIGN(1)) \
  _(INT32,		4,	CT_NUM, CTALIGN(2)) \
  _(UINT32,		4,	CT_NUM, CTF_UNSIGNED|CTALIGN(2)) \
  _(INT64,		8,	CT_NUM, CTF_LONG_IF8|CTALIGN(3)) \
  _(UINT64,		8,	CT_NUM, CTF_UNSIGNED|CTF_LONG_IF8|CTALIGN(3)) \
  _(INT128,		16,	CT_NUM, CTALIGN(4)) \
  _(UINT128,		16,	CT_NUM, CTF_UNSIGNED|CTALIGN(4)) \
  _(FLOAT,		4,	CT_NUM, CTF_FP|CTALIGN(2)) \
  _(DOUBLE,		8,	CT_NUM, CTF_FP|CTALIGN(3)) \
  _(COMPLEX_FLOAT,	8,	CT_ARRAY, CTF_COMPLEX|CTALIGN(2)|CTID_FLOAT) \
  _(COMPLEX_DOUBLE,	16,	CT_ARRAY, CTF_COMPLEX|CTALIGN(3)|CTID_DOUBLE) \
  _(P_VOID,	CTSIZE_PTR,	CT_PTR, CTALIGN_PTR|CTID_VOID) \
  _(P_CVOID,	CTSIZE_PTR,	CT_PTR, CTALIGN_PTR|CTID_CVOID) \
  _(P_CCHAR,	CTSIZE_PTR,	CT_PTR, CTALIGN_PTR|CTID_CCHAR) \
  _(P_UINT8,	CTSIZE_PTR,	CT_PTR, CTALIGN_PTR|CTID_UINT8) \
  _(A_CCHAR,		-1,	CT_ARRAY, CTF_CONST|CTALIGN(0)|CTID_CCHAR) \
  _(CTYPEID,		4,	CT_ENUM, CTALIGN(2)|CTID_INT32) \
  CTTYDEFP(_) \
  /* End of type list. */

/* Public predefined type IDs. */
enum {
#define CTTYIDDEF(id, sz, ct, info)	CTID_##id,
CTTYDEF(CTTYIDDEF)
#undef CTTYIDDEF
  /* Predefined typedefs and keywords follow. */
  CTID_MAX = 65536
};

/* Target-dependent type IDs. */
#if LJ_64
#define CTID_INT_PSZ	CTID_INT64
#define CTID_UINT_PSZ	CTID_UINT64
#else
#define CTID_INT_PSZ	CTID_INT32
#define CTID_UINT_PSZ	CTID_UINT32
#endif

#if LJ_ABI_WIN
#define CTID_WCHAR	CTID_UINT16
#elif LJ_TARGET_PPC
#define CTID_WCHAR	CTID_LINT32
#else
#define CTID_WCHAR	CTID_INT32
#endif

/* -- C tokens and keywords ----------------------------------------------- */

/* C lexer keywords. */
#define CTOKDEF(_) \
  _(IDENT, "<identifier>") _(STRING, "<string>") \
  _(INTEGER, "<integer>") _(EOF, "<eof>") \
  _(OROR, "||") _(ANDAND, "&&") _(EQ, "==") _(NE, "!=") \
  _(LE, "<=") _(GE, ">=") _(SHL, "<<") _(SHR, ">>") _(DEREF, "->")

/* Simple declaration specifiers. */
#define CDSDEF(_) \
  _(VOID) _(BOOL) _(CHAR) _(INT) _(FP) \
  _(LONG) _(LONGLONG) _(SHORT) _(COMPLEX) _(SIGNED) _(UNSIGNED) \
  _(CONST) _(VOLATILE) _(RESTRICT) _(INLINE) \
  _(TYPEDEF) _(EXTERN) _(STATIC) _(AUTO) _(REGISTER)

/* C keywords. */
#define CKWDEF(_) \
  CDSDEF(_) _(EXTENSION) _(ASM) _(ATTRIBUTE) \
  _(DECLSPEC) _(CCDECL) _(PTRSZ) \
  _(STRUCT) _(UNION) _(ENUM) \
  _(SIZEOF) _(ALIGNOF)

/* C token numbers. */
enum {
  CTOK_OFS = 255,
#define CTOKNUM(name, sym)	CTOK_##name,
#define CKWNUM(name)		CTOK_##name,
CTOKDEF(CTOKNUM)
CKWDEF(CKWNUM)
#undef CTOKNUM
#undef CKWNUM
  CTOK_FIRSTDECL = CTOK_VOID,
  CTOK_FIRSTSCL = CTOK_TYPEDEF,
  CTOK_LASTDECLFLAG = CTOK_REGISTER,
  CTOK_LASTDECL = CTOK_ENUM
};

/* Declaration specifier flags. */
enum {
#define CDSFLAG(name)	CDF_##name = (1u << (CTOK_##name - CTOK_FIRSTDECL)),
CDSDEF(CDSFLAG)
#undef CDSFLAG
  CDF__END
};

#define CDF_SCL  (CDF_TYPEDEF|CDF_EXTERN|CDF_STATIC|CDF_AUTO|CDF_REGISTER)

/* -- C type management --------------------------------------------------- */

#define ctype_ctsG(g)		(mref_acq((g)->ctype_state, CTState))

/* Get C type state. */
static LJ_AINLINE CTState *ctype_cts(lua_State *L)
{
  return ctype_ctsG(G(L));
}

/* Load FFI library on-demand. */
#define ctype_loadffi(L) \
  do { \
    if (!ctype_ctsG(G(L))) { \
      ptrdiff_t oldtop = (char *)L->top - mref(L->stack, char); \
      luaopen_ffi(L); \
      L->top = (TValue *)(mref(L->stack, char) + oldtop); \
    } \
  } while (0)

static LJ_AINLINE CTypeID ctype_top_acq(const CTState *cts)
{
  return (CTypeID)la_load32_acq(&cts->top);
}

static LJ_AINLINE void ctype_top_rel(CTState *cts, CTypeID top)
{
  la_store32_rel(&cts->top, top);
}

static LJ_AINLINE int ctype_top_cas(CTState *cts, uint32_t *oldp,
				    CTypeID top)
{
  return la_cas32(&cts->top, oldp, top, LA_ACQ_REL, LA_ACQ);
}

/* Check C type ID for validity when assertions are enabled. */
static LJ_AINLINE CTypeID ctype_check(CTState *cts, CTypeID id)
{
  lj_assertCTS(id > 0 && id < ctype_top_acq(cts), "bad CTID %d", id);
  return id;
}

/* Acquire current C type table header. */
static LJ_AINLINE CTypeTab *ctype_tabh_acq(const CTState *cts)
{
  return (CTypeTab *)la_loadptr_acq((void *const *)&cts->tabh);
}

static LJ_AINLINE void ctype_tabh_rel(CTState *cts, CTypeTab *tabh)
{
  la_storeptr_rel((void **)&cts->tabh, tabh);
}

static LJ_AINLINE int ctype_tabh_cas(CTState *cts, CTypeTab **oldp,
				     CTypeTab *tabh)
{
  return la_casptr((void **)&cts->tabh, (void **)oldp, tabh,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE CTypeTab *ctype_retiredtab_acq(const CTState *cts)
{
  return (CTypeTab *)la_loadptr_acq((void *const *)&cts->retiredtab);
}

static LJ_AINLINE int ctype_retiredtab_cas(CTState *cts, CTypeTab **oldp,
					   CTypeTab *tabh)
{
  return la_casptr((void **)&cts->retiredtab, (void **)oldp, tabh,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE CTypeTab *ctype_retiredtab_xchg_acqrel(CTState *cts,
							 CTypeTab *tabh)
{
  return (CTypeTab *)la_xchgptr_acqrel((void **)&cts->retiredtab, tabh);
}

static LJ_AINLINE CTypeID ctype_hash_head_acq(const CTState *cts, uint32_t h)
{
  return (CTypeID)(la_load32_acq(&cts->hash[h]) & 0xffffu);
}

static LJ_AINLINE int ctype_hash_head_cas(CTState *cts, uint32_t h,
					  CTypeID *oldid, CTypeID newid)
{
  uint32_t old = (uint32_t)*oldid;
  int ok = la_cas32(&cts->hash[h], &old, (uint32_t)newid,
		    LA_ACQ_REL, LA_ACQ);
  *oldid = (CTypeID)(old & 0xffffu);
  return ok;
}

/* Acquire current C type table. */
static LJ_AINLINE CType *ctype_tab_acq(CTState *cts)
{
  return ctype_tab_slot(ctype_tabh_acq(cts), 0);
}

/* Get C type for C type ID. */
static LJ_AINLINE CType *ctype_get(CTState *cts, CTypeID id)
{
  return &ctype_tab_acq(cts)[ctype_check(cts, id)];
}

/* Get child C type ID. */
static LJ_AINLINE CTypeID ctype_childid(CTState *cts, CType *ct)
{
  CTInfo info = ctype_info_acq(ct);
  lj_assertCTS(!(ctype_isvoid(info) || ctype_isstruct(info) ||
	       ctype_isbitfield(info)),
	       "ctype %08x has no children", info);
  return ctype_cid(info);
}

/* Get child C type. */
static LJ_AINLINE CType *ctype_child(CTState *cts, CType *ct)
{
  return ctype_get(cts, ctype_childid(cts, ct));
}

/* Get raw type ID for a C type ID. */
static LJ_AINLINE CTypeID ctype_rawid(CTState *cts, CTypeID id)
{
  CType *ct = ctype_get(cts, id);
  for (;;) {
    CTInfo info = ctype_info_acq(ct);
    if (!ctype_isattrib(info))
      return id;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
  }
}

/* Follow references and get raw type ID for a C type ID. */
static LJ_AINLINE CTypeID ctype_rawrefid(CTState *cts, CTypeID id)
{
  CType *ct = ctype_get(cts, id);
  for (;;) {
    CTInfo info = ctype_info_acq(ct);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      return id;
    id = ctype_cid(info);
    ct = ctype_get(cts, id);
  }
}

/* Get raw type for a C type ID. */
static LJ_AINLINE CType *ctype_raw(CTState *cts, CTypeID id)
{
  return ctype_get(cts, ctype_rawid(cts, id));
}

/* Get raw type of the child of a C type. */
static LJ_AINLINE CTypeID ctype_rawchildid(CTState *cts, CType *ct)
{
  CTypeID id;
  for (;;) {
    id = ctype_childid(cts, ct);
    ct = ctype_get(cts, id);
    if (!ctype_isattrib(ctype_info_acq(ct)))
      return id;
  }
}

/* Get raw type of the child of a C type. */
static LJ_AINLINE CType *ctype_rawchild(CTState *cts, CType *ct)
{
  return ctype_get(cts, ctype_rawchildid(cts, ct));
}

/* Set the name of a C type table element. */
static LJ_AINLINE void ctype_setname(CType *ct, GCstr *s)
{
  /* NOBARRIER: mark string as fixed -- the C type table is never collected. */
  fixstring(s);
  setgcrefrel(ct->name, obj2gco(s));
}

static LJ_AINLINE void ctype_clearname(CType *ct)
{
  setgcrefnullrel(ct->name);
}

static LJ_AINLINE GCobj *ctype_nameobj_acq(const CType *ct)
{
  return gcref_acq(ct->name);
}

static LJ_AINLINE void ctype_nameobj_rel(CType *ct, GCobj *o)
{
  if (o)
    setgcrefrel(ct->name, o);
  else
    setgcrefnullrel(ct->name);
}

static LJ_AINLINE GCstr *ctype_name_acq(const CType *ct)
{
  GCobj *o = ctype_nameobj_acq(ct);
  return o ? gco2str(o) : NULL;
}

static LJ_AINLINE void ctype_copy_rel(CType *dst, const CType *src)
{
  ctype_info_rel(dst, ctype_info_acq(src));
  ctype_size_rel(dst, ctype_size_acq(src));
  ctype_sib_rel(dst, ctype_sib_acq(src));
  ctype_next_rel(dst, ctype_next_acq(src));
  ctype_nameobj_rel(dst, ctype_nameobj_acq(src));
}

LJ_FUNC CTypeID lj_ctype_new_l(lua_State *L, CTState *cts, CType **ctp);
LJ_FUNC CTypeID lj_ctype_intern_l(lua_State *L, CTState *cts, CTInfo info,
				  CTSize size);
LJ_FUNC CTypeID lj_ctype_intern_new_l(lua_State *L, CTState *cts,
				      CTInfo info, CTSize size, int *newp);
LJ_FUNC int lj_ctype_predefined_string(const char *p, MSize len,
				       CTypeID *idp);
LJ_FUNC int lj_ctype_snapshot(CTState *cts, CTypeID id, CType *out);
LJ_FUNC int lj_ctype_size_predefined(CTState *cts, CTypeID id, CTSize *szp);
LJ_FUNC int lj_ctype_size_wait(lua_State *L, CTState *cts, CTypeID id,
			       CTSize *szp);
LJ_FUNC int lj_ctype_rawref_predefined(CTState *cts, CTypeID id,
				       CTypeID *ridp, CType *out);
LJ_FUNC int lj_ctype_info_predefined(CTState *cts, CTypeID id,
				     CTInfo *infop, CTSize *szp,
				     CTypeID *ridp, CType *rawp);
LJ_FUNC int lj_ctype_info_wait(lua_State *L, CTState *cts, CTypeID id,
			       CTInfo *infop, CTSize *szp, CTypeID *ridp,
			       CType *rawp);
LJ_FUNC int lj_ctype_enumconst_snapshot(CTState *cts, const CType *ct,
					GCstr *name, CTSize *valp,
					CTypeID *cidp);
LJ_FUNC int lj_ctype_enumconst_wait(lua_State *L, CTState *cts,
				    CTypeID id, GCstr *name,
				    CTSize *valp, CTypeID *cidp);
LJ_FUNC void lj_ctype_parse_wait(CTState *cts, lua_State *L, uint32_t seq);
LJ_FUNC void lj_ctype_parse_lock(CTState *cts, lua_State *L);
LJ_FUNC void lj_ctype_parse_unlock(CTState *cts);
LJ_FUNC GCtab *lj_ctype_fin_head(CTState *cts);
LJ_FUNC FinRegOrderNode *lj_ctype_fin_order_new(lua_State *L);
LJ_FUNC void lj_ctype_fin_order_free(global_State *g, FinRegOrderNode *ord);
LJ_FUNC void lj_ctype_fin_order_publish(CTState *cts, FinRegOrderNode *ord,
					GCobj *o, GCtab *t, TValue *slot);
LJ_FUNC int lj_ctype_fin_order_retire(CTState *cts, FinRegOrderNode *prev,
				      FinRegOrderNode *ord,
				      FinRegOrderNode *next);
LJ_FUNC cTValue *lj_ctype_fin_get(lua_State *L, CTState *cts, cTValue *key,
				  GCtab **tabp);
LJ_FUNC int lj_ctype_fin_newgen(lua_State *L, CTState *cts, cTValue *key,
				cTValue *claim, GCtab **tabp, TValue **slot);
LJ_FUNC int lj_ctype_fin_istab(global_State *g, GCtab *t);
LJ_FUNC void lj_ctype_fin_mark(global_State *g, void (*mark)(global_State *,
							    GCobj *),
			       void (*markmem)(global_State *, void *));
LJ_FUNC void lj_ctype_fin_freetabs(global_State *g, CTState *cts);
LJ_FUNC CType *lj_ctype_publish(CTState *cts, CTypeID id, CType *src);
LJ_FUNC int lj_ctype_setmeta(CTState *cts, CTypeID id, GCtab *mt);
LJ_FUNC void lj_ctype_cb_blacklist(CTState *cts, void *func);
LJ_FUNC int lj_ctype_cb_isblacklisted(CTState *cts, void *func);
LJ_FUNC void lj_ctype_addname(CTState *cts, CType *ct, CTypeID id);
LJ_FUNC CTypeID lj_ctype_addname_unique(CTState *cts, CType *ct, CTypeID id,
					uint32_t tmask);
LJ_FUNC CTypeID lj_ctype_getname(CTState *cts, CType **ctp, GCstr *name,
				 uint32_t tmask);
LJ_FUNC CType *lj_ctype_getfieldq(CTState *cts, CType *ct, GCstr *name,
				  CTSize *ofs, CTInfo *qual);
#define lj_ctype_getfield(cts, ct, name, ofs) \
  lj_ctype_getfieldq((cts), (ct), (name), (ofs), NULL)
LJ_FUNC int lj_ctype_getfieldq_snapshot(CTState *cts, const CType *ct,
					GCstr *name, CTSize *ofsp,
					CTInfo *qualp, CType *out);
LJ_FUNC int lj_ctype_getfieldq_wait(lua_State *L, CTState *cts,
				    CTypeID id, GCstr *name,
				    CTSize *ofsp, CTInfo *qualp,
				    CType *out);
LJ_FUNC int lj_ctype_ptrstruct_snapshot(CTState *cts, CTypeID id,
					CTypeID *cidp);
LJ_FUNC CType *lj_ctype_rawref(CTState *cts, CTypeID id);
LJ_FUNC CTSize lj_ctype_size(CTState *cts, CTypeID id);
LJ_FUNC int lj_ctype_rawref_snapshot(CTState *cts, CTypeID id,
				     CTypeID *ridp, CType *out);
LJ_FUNC int lj_ctype_info_snapshot(CTState *cts, CTypeID id,
				   CTInfo *infop, CTSize *szp,
				   CTypeID *ridp, CType *rawp);
LJ_FUNC int lj_ctype_size_snapshot(CTState *cts, CTypeID id, CTSize *szp);
LJ_FUNC int lj_ctype_getname_snapshot(CTState *cts, GCstr *name,
				      uint32_t tmask, CTypeID *idp,
				      CType *out, GCstr **redirp);
LJ_FUNC int lj_ctype_getname_wait(lua_State *L, CTState *cts, GCstr *name,
				  uint32_t tmask, CTypeID *idp, CType *out,
				  GCstr **redirp);
LJ_FUNC CTSize lj_ctype_vlsize(CTState *cts, CType *ct, CTSize nelem);
LJ_FUNC CTInfo lj_ctype_info(CTState *cts, CTypeID id, CTSize *szp);
LJ_FUNC CTInfo lj_ctype_info_raw(CTState *cts, CTypeID id, CTSize *szp);
LJ_FUNC cTValue *lj_ctype_meta(CTState *cts, CTypeID id, MMS mm);
LJ_FUNC cTValue *lj_ctype_metatv(CTState *cts, TValue *out,
				 CTypeID id, MMS mm);
LJ_FUNC int lj_ctype_predefined_nometa(CTState *cts, CTypeID id);
LJ_FUNC int lj_ctype_metatv_snapshot(CTState *cts, TValue *out,
				     CTypeID id, MMS mm);
LJ_FUNC cTValue *lj_ctype_metatv_wait(lua_State *L, CTState *cts,
				      TValue *out, CTypeID id, MMS mm);
LJ_FUNC GCstr *lj_ctype_repr(lua_State *L, CTypeID id, GCstr *name);
LJ_FUNC GCstr *lj_ctype_repr_int64(lua_State *L, uint64_t n, int isunsigned);
LJ_FUNC GCstr *lj_ctype_repr_complex(lua_State *L, void *sp, CTSize size);
LJ_FUNC uint32_t lj_ctype_reclaim_retired(global_State *g,
					  uint64_t completed_epoch);
LJ_FUNCA CTState *LJ_FASTCALL lj_ctype_ctsG_acq(global_State *g);
LJ_FUNC CTState *lj_ctype_init(lua_State *L);
LJ_FUNC void lj_ctype_freestate(global_State *g);

#endif

#endif
