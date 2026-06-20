/*
** LuaJIT VM tags, values and objects.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#ifndef _LJ_OBJ_H
#define _LJ_OBJ_H

#include "lua.h"
#include "lj_def.h"
#include "lj_arch.h"
#include "lj_atomic.h"

/* -- Memory references --------------------------------------------------- */

/* Memory and GC object sizes. */
typedef uint32_t MSize;
#if LJ_GC64
typedef uint64_t GCSize;
#else
typedef uint32_t GCSize;
#endif

/* Memory reference */
typedef struct MRef {
#if LJ_GC64
  uint64_t ptr64;	/* True 64 bit pointer. */
#else
  uint32_t ptr32;	/* Pseudo 32 bit pointer. */
#endif
} MRef;

#if LJ_GC64
#define mref(r, t)	((t *)(void *)(r).ptr64)
#define mref_acq(r, t)	((t *)(void *)(uintptr_t)la_load64_acq(&(r).ptr64))
#define mrefu(r)	((r).ptr64)

#define setmref(r, p)	((r).ptr64 = (uint64_t)(void *)(p))
#define setmrefu(r, u)	((r).ptr64 = (uint64_t)(u))
#define setmrefr(r, v)	((r).ptr64 = (v).ptr64)
#else
#define mref(r, t)	((t *)(void *)(uintptr_t)(r).ptr32)
#define mref_acq(r, t)	((t *)(void *)(uintptr_t)la_load32_acq(&(r).ptr32))
#define mrefu(r)	((r).ptr32)

#define setmref(r, p)	((r).ptr32 = (uint32_t)(uintptr_t)(void *)(p))
#define setmrefu(r, u)	((r).ptr32 = (uint32_t)(u))
#define setmrefr(r, v)	((r).ptr32 = (v).ptr32)
#endif

#if LJ_GC64
static LJ_AINLINE void setmrefrel_(MRef *r, const void *p)
{
  la_store64_rel(&r->ptr64, (uint64_t)(uintptr_t)p);
}
#else
static LJ_AINLINE void setmrefrel_(MRef *r, const void *p)
{
  la_store32_rel(&r->ptr32, (uint32_t)(uintptr_t)p);
}
#endif
#define setmrefrel(r, p)	setmrefrel_(&(r), (const void *)(p))

/* -- GC object references ------------------------------------------------ */

/* GCobj reference */
typedef struct GCRef {
#if LJ_GC64
  uint64_t gcptr64;	/* True 64 bit pointer. */
#else
  uint32_t gcptr32;	/* Pseudo 32 bit pointer. */
#endif
} GCRef;

/* Common GC header for all collectable objects. */
#define GCHeader	GCRef nextgc; uint8_t marked; uint8_t gct
/* This occupies 6 bytes, so use the next 2 bytes for non-32 bit fields. */

#if LJ_GC64
#define gcref(r)	((GCobj *)(r).gcptr64)
#define gcref_acq(r)	((GCobj *)(uintptr_t)la_load64_acq(&(r).gcptr64))
#define gcrefp(r, t)	((t *)(void *)(r).gcptr64)
#define gcrefu(r)	((r).gcptr64)
#define gcrefu_acq(r)	(la_load64_acq(&(r).gcptr64))
#define gcrefeq(r1, r2)	((r1).gcptr64 == (r2).gcptr64)

#define setgcref(r, gc)	((r).gcptr64 = (uint64_t)&(gc)->gch)
#define setgcreft(r, gc, it) \
  (r).gcptr64 = (uint64_t)&(gc)->gch | (((uint64_t)(it)) << 47)
#define setgcrefp(r, p)	((r).gcptr64 = (uint64_t)(p))
#define setgcrefnull(r)	((r).gcptr64 = 0)
#define setgcrefr(r, v)	((r).gcptr64 = (v).gcptr64)
#else
#define gcref(r)	((GCobj *)(uintptr_t)(r).gcptr32)
#define gcref_acq(r)	((GCobj *)(uintptr_t)la_load32_acq(&(r).gcptr32))
#define gcrefp(r, t)	((t *)(void *)(uintptr_t)(r).gcptr32)
#define gcrefu(r)	((r).gcptr32)
#define gcrefu_acq(r)	(la_load32_acq(&(r).gcptr32))
#define gcrefeq(r1, r2)	((r1).gcptr32 == (r2).gcptr32)

#define setgcref(r, gc)	((r).gcptr32 = (uint32_t)(uintptr_t)&(gc)->gch)
#define setgcrefp(r, p)	((r).gcptr32 = (uint32_t)(uintptr_t)(p))
#define setgcrefnull(r)	((r).gcptr32 = 0)
#define setgcrefr(r, v)	((r).gcptr32 = (v).gcptr32)
#endif

#define gcnext(gc)	(gcref((gc)->gch.nextgc))

/* IMPORTANT NOTE:
**
** All uses of the setgcref* macros MUST be accompanied with a write barrier.
**
** This is to ensure the integrity of the incremental GC. The invariant
** to preserve is that a black object never points to a white object.
** I.e. never store a white object into a field of a black object.
**
** It's ok to LEAVE OUT the write barrier ONLY in the following cases:
** - The source is not a GC object (NULL).
** - The target is a GC root. I.e. everything in global_State.
** - The target is a lua_State field (threads are never black).
** - The target is a stack slot, see setgcV et al.
** - The target is an open upvalue, i.e. pointing to a stack slot.
** - The target is a newly created object (i.e. marked white). But make
**   sure nothing invokes the GC inbetween.
** - The target and the source are the same object (self-reference).
** - The target already contains the object (e.g. moving elements around).
**
** The most common case is a store to a stack slot. All other cases where
** a barrier has been omitted are annotated with a NOBARRIER comment.
**
** The same logic applies for stores to table slots (array part or hash
** part). ALL uses of lj_tab_set* require a barrier for the stored value
** *and* the stored key, based on the above rules. In practice this means
** a barrier is needed if *either* of the key or value are a GC object.
**
** It's ok to LEAVE OUT the write barrier in the following special cases:
** - The stored value is nil. The key doesn't matter because it's either
**   not resurrected or lj_tab_newkey() will take care of the key barrier.
** - The key doesn't matter if the *previously* stored value is guaranteed
**   to be non-nil (because the key is kept alive in the table).
** - The key doesn't matter if it's guaranteed not to be part of the table,
**   since lj_tab_newkey() takes care of the key barrier. This applies
**   trivially to new tables, but watch out for resurrected keys. Storing
**   a nil value leaves the key in the table!
**
** In case of doubt use lj_gc_anybarriert() as it's rather cheap. It's used
** by the interpreter for all table stores.
**
** Note: In contrast to Lua's GC, LuaJIT's GC does *not* specially mark
** dead keys in tables. The reference is left in, but it's guaranteed to
** be never dereferenced as long as the value is nil. It's ok if the key is
** freed or if any object subsequently gets the same address.
**
** Not destroying dead keys helps to keep key hash slots stable. This avoids
** specialization back-off for HREFK when a value flips between nil and
** non-nil and the GC gets in the way. It also allows safely hoisting
** HREF/HREFK across GC steps. Dead keys are only removed if a table is
** resized (i.e. by NEWREF) and xREF must not be CSEd across a resize.
**
** The trade-off is that a write barrier for tables must take the key into
** account, too. Implicitly resurrecting the key by storing a non-nil value
** may invalidate the incremental GC invariant.
*/

/* -- Common type definitions --------------------------------------------- */

/* Types for handling bytecodes. Need this here, details in lj_bc.h. */
typedef uint32_t BCIns;  /* Bytecode instruction. */
typedef uint32_t BCPos;  /* Bytecode position. */
typedef uint32_t BCReg;  /* Bytecode register. */
typedef int32_t BCLine;  /* Bytecode line number. */

/* Internal assembler functions. Never call these directly from C. */
typedef void (*ASMFunction)(void);

/* Resizable string buffer. Need this here, details in lj_buf.h. */
#define SBufHeader	char *w, *e, *b; MRef L
typedef struct SBuf {
  SBufHeader;
} SBuf;

/* -- Tags and values ----------------------------------------------------- */

/* Frame link. */
typedef union {
  int32_t ftsz;		/* Frame type and size of previous frame. */
  MRef pcr;		/* Or PC for Lua frames. */
} FrameLink;

/* Tagged value. */
typedef LJ_ALIGN(8) union TValue {
  uint64_t u64;		/* 64 bit pattern overlaps number. */
  lua_Number n;		/* Number object overlaps split tag/value object. */
#if LJ_GC64
  GCRef gcr;		/* GCobj reference with tag. */
  int64_t it64;
  struct {
    LJ_ENDIAN_LOHI(
      int32_t i;	/* Integer value. */
    , uint32_t it;	/* Internal object tag. Must overlap MSW of number. */
    )
  };
#else
  struct {
    LJ_ENDIAN_LOHI(
      union {
	GCRef gcr;	/* GCobj reference (if any). */
	int32_t i;	/* Integer value. */
      };
    , uint32_t it;	/* Internal object tag. Must overlap MSW of number. */
    )
  };
#endif
#if LJ_FR2
  int64_t ftsz;		/* Frame type and size of previous frame, or PC. */
#else
  struct {
    LJ_ENDIAN_LOHI(
      GCRef func;	/* Function for next frame (or dummy L). */
    , FrameLink tp;	/* Link to previous frame. */
    )
  } fr;
#endif
  struct {
    LJ_ENDIAN_LOHI(
      uint32_t lo;	/* Lower 32 bits of number. */
    , uint32_t hi;	/* Upper 32 bits of number. */
    )
  } u32;
} TValue;

typedef const TValue cTValue;

#define tvref(r)	(mref(r, TValue))

#define tv_rawload(o)		la_load64_rlx(&(o)->u64)
#define tv_rawload_acq(o)	la_load64_acq(&(o)->u64)
#define tv_rawstore(o, u)	la_store64_rlx(&(o)->u64, (u))
#define tv_rawstore_rel(o, u)	la_store64_rel(&(o)->u64, (u))

/* More external and GCobj tags for internal objects. */
#define LAST_TT		LUA_TTHREAD
#define LUA_TPROTO	(LAST_TT+1)
#define LUA_TCDATA	(LAST_TT+2)

/* Internal object tags.
**
** Format for 32 bit GC references (!LJ_GC64):
**
** Internal tags overlap the MSW of a number object (must be a double).
** Interpreted as a double these are special NaNs. The FPU only generates
** one type of NaN (0xfff8_0000_0000_0000). So MSWs > 0xfff80000 are available
** for use as internal tags. Small negative numbers are used to shorten the
** encoding of type comparisons (reg/mem against sign-ext. 8 bit immediate).
**
**                  ---MSW---.---LSW---
** primitive types |  itype  |         |
** lightuserdata   |  itype  |  void * |  (32 bit platforms)
** lightuserdata   |ffff|seg|    ofs   |  (64 bit platforms)
** GC objects      |  itype  |  GCRef  |
** int (LJ_DUALNUM)|  itype  |   int   |
** number           -------double------
**
** Format for 64 bit GC references (LJ_GC64):
**
** The upper 13 bits must be 1 (0xfff8...) for a special NaN. The next
** 4 bits hold the internal tag. The lowest 47 bits either hold a pointer,
** a zero-extended 32 bit integer or all bits set to 1 for primitive types.
**
**                     ------MSW------.------LSW------
** primitive types    |1..1|itype|1..................1|
** GC objects         |1..1|itype|-------GCRef--------|
** lightuserdata      |1..1|itype|seg|------ofs-------|
** int (LJ_DUALNUM)   |1..1|itype|0..0|-----int-------|
** number              ------------double-------------
**
** ORDER LJ_T
** Primitive types nil/false/true must be first, lightuserdata next.
** GC objects are at the end, table/userdata must be lowest.
** Also check lj_ir.h for similar ordering constraints.
*/
#define LJ_TNIL			(~0u)
#define LJ_TFALSE		(~1u)
#define LJ_TTRUE		(~2u)
#define LJ_TLIGHTUD		(~3u)
#define LJ_TSTR			(~4u)
#define LJ_TUPVAL		(~5u)
#define LJ_TTHREAD		(~6u)
#define LJ_TPROTO		(~7u)
#define LJ_TFUNC		(~8u)
#define LJ_TTRACE		(~9u)
#define LJ_TCDATA		(~10u)
#define LJ_TTAB			(~11u)
#define LJ_TUDATA		(~12u)
/* This is just the canonical number type used in some places. */
#define LJ_TNUMX		(~13u)

/* Integers have itype == LJ_TISNUM doubles have itype < LJ_TISNUM */
#if LJ_64 && !LJ_GC64
#define LJ_TISNUM		0xfffeffffu
#else
#define LJ_TISNUM		LJ_TNUMX
#endif
#define LJ_TISTRUECOND		LJ_TFALSE
#define LJ_TISPRI		LJ_TTRUE
#define LJ_TISGCV		(LJ_TSTR+1)
#define LJ_TISTABUD		LJ_TTAB

/* Type marker for slot holding a traversal index. Must be lightuserdata. */
#define LJ_KEYINDEX		0xfffe7fffu

#if LJ_GC64
#define LJ_GCVMASK		(((uint64_t)1 << 47) - 1)
#endif

#if LJ_64
/* To stay within 47 bits, lightuserdata is segmented. */
#define LJ_LIGHTUD_BITS_SEG	8
#define LJ_LIGHTUD_BITS_LO	(47 - LJ_LIGHTUD_BITS_SEG)
#define LJ_LIGHTUD_INTERNAL_SEG	(((uint64_t)1 << LJ_LIGHTUD_BITS_SEG) - 1u)
#define LJ_LIGHTUD_INTERNAL_BASE \
  ((((uint64_t)LJ_TLIGHTUD) << 47) | \
   (LJ_LIGHTUD_INTERNAL_SEG << LJ_LIGHTUD_BITS_LO))
#define LJ_TFORWARD_BITS	(LJ_LIGHTUD_INTERNAL_BASE | 1u)
#define LJ_TKEYLOCK_BITS	(LJ_LIGHTUD_INTERNAL_BASE | 2u)
#endif

/* -- String object ------------------------------------------------------- */

typedef uint32_t StrHash;	/* String hash value. */
typedef uint32_t StrID;		/* String ID. */

typedef struct StrTabHdr {
  MSize mask;		/* String hash mask (size of hash table - 1). */
  MSize resize;		/* Reserved resize claim for M5 lock-free interning. */
  MSize copy_cursor;	/* Reserved resize copy cursor. */
  uint64_t retire_epoch;  /* Safepoint epoch when retired. */
  struct StrTabHdr *retired_next;  /* Retired string table headers. */
  GCRef bucket[1];	/* String hash table anchors. */
} StrTabHdr;

/* String object header. String payload follows. */
typedef struct GCstr {
  GCHeader;
  uint8_t reserved;	/* Used by lexer for fast lookup of reserved words. */
  uint8_t hashalg;	/* Hash algorithm. */
  StrID sid;		/* Interned string ID. */
  StrHash hash;		/* Hash of string. */
  MSize len;		/* Size of string. */
} GCstr;

#define strref(r)	(&gcref((r))->str)
#define strref_acq(r)	(&gcref_acq((r))->str)
#define strdata(s)	((const char *)((s)+1))
#define strdatawr(s)	((char *)((s)+1))
#define strVdata(o)	strdata(strV(o))

/* -- Userdata object ----------------------------------------------------- */

/* Userdata object. Payload follows. */
typedef struct GCudata {
  GCHeader;
  uint8_t udtype;	/* Userdata type. */
  uint8_t unused2;
  GCRef env;		/* Should be at same offset in GCfunc. */
  MSize len;		/* Size of payload. */
  GCRef metatable;	/* Must be at same offset in GCtab. */
  uint32_t align1;	/* To force 8 byte alignment of the payload. */
} GCudata;

/* Userdata types. */
enum {
  UDTYPE_USERDATA,	/* Regular userdata. */
  UDTYPE_IO_FILE,	/* I/O library FILE. */
  UDTYPE_FFI_CLIB,	/* FFI C library namespace. */
  UDTYPE_FFI_PIN,	/* FFI pinned Lua value. */
  UDTYPE_BUFFER,	/* String buffer. */
  UDTYPE_CHANNEL,	/* threading.channel object. */
  UDTYPE_THREAD,	/* threading.thread object. */
  UDTYPE_MUTEX,		/* threading.mutex object. */
  UDTYPE__MAX
};

static LJ_AINLINE uint8_t lj_udata_udtype_acq(const GCudata *ud)
{
  return la_load8_acq(&ud->udtype);
}

static LJ_AINLINE void lj_udata_udtype_rel(GCudata *ud, uint8_t udtype)
{
  la_store8_rel(&ud->udtype, udtype);
}

#define uddata(u)	((void *)((u)+1))
#define sizeudata(u)	(sizeof(struct GCudata)+(u)->len)

/* -- C data object ------------------------------------------------------- */

/* C data object. Payload follows. */
typedef struct GCcdata {
  GCHeader;
  uint16_t ctypeid;	/* C type ID. */
} GCcdata;

/* Prepended to variable-sized or realigned C data objects. */
typedef struct GCcdataVar {
  uint16_t offset;	/* Offset to allocated memory (relative to GCcdata). */
  uint16_t extra;	/* Extra space allocated (incl. GCcdata + GCcdatav). */
  MSize len;		/* Size of payload. */
} GCcdataVar;

#define cdataptr(cd)	((void *)((cd)+1))
#define cdataisv(cd)	((cd)->marked & 0x80)
#define cdatav(cd)	((GCcdataVar *)((char *)(cd) - sizeof(GCcdataVar)))
#define cdatavlen(cd)	check_exp(cdataisv(cd), cdatav(cd)->len)
#define sizecdatav(cd)	(cdatavlen(cd) + cdatav(cd)->extra)
#define memcdatav(cd)	((void *)((char *)(cd) - cdatav(cd)->offset))

/* -- Prototype object ---------------------------------------------------- */

#define SCALE_NUM_GCO	((int32_t)sizeof(lua_Number)/sizeof(GCRef))
#define round_nkgc(n)	(((n) + SCALE_NUM_GCO-1) & ~(SCALE_NUM_GCO-1))

typedef struct GCproto {
  GCHeader;
  uint8_t numparams;	/* Number of parameters. */
  uint8_t framesize;	/* Fixed frame size. */
  MSize sizebc;		/* Number of bytecode instructions. */
#if LJ_GC64
  uint32_t flags2;	/* Extended prototype flags. */
#endif
  GCRef gclist;
  MRef k;		/* Split constant array (points to the middle). */
  MRef uv;		/* Upvalue list. local slot|0x8000 or parent uv idx. */
  MSize sizekgc;	/* Number of collectable constants. */
  MSize sizekn;		/* Number of lua_Number constants. */
  MSize sizept;		/* Total size including colocated arrays. */
  uint8_t sizeuv;	/* Number of upvalues. */
  uint8_t flags;	/* Miscellaneous flags (see below). */
  uint16_t trace;	/* Anchor for chain of root traces. */
  /* ------ The following fields are for debugging/tracebacks only ------ */
  GCRef chunkname;	/* Name of the chunk this function was defined in. */
  BCLine firstline;	/* First line of the function definition. */
  BCLine numline;	/* Number of lines for the function definition. */
  MRef lineinfo;	/* Compressed map from bytecode ins. to source line. */
  MRef uvinfo;		/* Upvalue names. */
  MRef varinfo;		/* Names and compressed extents of local variables. */
} GCproto;

/* Flags for prototype. */
#define PROTO_CHILD		0x01	/* Has child prototypes. */
#define PROTO_VARARG		0x02	/* Vararg function. */
#define PROTO_FFI		0x04	/* Uses BC_KCDATA for FFI datatypes. */
#define PROTO_NOJIT		0x08	/* JIT disabled for this function. */
#define PROTO_ILOOP		0x10	/* Patched bytecode with ILOOP etc. */
/* Only used during parsing. */
#define PROTO_HAS_RETURN	0x20	/* Already emitted a return. */
#define PROTO_FIXUP_RETURN	0x40	/* Need to fixup emitted returns. */
/* Top bits used for counting created closures. */
#define PROTO_CLCOUNT		0x20	/* Base of saturating 3 bit counter. */
#define PROTO_CLC_BITS		3
#define PROTO_CLC_POLY		(3*PROTO_CLCOUNT)  /* Polymorphic threshold. */

/* Extended prototype flags. */
#define PROTO2_LEGACYUV		0x00000001u  /* Loaded from v2 bytecode. */
#define PROTO2_CELLUV		0x00000002u  /* Local upvalues are cell slots. */

#if LJ_GC64
#define proto_initflags2(pt)	((pt)->flags2 = 0)
#define proto_legacyuv(pt)	(((pt)->flags2 & PROTO2_LEGACYUV) != 0)
#define proto_setlegacyuv(pt)	((pt)->flags2 |= PROTO2_LEGACYUV)
#define proto_celluv(pt)	(((pt)->flags2 & PROTO2_CELLUV) != 0)
#define proto_setcelluv(pt)	((pt)->flags2 |= PROTO2_CELLUV)
#else
#define proto_initflags2(pt)	((void)0)
#define proto_legacyuv(pt)	0
#define proto_setlegacyuv(pt)	((void)0)
#define proto_celluv(pt)	0
#define proto_setcelluv(pt)	((void)0)
#endif

#define PROTO_UV_LOCAL		0x8000	/* Upvalue for local slot. */
#define PROTO_UV_IMMUTABLE	0x4000	/* Immutable upvalue. */

#define proto_kgc(pt, idx) \
  check_exp((uintptr_t)(intptr_t)(idx) >= ~(uintptr_t)(pt)->sizekgc+1u, \
	    gcref(mref((pt)->k, GCRef)[(idx)]))
#define proto_kgc_acq(pt, idx) \
  check_exp((uintptr_t)(intptr_t)(idx) >= ~(uintptr_t)(pt)->sizekgc+1u, \
	    gcref_acq(mref((pt)->k, GCRef)[(idx)]))
#define proto_knumtv(pt, idx) \
  check_exp((uintptr_t)(idx) < (pt)->sizekn, &mref((pt)->k, TValue)[(idx)])
#define proto_bc(pt)		((BCIns *)((char *)(pt) + sizeof(GCproto)))
#define proto_bcpos(pt, pc)	((BCPos)((pc) - proto_bc(pt)))
#define proto_uv(pt)		(mref((pt)->uv, uint16_t))

#define proto_chunkname(pt)	(strref((pt)->chunkname))
#define proto_chunkname_acq(pt)	(strref_acq((pt)->chunkname))
#define proto_chunknamestr(pt)	(strdata(proto_chunkname((pt))))
#define proto_chunknamestr_acq(pt)	(strdata(proto_chunkname_acq((pt))))
#define proto_lineinfo(pt)	(mref((pt)->lineinfo, const void))
#define proto_uvinfo(pt)	(mref((pt)->uvinfo, const uint8_t))
#define proto_varinfo(pt)	(mref((pt)->varinfo, const uint8_t))

/* -- Upvalue object ------------------------------------------------------ */

typedef struct GCupval {
  GCHeader;
  uint8_t closed;	/* Set if closed (i.e. uv->v == &uv->u.value). */
  uint8_t immutable;	/* Immutable value. */
  union {
    TValue tv;		/* If closed: the value itself. */
    struct {		/* If open: double linked list, anchored at thread. */
      GCRef prev;
      GCRef next;
    };
  };
  MRef v;		/* Points to stack slot (open) or above (closed). */
  uint32_t dhash;	/* Disambiguation hash: dh1 != dh2 => cannot alias. */
} GCupval;

#define uvprev(uv_)	(&gcref((uv_)->prev)->uv)
#define uvnext(uv_)	(&gcref((uv_)->next)->uv)
#define uvval(uv_)	(mref((uv_)->v, TValue))

/* -- Function object (closures) ------------------------------------------ */

/* Common header for functions. env should be at same offset in GCudata. */
#define GCfuncHeader \
  GCHeader; uint8_t ffid; uint8_t nupvalues; \
  GCRef env; GCRef gclist; MRef pc

typedef struct GCfuncC {
  GCfuncHeader;
  lua_CFunction f;	/* C function to be called. */
  TValue upvalue[1];	/* Array of upvalues (TValue). */
} GCfuncC;

typedef struct GCfuncL {
  GCfuncHeader;
  GCRef uvptr[1];	/* Array of _pointers_ to upvalue objects (GCupval). */
} GCfuncL;

typedef union GCfunc {
  GCfuncC c;
  GCfuncL l;
} GCfunc;

#define FF_LUA		0
#define FF_C		1
#define isluafunc(fn)	((fn)->c.ffid == FF_LUA)
#define iscfunc(fn)	((fn)->c.ffid == FF_C)
#define isffunc(fn)	((fn)->c.ffid > FF_C)
#define funcproto(fn) \
  check_exp(isluafunc(fn), (GCproto *)(mref((fn)->l.pc, char)-sizeof(GCproto)))
#define sizeCfunc(n)	(sizeof(GCfuncC)-sizeof(TValue)+sizeof(TValue)*(n))
#define sizeLfunc(n)	(sizeof(GCfuncL)-sizeof(GCRef)+sizeof(GCRef)*(n))

/* -- Table object -------------------------------------------------------- */

/* Hash node. */
typedef struct Node {
  TValue val;		/* Value object. Must be first field. */
  TValue key;		/* Key object. */
  MRef next;		/* Hash chain. */
#if !LJ_GC64
  MRef freetop;		/* Top of free elements (stored in t->node[0]). */
#endif
} Node;

LJ_STATIC_ASSERT(offsetof(Node, val) == 0);

typedef struct TabNodeHdr {
  MSize hmask;		/* Hash mask paired with the following Node vector. */
  MSize flags;		/* Low bits: freecount. High bits: state flags. */
  MRef next_gen;	/* Replacement generation during/after retirement. */
#if !LJ_GC64
  MSize reserved;	/* Keep Node[0] aligned after the header. */
#endif
} TabNodeHdr;

#define TABNODE_FREECOUNT_BITS	31
#define TABNODE_FREECOUNT_MASK	((((MSize)1u) << TABNODE_FREECOUNT_BITS) - 1u)
#define TABNODE_FLAGS_MASK	((MSize)~TABNODE_FREECOUNT_MASK)
#define TABNODE_FLAG_RETIRING	(((MSize)1u) << 31)

LJ_STATIC_ASSERT(sizeof(TabNodeHdr) == 16);
LJ_STATIC_ASSERT(((MSize)1u << LJ_MAX_HBITS) <= TABNODE_FREECOUNT_MASK);

typedef struct TabNodeRetire {
  Node *node;		/* Retired hash vector, owned only when armed. */
  MSize hmask;		/* Original hash mask for vector free. */
  uint64_t retire_epoch;  /* Safepoint epoch when retired. */
  uint32_t armed;	/* Node vector has been unpublished from its table. */
  struct TabNodeRetire *next;
} TabNodeRetire;

typedef struct TabArrayHdr {
  MSize asize;		/* Visible array size paired with the slots vector. */
  MSize acap;		/* Capacity plus high-bit state flags. */
  MRef next_gen;	/* Replacement array during/after retirement. */
#if !LJ_GC64
  MSize reserved;	/* Keep slots aligned after the header. */
#endif
} TabArrayHdr;

#define TABARRAY_ACAP_BITS	28
#define TABARRAY_ACAP_MASK	((((MSize)1u) << TABARRAY_ACAP_BITS) - 1u)
#define TABARRAY_FLAGS_MASK	((MSize)~TABARRAY_ACAP_MASK)
#define TABARRAY_FLAG_RETIRING	(((MSize)1u) << 31)

LJ_STATIC_ASSERT(sizeof(TabArrayHdr) == 16);
LJ_STATIC_ASSERT(LJ_MAX_ASIZE <= TABARRAY_ACAP_MASK);
LJ_STATIC_ASSERT((TABARRAY_FLAG_RETIRING & TABARRAY_ACAP_MASK) == 0);

typedef struct TabArrayRetire {
  TValue *array;	/* Retired array vector, owned only when armed. */
  MSize acap;		/* Original array capacity for vector free. */
  uint64_t retire_epoch;  /* Safepoint epoch when retired. */
  uint32_t armed;	/* Array vector has been unpublished from its table. */
  struct TabArrayRetire *next;
} TabArrayRetire;

typedef struct GC2FinRegUDataNode {
  GCRef obj;		/* Userdata object tracked for metatable __gc. */
  struct GC2FinRegUDataNode *next;
  struct GC2FinRegUDataNode *retired_next;
  uint32_t active;	/* Node is still part of active discovery set. */
} GC2FinRegUDataNode;

typedef struct GCtab {
  GCHeader;
  uint8_t nomm;		/* Negative cache for fast metamethods. */
  int8_t colo;		/* Array colocation. */
  MRef array;		/* Array part. */
  GCRef gclist;
  GCRef metatable;	/* Must be at same offset in GCudata. */
  MRef node;		/* Hash part. */
  uint32_t asize;	/* Size of array part (keys [0, asize-1]). */
  uint32_t hmask;	/* Hash part mask (size of hash part - 1). */
#if LJ_GC64
  MRef freetop;		/* Top of free elements. */
#endif
  uint32_t acap;	/* Allocated array capacity. */
} GCtab;

#define sizetabcolo(n)	((n)*sizeof(TValue) + sizeof(GCtab))
#define tabref(r)	((GCtab *)gcref((r)))
#define tabref_acq(r)	((GCtab *)gcref_acq((r)))
#define noderef(r)	(mref((r), Node))
#define nextnode(n)	(mref((n)->next, Node))

static LJ_AINLINE TValue *lj_tab_array_acq(const GCtab *t)
{
#if LJ_GC64
  return (TValue *)(void *)(uintptr_t)la_load64_acq(&t->array.ptr64);
#else
  return (TValue *)(void *)(uintptr_t)la_load32_acq(&t->array.ptr32);
#endif
}

static LJ_AINLINE void lj_tab_array_set(GCtab *t, const TValue *array)
{
  setmref(t->array, array);
}

static LJ_AINLINE void lj_tab_array_rel(GCtab *t, const TValue *array)
{
#if LJ_GC64
  la_store64_rel(&t->array.ptr64, (uint64_t)(uintptr_t)(const void *)array);
#else
  la_store32_rel(&t->array.ptr32, (uint32_t)(uintptr_t)(const void *)array);
#endif
}

static LJ_AINLINE MSize lj_tab_asize_acq(const GCtab *t)
{
  return (MSize)la_load32_acq(&t->asize);
}

static LJ_AINLINE void lj_tab_asize_rel(GCtab *t, MSize asize)
{
  la_store32_rel(&t->asize, (uint32_t)asize);
}

static LJ_AINLINE int lj_tab_array_separated(const GCtab *t)
{
  return LJ_MAX_COLOSIZE == 0 || t->colo <= 0;
}

static LJ_AINLINE const TabArrayHdr *lj_tab_array_hdr(const TValue *array)
{
  return (const TabArrayHdr *)(const void *)
    ((const char *)(const void *)array - sizeof(TabArrayHdr));
}

static LJ_AINLINE TabArrayHdr *lj_tab_array_hdrw(TValue *array)
{
  return (TabArrayHdr *)(void *)((char *)(void *)array - sizeof(TabArrayHdr));
}

static LJ_AINLINE TValue *lj_tab_array_slots(TabArrayHdr *hdr)
{
  return (TValue *)(void *)((char *)(void *)hdr + sizeof(TabArrayHdr));
}

static LJ_AINLINE GCSize lj_tab_array_bytes(MSize acap)
{
  return (GCSize)sizeof(TabArrayHdr) + (GCSize)acap * (GCSize)sizeof(TValue);
}

static LJ_AINLINE MSize lj_tab_array_hdr_pack_acap(MSize acap, MSize flags)
{
  return (acap & TABARRAY_ACAP_MASK) | (flags & TABARRAY_FLAGS_MASK);
}

static LJ_AINLINE void lj_tab_array_hdr_init(TabArrayHdr *hdr, MSize asize,
					     MSize acap)
{
  hdr->asize = asize;
  hdr->acap = lj_tab_array_hdr_pack_acap(acap, 0);
  setmref(hdr->next_gen, NULL);
#if !LJ_GC64
  hdr->reserved = 0;
#endif
}

static LJ_AINLINE int lj_tab_array_is_colocated(const GCtab *t,
						const TValue *array)
{
#if LJ_MAX_COLOSIZE != 0
  return array == (const TValue *)(const void *)
    ((const char *)(const void *)t + sizeof(GCtab));
#else
  UNUSED(t); UNUSED(array);
  return 0;
#endif
}

static LJ_AINLINE MSize lj_tab_array_hdr_asize_acq(const TValue *array)
{
  return (MSize)la_load32_acq(&lj_tab_array_hdr(array)->asize);
}

static LJ_AINLINE MSize lj_tab_array_hdr_acap_acq(const TValue *array)
{
  return (MSize)la_load32_acq(&lj_tab_array_hdr(array)->acap) &
	 TABARRAY_ACAP_MASK;
}

static LJ_AINLINE MSize lj_tab_array_hdr_flags_acq(const TValue *array)
{
  return (MSize)la_load32_acq(&lj_tab_array_hdr(array)->acap) &
	 TABARRAY_FLAGS_MASK;
}

static LJ_AINLINE TValue *lj_tab_array_nextgen_acq(const TValue *array)
{
#if LJ_GC64
  return (TValue *)(void *)(uintptr_t)
    la_load64_acq(&lj_tab_array_hdr(array)->next_gen.ptr64);
#else
  return (TValue *)(void *)(uintptr_t)
    la_load32_acq(&lj_tab_array_hdr(array)->next_gen.ptr32);
#endif
}

static LJ_AINLINE void lj_tab_array_nextgen_rel(TValue *array,
						const TValue *next)
{
#if LJ_GC64
  la_store64_rel(&lj_tab_array_hdrw(array)->next_gen.ptr64,
		 (uint64_t)(uintptr_t)(const void *)next);
#else
  la_store32_rel(&lj_tab_array_hdrw(array)->next_gen.ptr32,
		 (uint32_t)(uintptr_t)(const void *)next);
#endif
}

static LJ_AINLINE int lj_tab_array_is_retiring(const GCtab *t,
					       const TValue *array)
{
  return array && !lj_tab_array_is_colocated(t, array) &&
	 ((lj_tab_array_hdr_flags_acq(array) & TABARRAY_FLAG_RETIRING) != 0);
}

static LJ_AINLINE void lj_tab_array_hdr_flags_or_rel(TValue *array,
						     MSize flags)
{
  uint32_t *word = &lj_tab_array_hdrw(array)->acap;
  uint32_t old = la_load32_acq(word);
  uint32_t want;
  flags &= TABARRAY_FLAGS_MASK;
  /* 06 section 6.3.2: publish generation state before replacement. */
  do {
    want = old | (uint32_t)flags;
  } while (old != want && !la_cas32(word, &old, want, LA_ACQ_REL, LA_ACQ));
}

static LJ_AINLINE MSize lj_tab_array_snapshot_acq(const GCtab *t,
						  TValue **arrayp)
{
  MSize asize;
  TValue *array;
retry_snapshot:
  asize = lj_tab_asize_acq(t);
  array = lj_tab_array_acq(t);
  if (lj_tab_array_is_retiring(t, array)) {
    la_cpu_pause();
    goto retry_snapshot;
  }
  if (array && !lj_tab_array_is_colocated(t, array))
    asize = lj_tab_array_hdr_asize_acq(array);
  *arrayp = array;
  return asize;
}

static LJ_AINLINE void *lj_tab_array_mem_acq(const GCtab *t)
{
  TValue *array;
  (void)lj_tab_array_snapshot_acq(t, &array);
  if (array && !lj_tab_array_is_colocated(t, array))
    return (void *)lj_tab_array_hdrw(array);
  return (void *)array;
}

static LJ_AINLINE MSize lj_tab_array_separated_snapshot_acq(const GCtab *t,
							    TValue **arrayp)
{
  TValue *array;
retry_snapshot:
  array = lj_tab_array_acq(t);
  if (lj_tab_array_is_retiring(t, array)) {
    la_cpu_pause();
    goto retry_snapshot;
  }
  *arrayp = array;
  if (array && !lj_tab_array_is_colocated(t, array))
    return lj_tab_array_hdr_acap_acq(array);
  return 0;
}

static LJ_AINLINE MSize lj_tab_array_separated_acap_acq(const GCtab *t)
{
  TValue *array;
  return lj_tab_array_separated_snapshot_acq(t, &array);
}

static LJ_AINLINE Node *lj_tab_node_acq(const GCtab *t)
{
#if LJ_GC64
  return (Node *)(void *)(uintptr_t)la_load64_acq(&t->node.ptr64);
#else
  return (Node *)(void *)(uintptr_t)la_load32_acq(&t->node.ptr32);
#endif
}

static LJ_AINLINE void lj_tab_node_set(GCtab *t, const Node *node)
{
  setmref(t->node, node);
}

static LJ_AINLINE void lj_tab_node_rel(GCtab *t, const Node *node)
{
#if LJ_GC64
  la_store64_rel(&t->node.ptr64, (uint64_t)(uintptr_t)(const void *)node);
#else
  la_store32_rel(&t->node.ptr32, (uint32_t)(uintptr_t)(const void *)node);
#endif
}

static LJ_AINLINE const TabNodeHdr *lj_tab_node_hdr(const Node *node)
{
  return (const TabNodeHdr *)(const void *)
    ((const char *)(const void *)node - sizeof(TabNodeHdr));
}

static LJ_AINLINE TabNodeHdr *lj_tab_node_hdrw(Node *node)
{
  return (TabNodeHdr *)(void *)((char *)(void *)node - sizeof(TabNodeHdr));
}

static LJ_AINLINE GCSize lj_tab_node_bytes(MSize hmask)
{
  return (GCSize)sizeof(TabNodeHdr) +
	 (GCSize)(hmask + 1u) * (GCSize)sizeof(Node);
}

static LJ_AINLINE MSize lj_tab_node_hmask_acq(const Node *node)
{
  return (MSize)la_load32_acq(&lj_tab_node_hdr(node)->hmask);
}

static LJ_AINLINE void lj_tab_node_hmask_set(Node *node, MSize hmask)
{
  lj_tab_node_hdrw(node)->hmask = hmask;
}

static LJ_AINLINE MSize lj_tab_node_hdr_flags_acq(const Node *node)
{
  return (MSize)la_load32_acq(&lj_tab_node_hdr(node)->flags) &
	 TABNODE_FLAGS_MASK;
}

static LJ_AINLINE MSize lj_tab_node_freecount_acq(const Node *node)
{
  return (MSize)la_load32_acq(&lj_tab_node_hdr(node)->flags) &
	 TABNODE_FREECOUNT_MASK;
}

static LJ_AINLINE void lj_tab_node_freecount_set_rel(Node *node,
						     MSize freecount)
{
  uint32_t *word = &lj_tab_node_hdrw(node)->flags;
  uint32_t old = la_load32_acq(word);
  uint32_t want;
  freecount &= TABNODE_FREECOUNT_MASK;
  /* 06 section 6.3.4: keep freecount atomic with generation flags. */
  do {
    want = (old & (uint32_t)TABNODE_FLAGS_MASK) | (uint32_t)freecount;
  } while (old != want && !la_cas32(word, &old, want, LA_ACQ_REL, LA_ACQ));
}

static LJ_AINLINE int lj_tab_node_free_reserve(Node *node)
{
  uint32_t *word = &lj_tab_node_hdrw(node)->flags;
  uint32_t old = la_load32_acq(word);
  for (;;) {
    uint32_t count = old & (uint32_t)TABNODE_FREECOUNT_MASK;
    if (old & (uint32_t)TABNODE_FLAG_RETIRING)
      return -1;
    if (count == 0)
      return 0;
    if (la_cas32(word, &old, (old & (uint32_t)TABNODE_FLAGS_MASK) |
		 (count - 1u), LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE void lj_tab_node_free_release(Node *node)
{
  uint32_t *word = &lj_tab_node_hdrw(node)->flags;
  uint32_t old = la_load32_acq(word);
  uint32_t want;
  /* 06 section 6.3.4: return an abandoned key-claim reservation. */
  do {
    uint32_t count = old & (uint32_t)TABNODE_FREECOUNT_MASK;
    want = (old & (uint32_t)TABNODE_FLAGS_MASK) |
	   ((count + 1u) & (uint32_t)TABNODE_FREECOUNT_MASK);
  } while (!la_cas32(word, &old, want, LA_ACQ_REL, LA_ACQ));
}

static LJ_AINLINE Node *lj_tab_node_nextgen_acq(const Node *node)
{
#if LJ_GC64
  return (Node *)(void *)(uintptr_t)
    la_load64_acq(&lj_tab_node_hdr(node)->next_gen.ptr64);
#else
  return (Node *)(void *)(uintptr_t)
    la_load32_acq(&lj_tab_node_hdr(node)->next_gen.ptr32);
#endif
}

static LJ_AINLINE void lj_tab_node_nextgen_rel(Node *node, const Node *next)
{
#if LJ_GC64
  la_store64_rel(&lj_tab_node_hdrw(node)->next_gen.ptr64,
		 (uint64_t)(uintptr_t)(const void *)next);
#else
  la_store32_rel(&lj_tab_node_hdrw(node)->next_gen.ptr32,
		 (uint32_t)(uintptr_t)(const void *)next);
#endif
}

static LJ_AINLINE int lj_tab_node_is_retiring(const Node *node)
{
  return (lj_tab_node_hdr_flags_acq(node) & TABNODE_FLAG_RETIRING) != 0;
}

static LJ_AINLINE void lj_tab_node_hdr_flags_or_rel(Node *node, MSize flags)
{
  uint32_t *word = &lj_tab_node_hdrw(node)->flags;
  uint32_t old = la_load32_acq(word);
  uint32_t want;
  flags &= TABNODE_FLAGS_MASK;
  /* 06 section 6.3.4: publish generation state before replacement. */
  do {
    want = old | (uint32_t)flags;
  } while (old != want && !la_cas32(word, &old, want, LA_ACQ_REL, LA_ACQ));
}

static LJ_AINLINE Node *lj_tab_node_snapshot_acq(const GCtab *t,
						 MSize *hmaskp)
{
  Node *node;
  MSize hmask;
retry_snapshot:
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);
  if (lj_tab_node_is_retiring(node)) {
    la_cpu_pause();
    goto retry_snapshot;
  }
  *hmaskp = hmask;
  return node;
}

static LJ_AINLINE MSize lj_tab_hmask_acq(const GCtab *t)
{
  return (MSize)la_load32_acq(&t->hmask);
}

static LJ_AINLINE void lj_tab_hmask_rel(GCtab *t, MSize hmask)
{
  la_store32_rel(&t->hmask, (uint32_t)hmask);
}

static LJ_AINLINE Node *lj_tab_nextnode_acq(const Node *n)
{
#if LJ_GC64
  return (Node *)(void *)(uintptr_t)la_load64_acq(&n->next.ptr64);
#else
  return (Node *)(void *)(uintptr_t)la_load32_acq(&n->next.ptr32);
#endif
}

static LJ_AINLINE void lj_tab_nextnode_set(Node *n, const Node *next)
{
  setmref(n->next, next);
}

static LJ_AINLINE void lj_tab_nextnode_rel(Node *n, const Node *next)
{
#if LJ_GC64
  la_store64_rel(&n->next.ptr64, (uint64_t)(uintptr_t)(const void *)next);
#else
  la_store32_rel(&n->next.ptr32, (uint32_t)(uintptr_t)(const void *)next);
#endif
}
#if LJ_GC64
#define getfreetop(t, n)	(noderef((t)->freetop))
#define setfreetop(t, n, v)	(setmref((t)->freetop, (v)))
#else
#define getfreetop(t, n)	(noderef((n)->freetop))
#define setfreetop(t, n, v)	(setmref((n)->freetop, (v)))
#endif

/* -- State objects ------------------------------------------------------- */

/* VM states. */
enum {
  LJ_VMST_INTERP,	/* Interpreter. */
  LJ_VMST_C,		/* C function. */
  LJ_VMST_GC,		/* Garbage collector. */
  LJ_VMST_EXIT,		/* Trace exit handler. */
  LJ_VMST_RECORD,	/* Trace recorder. */
  LJ_VMST_OPT,		/* Optimizer. */
  LJ_VMST_ASM,		/* Assembler. */
  LJ_VMST__MAX
};

#define setvmstate(g, st)	((g)->vmstate = ~LJ_VMST_##st)

/* Metamethods. ORDER MM */
#ifdef LJ_HASFFI
#define MMDEF_FFI(_) _(new)
#else
#define MMDEF_FFI(_)
#endif

#if LJ_52 || LJ_HASFFI
#define MMDEF_PAIRS(_) _(pairs) _(ipairs)
#else
#define MMDEF_PAIRS(_)
#define MM_pairs	255
#define MM_ipairs	255
#endif

#define MMDEF(_) \
  _(index) _(newindex) _(gc) _(mode) _(eq) _(len) \
  /* Only the above (fast) metamethods are negative cached (max. 8). */ \
  _(lt) _(le) _(concat) _(call) \
  /* The following must be in ORDER ARITH. */ \
  _(add) _(sub) _(mul) _(div) _(mod) _(pow) _(unm) \
  /* The following are used in the standard libraries. */ \
  _(metatable) _(tostring) MMDEF_FFI(_) MMDEF_PAIRS(_)

typedef enum {
#define MMENUM(name)	MM_##name,
MMDEF(MMENUM)
#undef MMENUM
  MM__MAX,
  MM____ = MM__MAX,
  MM_FAST = MM_len
} MMS;

/* GC root IDs. */
typedef enum {
  GCROOT_MMNAME,	/* Metamethod names. */
  GCROOT_MMNAME_LAST = GCROOT_MMNAME + MM__MAX-1,
  GCROOT_BASEMT,	/* Metatables for base types. */
  GCROOT_BASEMT_NUM = GCROOT_BASEMT + ~LJ_TNUMX,
  GCROOT_IO_INPUT,	/* Userdata for default I/O input file. */
  GCROOT_IO_OUTPUT,	/* Userdata for default I/O output file. */
  GCROOT_THREADING_ENV,	/* threading.* private function environment. */
  GCROOT_MAX
} GCRootID;

#define basemt_it(g, it)	((g)->gcroot[GCROOT_BASEMT+~(it)])
#define basemt_obj(g, o)	((g)->gcroot[GCROOT_BASEMT+itypemap(o)])
#define mmname_str(g, mm)	(strref_acq((g)->gcroot[GCROOT_MMNAME+(mm)]))

/* Garbage collector state. */
typedef struct GCState {
  GCSize total;		/* Memory currently allocated. */
  GCSize threshold;	/* Memory threshold. */
  uint8_t currentwhite;	/* Current white color. */
  uint8_t state;	/* GC state. */
  uint8_t unused0;
#if LJ_64
  uint8_t lightudnum;	/* Number of lightuserdata segments - 1. */
#else
  uint8_t unused1;
#endif
  MSize sweepstr;	/* Sweep position in string table. */
  GCRef root;		/* List of all collectable objects. */
  MRef sweep;		/* Sweep position in root list. */
  GCRef gray;		/* List of gray objects. */
  GCRef grayagain;	/* List of objects for atomic traversal. */
  GCRef weak;		/* List of weak tables (to be cleared). */
  GCSize debt;		/* Debt (how much GC is behind schedule). */
  GCSize estimate;	/* Estimate of memory actually in use. */
  MSize stepmul;	/* Incremental GC step granularity. */
  MSize pause;		/* Pause between successive GC cycles. */
#if LJ_64
  MRef lightudseg;	/* Upper bits of lightuserdata segments. */
#endif
} GCState;

/* String interning state. */
typedef struct StrInternState {
  StrTabHdr *tabh;	/* String hash table header and anchors. */
  StrTabHdr *retired;	/* Retired table headers kept until state close. */
  MSize mask;		/* Mirror of tabh->mask for existing fast paths. */
  MSize num;		/* Number of strings in hash table. */
  StrID id;		/* Next string ID. */
  uint8_t idreseed;	/* String ID reseed counter. */
  uint8_t second;	/* String interning table uses secondary hashing. */
  uint8_t unused1;
  uint8_t unused2;
  LJ_ALIGN(8) uint64_t seed;	/* Random string seed. */
} StrInternState;

typedef struct TabState {
  TabNodeRetire *retired_nodes;  /* Retired hash vectors awaiting SMR. */
  TabArrayRetire *retired_arrays;  /* Retired array vectors awaiting SMR. */
} TabState;

#define LJ_GC2_HS_LATENCY_BUCKETS 48
#define LJ_GC2_WORKER_MAX 2

typedef struct TGState TGState;
typedef struct LJThreadLive LJThreadLive;
typedef struct GC2SSBNode GC2SSBNode;
typedef struct GC2State {
  uint32_t phase;	/* LJ_GC2_*; authoritative scaffold phase. */
  uint32_t cycle;	/* Monotonically increasing legacy cycle id. */
  uint32_t cycle_leader;  /* Nonblocking token for requested cycle leader. */
  uint64_t hs_epoch;	/* Soft-handshake generation. */
  uint32_t hs_pending;	/* Outstanding handshake acknowledgements. */
  uint32_t hs_actions;	/* Current LJ_GC2_HS_* action bits. */
  uint64_t hs_signal_ns;  /* Current handshake publication timestamp. */
  uint64_t hs_ack_latency_samples;  /* Safepoint ack latency samples. */
  uint64_t hs_ack_latency_sum_ns;  /* Total safepoint ack latency. */
  uint64_t hs_ack_latency_max_ns;  /* Max safepoint ack latency. */
  uint64_t hs_ack_latency_buckets[LJ_GC2_HS_LATENCY_BUCKETS];
  uint64_t smr_reclaim_runs;  /* Retired-object epoch drains with work. */
  uint64_t smr_reclaimed;  /* Retired objects freed after a grace period. */
  uint64_t cycle_requests;  /* Allocation-triggered cycle requests. */
  uint64_t cycle_starts;  /* Requested cycles consumed at mark begin. */
  uint64_t major_cycle_starts;  /* Actual major GC2 mark begins. */
  uint64_t minor_cycle_requests;  /* Generational minor requests seen. */
  uint64_t minor_cycle_starts;  /* Actual fully-minor GC2 mark begins. */
  uint32_t cycle_minor_requested;  /* Current cycle requested minor mode. */
  uint32_t cycle_sweep_minor;  /* Current cycle uses minor sweep identity. */
  uint32_t minor_sweep_enabled;  /* Public gate for minor sweep identity. */
  uint32_t cycle_roots_minor;  /* Current cycle may use minor root set. */
  uint32_t minor_roots_enabled;  /* Public gate for minor root selection. */
  uint64_t minor_sweep_deferred;  /* Minor requests kept on major sweep. */
  uint64_t minor_sweep_arenas;  /* Arenas swept with minor identity. */
  uint64_t minor_roots_deferred;  /* Minor requests kept on full roots. */
  uint64_t major_root_scans;  /* Full/global root scans selected. */
  uint64_t minor_root_scans;  /* Minor root scans selected. */
  uint64_t minor_survival_base_live;  /* Previous live estimate for survival. */
  uint64_t minor_survival_bytes;  /* Last estimated young bytes kept by minor. */
  uint32_t minor_survival_pct;  /* Last minor survival percentage. */
  uint32_t minor_survival_threshold_pct;  /* Survival pct forcing a major. */
  uint64_t minor_survival_major_requests;  /* High-survival major requests. */
  uint32_t force_major;  /* One-shot full-GC major-cycle override. */
  uint64_t remembered_barriers;  /* Idle generational barriers observed. */
  uint64_t remembered_pushed;  /* Idle remembered entries queued. */
  uint64_t remembered_overflows;  /* Remembered SSB overflows forcing major. */
  uint64_t remembered_filtered;  /* Remembered pairs rejected by age filter. */
  uint64_t remembered_drained;  /* Remembered entries consumed by minor starts. */
  uint64_t marks_this_round;  /* New arena/HugeTab marks this round. */
  GC2SSBNode *ssb_head;	/* Published mutator SSB buffers. */
  uint32_t ssb_published;  /* Published SSB node count. */
  uint32_t ssb_drained;	/* Drained/recycled SSB node count. */
  uint64_t ssb_items_published;  /* Published SSB entries. */
  uint64_t ssb_items_drained;  /* Drained/recycled SSB entries. */
  uint64_t fixpoint_rounds;  /* Bounded mark fixpoint round attempts. */
  uint64_t fixpoint_hits;  /* Rounds ending at zero-mark empty work. */
  uint64_t mark_complete_runs;  /* Final mark completion attempts. */
  uint64_t mark_complete_hits;  /* Final mark completion reached fixpoint. */
  uint64_t mark_complete_peer_waits;  /* Waits for active peer mark drains. */
  uint64_t mark_to_weak;  /* MARK-to-WEAK phase publications. */
  uint64_t weak_complete_runs;  /* P_WEAK completion attempts. */
  uint64_t weak_complete_progress;  /* Worker progress during P_WEAK finish. */
  uint64_t weak_to_sweep;  /* WEAK-to-SWEEP phase publications. */
  uint64_t sweep_to_idle;  /* SWEEP-to-IDLE phase publications. */
  uint64_t preserve_abort_to_idle;  /* Preserve aborts leaving an active phase. */
  uint64_t alloc_since_trigger;  /* Flushed mutator allocation bytes. */
  uint64_t cycle_alloc_bytes;  /* Flushed allocation bytes at cycle start. */
  uint64_t trigger_bytes;  /* Allocation bytes before next GC2 trigger. */
  uint64_t hard_bytes;	/* Allocation bytes before mutator assists. */
  uint64_t assist_runs;  /* Mutator assist attempts past hard limit. */
  uint64_t assist_grey_drained;  /* Grey objects traced by assists. */
  uint64_t assist_ssb_converted;  /* SSB entries converted by assists. */
  uint64_t assist_weak_drained;  /* Weak tables clear-scanned by assists. */
  uint64_t jit_hard_checks;  /* Trace GC checks entered past hard limit. */
  uint64_t interp_hard_checks;  /* Interpreter GC checks past hard limit. */
  uint64_t jit_scoped_slots_retired;  /* Scoped flush trace slots retired. */
  uint32_t gcpause_pct;	/* GC2 pacing percentage. */
  uint32_t assist_shift;  /* Bounded assist work is 1 << shift. */
  uint32_t assist_active;  /* Nonblocking owner token for mark assists. */
  uint32_t generational;  /* M10: requested generational mode. */
  GCRef *grey_stack;	/* GC2 grey work deque ring. */
  MSize grey_capacity;	/* Allocated grey deque slots. */
  uint64_t grey_top;	/* Chase-Lev steal-side index. */
  uint64_t grey_bottom;	/* Chase-Lev owner-side index. */
  uint64_t grey_pushed;	/* Grey entries scheduled from SSB/traversal. */
  uint64_t grey_drained;  /* Grey entries popped for traversal. */
  void *worker_thread[LJ_GC2_WORKER_MAX];  /* Opaque LJThr* parked workers. */
  uint32_t n_workers;	/* Parked GC workers started for this state. */
  uint32_t worker_stop;  /* Request parked worker shutdown. */
  uint32_t worker_wake;  /* Futex word for worker wakeups. */
  uint32_t worker_started;  /* Workers that have entered their loops. */
  uint32_t worker_exited;  /* Workers that have left their loops. */
  uint32_t worker_active;  /* Temporary single drain owner token. */
  uint64_t worker_runs;  /* Non-owner worker drain attempts with work. */
  uint64_t worker_grey_drained;  /* Grey objects traced by workers. */
  uint64_t worker_ssb_converted;  /* SSB entries converted by workers. */
  uint64_t worker_weak_drained;  /* Weak tables clear-scanned by workers. */
  uint64_t worker_idle_declares;  /* Owned worker passes with no progress. */
  uint64_t worker_busy_retries;  /* Worker attempts that found active owner. */
  uint64_t worker_wakes;  /* Parked worker wake publications. */
  uint64_t worker_parks;  /* Parked worker sleeps after no progress. */
  uint64_t worker_async_progress;  /* Work completed by parked workers. */
  uint64_t tg_thread_roots;  /* Live TG thread_L roots marked by GC2. */
  uint64_t tg_cur_roots;  /* Live TG cur_L roots marked by GC2. */
  uint64_t tg_trace_roots;  /* Live TG executing traces marked by GC2. */
  uint64_t thread_scan_claims;  /* Suspended thread stacks claimed by GC2. */
  uint64_t thread_scan_busy;  /* Thread stacks deferred to running owners. */
  uint64_t thread_scan_requeues;  /* Busy suspended threads kept grey. */
  uint64_t thread_scan_owner_scans;  /* Busy stacks covered by owner scans. */
  uint64_t thread_scan_needscan;  /* Busy stacks handed to owning TG scan. */
  uint64_t thread_scan_owner_needscans;  /* Pending owned stacks scanned. */
  uint64_t thread_scan_dirty_misses;  /* Same-cycle scans rejected as stale. */
  uint64_t sweep_owner_runs;  /* Owner traversable arena sweep batches. */
  uint64_t sweep_owner_arenas;  /* Traversable arenas swept by owner. */
  uint64_t sweep_owner_live_cells;  /* Post-sweep live cells observed. */
  uint64_t sweep_live_updates;  /* Sweep-closure live estimate refreshes. */
  uint64_t sweep_live_huge_bytes;  /* Marked traversable huge bytes observed. */
  uint64_t live_estimate;  /* GC2 live bytes from swept traversable memory. */
  GCRef *weak_stack;	/* GC2-owned weak-table discovery vector. */
  uint8_t *weak_ready;	/* Published weak discovery slots. */
  MSize weak_capacity;	/* Allocated weak discovery slots. */
  uint64_t weak_count;	/* Weak discovery slots reserved this cycle. */
  uint64_t weak_tables_seen;  /* Weak table traversals found by GC2. */
  uint64_t weak_tables_weakkey;  /* Weak-key table traversals. */
  uint64_t weak_tables_weakval;  /* Weak-value table traversals. */
  uint64_t weak_tables_allweak;  /* Weak key+value table traversals. */
  uint64_t weak_tables_queued;  /* Weak tables stored in GC2 vector. */
  uint64_t weak_tables_overflow;  /* Weak discoveries beyond vector capacity. */
  uint64_t weak_scan_cursor;  /* Next weak snapshot table to scan. */
  uint64_t weak_scan_runs;  /* Weak snapshot scan attempts with work. */
  uint64_t weak_scan_tables;  /* Weak snapshot tables scanned. */
  uint64_t weak_scan_slots;  /* Weak snapshot entries inspected. */
  uint64_t weak_scan_clearable;  /* Entries that match weak clear rules. */
  uint64_t weak_clear_cursor;  /* Next weak snapshot table to clear. */
  uint64_t weak_clear_runs;  /* Weak snapshot clear attempts with work. */
  uint64_t weak_clear_tables;  /* Weak snapshot tables clear-scanned. */
  uint64_t weak_clear_slots;  /* Weak snapshot clear entries inspected. */
  uint64_t weak_clear_cleared;  /* Weak entries cleared by GC2. */
  uint64_t weak_legacy_skipped;  /* Legacy weak pass skipped after coverage. */
  uint64_t weak_legacy_fallbacks;  /* Legacy weak pass fallback executions. */
  uint64_t weak_legacy_backfills;  /* Legacy weak gaps cleared by GC2 owner. */
  uint64_t weak_legacy_backfill_tables;  /* Missing legacy weak tables cleared. */
  uint64_t weak_legacy_backfill_slots;  /* Backfilled weak entries inspected. */
  uint64_t weak_legacy_backfill_cleared;  /* Backfilled weak entries cleared. */
  uint64_t finreg_cdata_sets;  /* Cdata finalizer registrations mirrored. */
  uint64_t finreg_cdata_clears;  /* Cdata finalizer clears mirrored. */
  uint64_t finreg_cdata_queued;  /* Cdata finalizers queued from FINREG. */
  uint64_t finreg_cdata_sweep_queued;  /* Cdata queued during sweep/free. */
  uint64_t finreg_cdata_pweak_queued;  /* Cdata queued during P_WEAK. */
  GCRef *finreg_cdata_preclaim_obj;  /* P_WEAK claimed cdata queue objs. */
  TValue *finreg_cdata_preclaim_fin;  /* P_WEAK claimed finalizer values. */
  MSize finreg_cdata_preclaim_capacity;  /* Claimed cdata queue slots. */
  MSize finreg_cdata_preclaim_head;  /* Next claimed cdata record to drain. */
  MSize finreg_cdata_preclaim_count;  /* One-past-last claimed record. */
  uint64_t finreg_cdata_pweak_claimed;  /* Cdata finalizers claimed in P_WEAK. */
  uint64_t finreg_cdata_preclaim_overflow;  /* Claimed queue full fallbacks. */
  uint64_t finreg_cdata_preclaim_dispatched;  /* Claimed callbacks consumed. */
  uint64_t finreg_cdata_order_seen;  /* Ordered FINREG nodes inspected. */
  uint64_t finreg_cdata_order_claimed;  /* Ordered FINREG slots claimed. */
  uint64_t finreg_cdata_order_unlinked;  /* Ordered cdata unlinked from root. */
  uint64_t finreg_cdata_order_queued;  /* Ordered cdata queued from FINREG. */
  uint64_t finreg_cdata_order_tombstones;  /* Dead ordered FINREG records. */
  uint64_t finreg_cdata_order_fallbacks;  /* Ordered scan fallback cases. */
  uint64_t finreg_cdata_pending_order_hits;  /* Ordered pending positives. */
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  uint32_t finreg_cdata_preclaim_test_fail;  /* Test-only preclaim failures. */
  uint32_t finreg_cdata_preclaim_publish_pause;
  uint32_t finreg_cdata_preclaim_publish_paused;
  uint32_t finreg_cdata_preclaim_publish_release;
#endif
  uint64_t finreg_udata_sets;  /* Userdata finalizer registrations mirrored. */
  uint64_t finreg_udata_clears;  /* Userdata finalizer clears mirrored. */
  uint64_t finreg_udata_queued;  /* Userdata finalizers queued for dispatch. */
  void *finreg_udata_head;  /* GC2-owned userdata metatable side list. */
  void *finreg_udata_retired;  /* Unlinked nodes retained until teardown. */
  uint64_t finreg_udata_registered;  /* Userdata side-list nodes published. */
  uint64_t finreg_udata_retired_nodes;  /* Userdata side-list nodes unlinked. */
  uint64_t finreg_udata_discovered;  /* Userdata queued from side list. */
  uint64_t finreg_udata_forgets;  /* Stale userdata side-list refs cleared. */
  void *finalizer_mpsc;  /* Producer-published finalizer stack. */
  void *finalizer_tail;  /* Single-consumer finalizer ring tail. */
  uint32_t finalizer_active;  /* Finalizer callbacks currently executing. */
  uint32_t finalizer_owner_tid;  /* TG allowed to finish nested finalizer GC. */
  uint64_t finalizer_queued;  /* Objects published to the GC2 finalizer queue. */
  uint64_t finalizer_dequeued;  /* Objects popped from the GC2 finalizer queue. */
  uint64_t finalizer_mpsc_drained;  /* Objects drained from producer stack. */
  uint64_t finalizer_enters;  /* Legacy finalizer callback guard enters. */
  uint64_t finalizer_leaves;  /* Legacy finalizer callback guard leaves. */
  uint64_t finalizer_sweep_blocks;  /* Sweep attempts blocked by finalizers. */
  uint64_t finalizer_spawn_deferrals;  /* Live spawned TG kept finalize open. */
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  uint32_t finalizer_drain_test_pause;  /* Test hook: pause one drain splice. */
  uint32_t finalizer_drain_test_paused;
  uint32_t finalizer_drain_test_release;
#endif
  uint64_t weak_keys_marked;  /* P_WEAK write barriers marking keys. */
  uint64_t weak_values_marked;  /* P_WEAK write barriers marking values. */
  TGState *tg_list;	/* Registered per-thread state blocks. */
  uint32_t n_threads;	/* Number of registered TG blocks. */
} GC2State;
#if LJ_HASJIT
typedef struct jit_State jit_State;
#endif

/* Global state, shared by all threads of a Lua universe. */
typedef struct global_State {
  lua_Alloc allocf;	/* Memory allocator. */
  void *allocd;		/* Memory allocator data. */
  GCState gc;		/* Garbage collector. */
  GCstr strempty;	/* Empty string. */
  uint8_t stremptyz;	/* Zero terminator of empty string. */
  uint8_t hookmask;	/* Hook mask. */
  uint8_t dispatchmode;	/* Dispatch mode. */
  uint8_t vmevmask;	/* VM event mask. */
  StrInternState str;	/* String interning. */
  TabState tab;		/* Table raw storage retirement. */
  volatile int32_t vmstate;  /* VM state or current JIT code trace number. */
  GCRef mainthref;	/* Link to main thread. */
  SBuf tmpbuf;		/* Temporary string buffer. */
  TValue tmptv, tmptv2;	/* Temporary TValues. */
  TabNodeHdr nilnodehdr;  /* Header for nilnode's empty hash vector. */
  Node nilnode;		/* Fallback 1-element hash part (nil key and value). */
  TValue registrytv;	/* Anchor for registry. */
  GCRef vmthref;	/* Link to VM thread. */
  GCupval uvhead;	/* Head of double-linked list of all open upvalues. */
  int32_t hookcount;	/* Instruction hook countdown. */
  int32_t hookcstart;	/* Start count for instruction hook counter. */
  lua_Hook hookf;	/* Hook function. */
  lua_CFunction wrapf;	/* Wrapper for C function calls. */
  lua_CFunction panic;	/* Called as a last resort for errors. */
  BCIns bc_cfunc_int;	/* Bytecode for internal C function calls. */
  BCIns bc_cfunc_ext;	/* Bytecode for external C function calls. */
  GCRef cur_L;		/* Currently executing lua_State. */
  MRef jit_base;	/* Current JIT code L->base or NULL. */
  MRef ctype_state;	/* Pointer to C type state. */
  PRNGState prng;	/* Global PRNG state. */
  GCRef gcroot[GCROOT_MAX];  /* GC roots. */
#if LJ_HASJIT
  jit_State *jitp;	/* Pointer to the universe-global JIT state. */
  uint32_t jit_token;	/* Recorder token owner tid, 0 if idle. */
  uint32_t jit_mcode_synccore;  /* Sync-core membarrier is registered. */
#endif
  TGState *main_tg;	/* Main per-OS-thread state block. */
  LJThreadLive *threading_live;  /* Lockless threading.thread root list. */
  GC2State gc2;		/* Concurrent GC scaffold state. */
  uint32_t mt_active;	/* One-way latch: secondary Lua threads existed. */
  uint32_t mt_live;	/* Active secondary Lua threads. */
  uint32_t mt_gc_exclusive;  /* Explicit legacy GC excludes secondary entry. */
  uint32_t mt_shutdown;	/* VM teardown is rejecting new secondary threads. */
  GCSize mt_gc_threshold;  /* Saved automatic-GC threshold. */
} global_State;

LJ_STATIC_ASSERT(offsetof(global_State, nilnode) ==
		 offsetof(global_State, nilnodehdr) + sizeof(TabNodeHdr));

#define mainthread(g)	(&gcref(g->mainthref)->th)
#define vmthread(g)	(&gcref(g->vmthref)->th)
#define niltv(L) \
  check_exp(tvisnil(&G(L)->nilnode.val), &G(L)->nilnode.val)
#define niltvg(g) \
  check_exp(tvisnil(&(g)->nilnode.val), &(g)->nilnode.val)

/* Hook management. Hook event masks are defined in lua.h. */
#define HOOK_EVENTMASK		0x0f
#define HOOK_ACTIVE		0x10
#define HOOK_ACTIVE_SHIFT	4
#define HOOK_VMEVENT		0x20
#define HOOK_GC			0x40
#define HOOK_PROFILE		0x80

static LJ_AINLINE uint8_t hookmask_load(global_State *g)
{
  return la_load8_acq(&g->hookmask);  /* 03 section 3.6 global hooks. */
}

static LJ_AINLINE void hookmask_store(global_State *g, uint8_t mask)
{
  la_store8_rel(&g->hookmask, mask);  /* 03 section 3.6 global hooks. */
}

static LJ_AINLINE uint8_t hookmask_update(global_State *g, uint8_t clear,
					  uint8_t set)
{
  uint8_t old = hookmask_load(g);
  for (;;) {
    uint8_t next = (uint8_t)((old & (uint8_t)~clear) | set);
    if (la_cas8(&g->hookmask, &old, next, LA_ACQ_REL, LA_ACQ))
      return next;  /* 03 section 3.6 global hooks. */
  }
}

static LJ_AINLINE int hookmask_set_if_clear(global_State *g, uint8_t blocked,
					    uint8_t set)
{
  uint8_t old = hookmask_load(g);
  for (;;) {
    uint8_t next;
    if ((old & blocked))
      return 0;
    next = (uint8_t)(old | set);
    if (la_cas8(&g->hookmask, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;  /* 03 section 3.6 global hooks. */
  }
}

static LJ_AINLINE uint8_t hookmask_setevents(global_State *g, uint8_t mask)
{
  return hookmask_update(g, HOOK_EVENTMASK, (uint8_t)(mask & HOOK_EVENTMASK));
}

static LJ_AINLINE uint8_t hookmask_restore_(global_State *g, uint8_t h)
{
  uint8_t old = hookmask_load(g);
  h &= (uint8_t)~HOOK_EVENTMASK;
  for (;;) {
    uint8_t next = (uint8_t)((old & HOOK_EVENTMASK) | h);
    if (la_cas8(&g->hookmask, &old, next, LA_ACQ_REL, LA_ACQ))
      return next;  /* 03 section 3.6 global hooks. */
  }
}

#define hook_active(g)		(hookmask_load((g)) & HOOK_ACTIVE)
#define hook_enter(g)		((void)hookmask_update((g), 0, HOOK_ACTIVE))
#define hook_entergc(g) \
  ((void)hookmask_update((g), HOOK_PROFILE, HOOK_ACTIVE|HOOK_GC))
#define hook_vmevent(g) \
  ((void)hookmask_update((g), 0, HOOK_ACTIVE|HOOK_VMEVENT))
#define hook_leave(g)		((void)hookmask_update((g), HOOK_ACTIVE, 0))
#define hook_save(g)		(hookmask_load((g)) & (uint8_t)~HOOK_EVENTMASK)
#define hook_restore(g, h) \
  ((void)hookmask_restore_((g), (h)))

static LJ_AINLINE lua_Hook hookf_load(global_State *g)
{
  return __atomic_load_n(&g->hookf, LA_ACQ);  /* 03 section 3.6 global hooks. */
}

static LJ_AINLINE void hookf_store(global_State *g, lua_Hook hookf)
{
  __atomic_store_n(&g->hookf, hookf, LA_REL);  /* 03 section 3.6 global hooks. */
}

static LJ_AINLINE int32_t hookcount_load(global_State *g)
{
  /* 03 section 3.6 global hooks. */
  return (int32_t)la_load32_acq((uint32_t *)&g->hookcount);
}

static LJ_AINLINE int32_t hookcstart_load(global_State *g)
{
  /* 03 section 3.6 global hooks. */
  return (int32_t)la_load32_acq((uint32_t *)&g->hookcstart);
}

static LJ_AINLINE void hookcount_store(global_State *g, int32_t count)
{
  /* 03 section 3.6 global hooks. */
  la_store32_rel((uint32_t *)&g->hookcount, (uint32_t)count);
}

static LJ_AINLINE void hookcount_setstart(global_State *g, int32_t count)
{
  /* 03 section 3.6 global hooks. */
  la_store32_rel((uint32_t *)&g->hookcstart, (uint32_t)count);
  hookcount_store(g, count);
}

static LJ_AINLINE void hookcount_reset(global_State *g)
{
  hookcount_store(g, hookcstart_load(g));
}

/* Per-thread state object. */
struct lua_State {
  GCHeader;
  uint8_t dummy_ffid;	/* Fake FF_C for curr_funcisL() on dummy frames. */
  uint8_t status;	/* Thread status. */
  MRef glref;		/* Link to global state. */
  GCRef gclist;		/* GC chain. */
  TValue *base;		/* Base of currently executing function. */
  TValue *top;		/* First free slot in the stack. */
  MRef maxstack;	/* Last free slot in the stack. */
  MRef stack;		/* Stack base. */
  GCRef openupval;	/* List of open upvalues in the stack. */
  GCRef env;		/* Thread environment (table of globals). */
  GCRef mt_thread;	/* threading.thread userdata for this state. */
  void *cframe;		/* End of C stack frame chain. */
  MSize stacksize;	/* True stack size (incl. LJ_STACK_EXTRA). */
  TGState *tg_hint;	/* Owning/running TG block, if attached. */
  uint32_t thr_owner;	/* OS-thread owner tid or claim sentinel. */
  uint64_t scan_epoch;	/* Last stack scan epoch for GC workers. */
  uint64_t scan_dirty_epoch;  /* Owner stack-dirty stamp at last scan. */
};

#define G(L)			(mref(L->glref, global_State))
LJ_FUNC TGState *lj_thr_get_tg(void);
LJ_FUNCA TGState *lj_thr_get_tg_fallback(global_State *g);
#define G2TG(gl)		(lj_thr_get_tg_fallback((gl)))
#define L2TG(L)			((L)->tg_hint ? (L)->tg_hint : G2TG(G(L)))
#define registry(L)		(&G(L)->registrytv)

/* Macros to access the currently executing (Lua) function. */
#if LJ_GC64
#define curr_func(L)		(&gcval(L->base-2)->fn)
#elif LJ_FR2
#define curr_func(L)		(&gcref((L->base-2)->gcr)->fn)
#else
#define curr_func(L)		(&gcref((L->base-1)->fr.func)->fn)
#endif
#define curr_funcisL(L)		(isluafunc(curr_func(L)))
#define curr_proto(L)		(funcproto(curr_func(L)))
#define curr_topL(L)		(L->base + curr_proto(L)->framesize)
#define curr_top(L)		(curr_funcisL(L) ? curr_topL(L) : L->top)

#if defined(LUA_USE_ASSERT) || defined(LUA_USE_APICHECK)
LJ_FUNC_NORET void lj_assert_fail(global_State *g, const char *file, int line,
				  const char *func, const char *fmt, ...);
#endif

/* -- GC object definition and conversions -------------------------------- */

/* GC header for generic access to common fields of GC objects. */
typedef struct GChead {
  GCHeader;
  uint8_t unused1;
  uint8_t unused2;
  GCRef env;
  GCRef gclist;
  GCRef metatable;
} GChead;

/* The env field SHOULD be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, env) == offsetof(GCfuncL, env));
LJ_STATIC_ASSERT(offsetof(GChead, env) == offsetof(GCudata, env));

/* The metatable field MUST be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, metatable) == offsetof(GCtab, metatable));
LJ_STATIC_ASSERT(offsetof(GChead, metatable) == offsetof(GCudata, metatable));

/* The gclist field MUST be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(lua_State, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCproto, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCfuncL, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCtab, gclist));

typedef union GCobj {
  GChead gch;
  GCstr str;
  GCupval uv;
  lua_State th;
  GCproto pt;
  GCfunc fn;
  GCcdata cd;
  GCtab tab;
  GCudata ud;
} GCobj;

static LJ_AINLINE GCRef *lj_obj_gcwref(GCobj *o)
{
  return &o->gch.nextgc;
}

static LJ_AINLINE GCobj *lj_obj_gcw(GCobj *o)
{
  return gcref(o->gch.nextgc);
}

static LJ_AINLINE GCobj *lj_obj_gcw_acq(GCobj *o)
{
  return gcref_acq(o->gch.nextgc);
}

static LJ_AINLINE void lj_obj_setgcw(GCobj *o, GCobj *next)
{
  setgcref(o->gch.nextgc, next);
}

static LJ_AINLINE void lj_obj_setgcwr(GCobj *o, GCRef next)
{
  setgcrefr(o->gch.nextgc, next);
}

static LJ_AINLINE void lj_obj_setgcwnull(GCobj *o)
{
  setgcrefnull(o->gch.nextgc);
}

static LJ_AINLINE GCobj *func_uvptr_acq(const GCfuncL *fn, uint32_t idx)
{
  return gcref_acq(fn->uvptr[idx]);
}

static LJ_AINLINE GCupval *func_uv_acq(const GCfuncL *fn, uint32_t idx)
{
  return &func_uvptr_acq(fn, idx)->uv;
}

static LJ_AINLINE uint8_t lj_obj_gcflags(const GCobj *o)
{
  return o->gch.marked;
}

static LJ_AINLINE uint8_t *lj_obj_gcflags_ref(GCobj *o)
{
  return &o->gch.marked;
}

static LJ_AINLINE void lj_obj_setgcflags(GCobj *o, uint8_t flags)
{
  o->gch.marked = flags;
}

static LJ_AINLINE void lj_obj_addgcflags(GCobj *o, uint8_t flags)
{
  o->gch.marked |= flags;
}

static LJ_AINLINE void lj_obj_addgcflags_atomic(GCobj *o, uint8_t flags)
{
  uint8_t old = la_load8_acq(&o->gch.marked);
  for (;;) {
    uint8_t next = (uint8_t)(old | flags);
    if (la_cas8(&o->gch.marked, &old, next, LA_ACQ_REL, LA_ACQ))
      return;
  }
}

static LJ_AINLINE void lj_obj_cleargcflags(GCobj *o, uint8_t flags)
{
  o->gch.marked &= (uint8_t)~flags;
}

static LJ_AINLINE void lj_obj_cleargcflags_atomic(GCobj *o, uint8_t flags)
{
  uint8_t old = la_load8_acq(&o->gch.marked);
  for (;;) {
    uint8_t next = (uint8_t)(old & (uint8_t)~flags);
    if (la_cas8(&o->gch.marked, &old, next, LA_ACQ_REL, LA_ACQ))
      return;
  }
}

static LJ_AINLINE void lj_obj_xorgcflags(GCobj *o, uint8_t flags)
{
  o->gch.marked ^= flags;
}

static LJ_AINLINE void lj_obj_masksetgcflags(GCobj *o, uint8_t clear,
					     uint8_t set)
{
  o->gch.marked = (uint8_t)((o->gch.marked & (uint8_t)~clear) | set);
}

LJ_STATIC_ASSERT(sizeof(GCRef) == 8u);
LJ_STATIC_ASSERT(offsetof(GChead, nextgc) == 0u);
LJ_STATIC_ASSERT(offsetof(GChead, marked) == sizeof(GCRef));
LJ_STATIC_ASSERT(offsetof(GChead, gct) == sizeof(GCRef) + 1u);
LJ_STATIC_ASSERT(offsetof(GChead, marked) == offsetof(GCstr, marked));
LJ_STATIC_ASSERT(offsetof(GChead, marked) == offsetof(GCtab, marked));
LJ_STATIC_ASSERT(offsetof(GChead, marked) == offsetof(GCupval, marked));
LJ_STATIC_ASSERT(((int)offsetof(GCupval, marked) -
		  (int)offsetof(GCupval, tv)) == -8);

/* Macros to convert a GCobj pointer into a specific value. */
#define gco2str(o)	check_exp((o)->gch.gct == ~LJ_TSTR, &(o)->str)
#define gco2uv(o)	check_exp((o)->gch.gct == ~LJ_TUPVAL, &(o)->uv)
#define gco2th(o)	check_exp((o)->gch.gct == ~LJ_TTHREAD, &(o)->th)
#define gco2pt(o)	check_exp((o)->gch.gct == ~LJ_TPROTO, &(o)->pt)
#define gco2func(o)	check_exp((o)->gch.gct == ~LJ_TFUNC, &(o)->fn)
#define gco2cd(o)	check_exp((o)->gch.gct == ~LJ_TCDATA, &(o)->cd)
#define gco2tab(o)	check_exp((o)->gch.gct == ~LJ_TTAB, &(o)->tab)
#define gco2ud(o)	check_exp((o)->gch.gct == ~LJ_TUDATA, &(o)->ud)

/* Macro to convert any collectable object into a GCobj pointer. */
#define obj2gco(v)	((GCobj *)(v))

static LJ_AINLINE lua_State *mainthread_acq(global_State *g)
{
  GCobj *o = gcref_acq(g->mainthref);
  return o ? gco2th(o) : NULL;
}

static LJ_AINLINE lua_State *vmthread_acq(global_State *g)
{
  GCobj *o = gcref_acq(g->vmthref);
  return o ? gco2th(o) : NULL;
}

#if LJ_GC64
static LJ_AINLINE void setgcrefrel_(GCRef *r, const GCobj *gc)
{
  la_store64_rel(&r->gcptr64, (uint64_t)(uintptr_t)gc);
}
static LJ_AINLINE void setgcrefrrel_(GCRef *r, GCRef v)
{
  la_store64_rel(&r->gcptr64, v.gcptr64);
}
static LJ_AINLINE void setgcrefnullrel_(GCRef *r)
{
  la_store64_rel(&r->gcptr64, 0);
}
#else
static LJ_AINLINE void setgcrefrel_(GCRef *r, const GCobj *gc)
{
  la_store32_rel(&r->gcptr32, (uint32_t)(uintptr_t)gc);
}
static LJ_AINLINE void setgcrefrrel_(GCRef *r, GCRef v)
{
  la_store32_rel(&r->gcptr32, v.gcptr32);
}
static LJ_AINLINE void setgcrefnullrel_(GCRef *r)
{
  la_store32_rel(&r->gcptr32, 0);
}
#endif
#define setgcrefrel(r, gc)	setgcrefrel_(&(r), (gc))
#define setgcrefrrel(r, v)	setgcrefrrel_(&(r), (v))
#define setgcrefnullrel(r)	setgcrefnullrel_(&(r))
#define setgcrefroot(r, gc)	setgcrefrel((r), (gc))
#define setgcrefmt(r, gc)	setgcrefrel((r), (gc))

static LJ_AINLINE GCobj *gc2_finreg_udata_obj_acq(GC2FinRegUDataNode *node)
{
  return gcref_acq(node->obj);
}

static LJ_AINLINE void gc2_finreg_udata_obj_rel(GC2FinRegUDataNode *node,
						GCobj *o)
{
  setgcrefrel(node->obj, o);
}

static LJ_AINLINE void gc2_finreg_udata_obj_clear(GC2FinRegUDataNode *node)
{
  setgcrefnullrel(node->obj);
}

static LJ_AINLINE uint32_t
gc2_finreg_udata_active_acq(const GC2FinRegUDataNode *node)
{
  return la_load32_acq(&node->active);
}

static LJ_AINLINE void gc2_finreg_udata_active_rel(GC2FinRegUDataNode *node,
						   uint32_t active)
{
  la_store32_rel(&node->active, active);
}

static LJ_AINLINE int gc2_finreg_udata_active_retire(GC2FinRegUDataNode *node)
{
  uint32_t old = 1;
  return la_cas32(&node->active, &old, 0, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE GC2FinRegUDataNode *
gc2_finreg_udata_next_acq(const GC2FinRegUDataNode *node)
{
  return (GC2FinRegUDataNode *)la_loadptr_acq((void *const *)&node->next);
}

static LJ_AINLINE void gc2_finreg_udata_next_rel(GC2FinRegUDataNode *node,
						 GC2FinRegUDataNode *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

static LJ_AINLINE int gc2_finreg_udata_next_cas(GC2FinRegUDataNode *node,
						GC2FinRegUDataNode **oldp,
						GC2FinRegUDataNode *next)
{
  return la_casptr((void **)&node->next, (void **)oldp, next,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE GC2FinRegUDataNode *
gc2_finreg_udata_retired_next_acq(const GC2FinRegUDataNode *node)
{
  return (GC2FinRegUDataNode *)la_loadptr_acq(
    (void *const *)&node->retired_next);
}

static LJ_AINLINE void
gc2_finreg_udata_retired_next_rel(GC2FinRegUDataNode *node,
				  GC2FinRegUDataNode *next)
{
  la_storeptr_rel((void **)&node->retired_next, next);
}

static LJ_AINLINE void lj_obj_setgcwrel(GCobj *o, const GCobj *next)
{
  setgcrefrel(o->gch.nextgc, next);
}

static LJ_AINLINE void lj_obj_setgcwrrel(GCobj *o, GCRef next)
{
  setgcrefrrel(o->gch.nextgc, next);
}

static LJ_AINLINE void lj_obj_setgcwnullrel(GCobj *o)
{
  setgcrefnullrel(o->gch.nextgc);
}

static LJ_AINLINE GCupval *lj_uv_prev_acq(const GCupval *uv)
{
  return &gcref_acq(uv->prev)->uv;
}

static LJ_AINLINE GCupval *lj_uv_next_acq(const GCupval *uv)
{
  return &gcref_acq(uv->next)->uv;
}

static LJ_AINLINE void lj_uv_setprev_rel(GCupval *uv, GCupval *prev)
{
  setgcrefrel(uv->prev, obj2gco(prev));
}

static LJ_AINLINE void lj_uv_setnext_rel(GCupval *uv, GCupval *next)
{
  setgcrefrel(uv->next, obj2gco(next));
}

/* -- TValue getters/setters ---------------------------------------------- */

/* Macros to test types. */
#if LJ_GC64
#define itype(o)	((uint32_t)((o)->it64 >> 47))
#define tvisnil(o)	((o)->it64 == -1)
#else
#define itype(o)	((o)->it)
#define tvisnil(o)	(itype(o) == LJ_TNIL)
#endif
#define tvisfalse(o)	(itype(o) == LJ_TFALSE)
#define tvistrue(o)	(itype(o) == LJ_TTRUE)
#define tvisbool(o)	(tvisfalse(o) || tvistrue(o))
#if LJ_64 && !LJ_GC64
#define tvislightud(o)	(((int32_t)itype(o) >> 15) == -2)
#else
#define tvislightud(o)	(itype(o) == LJ_TLIGHTUD)
#endif
#if LJ_64
#define tvisforward(o)	((o)->u64 == LJ_TFORWARD_BITS)
#define tviskeylock(o)	((o)->u64 == LJ_TKEYLOCK_BITS)
#else
#define tvisforward(o)	0
#define tviskeylock(o)	0
#endif
#define tvistabinternal(o)	(tvisforward(o) || tviskeylock(o))
#define tvisstr(o)	(itype(o) == LJ_TSTR)
#define tvisfunc(o)	(itype(o) == LJ_TFUNC)
#define tvisthread(o)	(itype(o) == LJ_TTHREAD)
#define tvisproto(o)	(itype(o) == LJ_TPROTO)
#define tviscdata(o)	(itype(o) == LJ_TCDATA)
#define tvistab(o)	(itype(o) == LJ_TTAB)
#define tvisudata(o)	(itype(o) == LJ_TUDATA)
#define tvisnumber(o)	(itype(o) <= LJ_TISNUM)
#define tvisint(o)	(LJ_DUALNUM && itype(o) == LJ_TISNUM)
#define tvisnum(o)	(itype(o) < LJ_TISNUM)

static LJ_AINLINE void lj_tv_load_acq(TValue *dst, const TValue *src)
{
  dst->u64 = tv_rawload_acq(src);
}

static LJ_AINLINE void proto_knumtv_load_acq(TValue *dst, const GCproto *pt,
					     MSize idx)
{
  lj_assertX((uintptr_t)idx < (uintptr_t)pt->sizekn,
	     "bad prototype numeric constant index");
  lj_tv_load_acq(dst, &mref(pt->k, TValue)[idx]);
}

static LJ_AINLINE int gc2_finreg_cdata_preclaim_ready(global_State *g)
{
  return g->gc2.finreg_cdata_preclaim_obj != NULL &&
	 g->gc2.finreg_cdata_preclaim_fin != NULL;
}

static LJ_AINLINE GCobj *gc2_finreg_cdata_preclaim_obj_acq(global_State *g,
							   MSize i)
{
  return gcref_acq(g->gc2.finreg_cdata_preclaim_obj[i]);
}

static LJ_AINLINE void gc2_finreg_cdata_preclaim_fin_acq(global_State *g,
							 MSize i, TValue *fin)
{
  lj_tv_load_acq(fin, &g->gc2.finreg_cdata_preclaim_fin[i]);
}

static LJ_AINLINE int lj_tv_isnil_acq(const TValue *src)
{
  TValue tv;
  lj_tv_load_acq(&tv, src);
  return tvisnil(&tv);
}

static LJ_AINLINE int lj_tv_cas(TValue *dst, TValue *expect,
				const TValue *src)
{
  uint64_t old = tv_rawload(expect);
  int ok = la_cas64(&dst->u64, &old, tv_rawload(src), LA_ACQ_REL, LA_ACQ);
  if (!ok)
    tv_rawstore(expect, old);
  return ok;
}

#define tvistruecond(o)	(itype(o) < LJ_TISTRUECOND)
#define tvispri(o)	(itype(o) >= LJ_TISPRI)
#define tvistabud(o)	(itype(o) <= LJ_TISTABUD)  /* && !tvisnum() */
#define tvisgcv(o)	((itype(o) - LJ_TISGCV) > (LJ_TNUMX - LJ_TISGCV))

/* Special macros to test numbers for NaN, +0, -0, +1 and raw equality. */
#define tvisnan(o)	((o)->n != (o)->n)
#if LJ_64
#define tviszero(o)	(((o)->u64 << 1) == 0)
#else
#define tviszero(o)	(((o)->u32.lo | ((o)->u32.hi << 1)) == 0)
#endif
#define tvispzero(o)	((o)->u64 == 0)
#define tvismzero(o)	((o)->u64 == U64x(80000000,00000000))
#define tvispone(o)	((o)->u64 == U64x(3ff00000,00000000))
#define rawnumequal(o1, o2)	((o1)->u64 == (o2)->u64)

/* Macros to convert type ids. */
#if LJ_64 && !LJ_GC64
#define itypemap(o) \
  (tvisnumber(o) ? ~LJ_TNUMX : tvislightud(o) ? ~LJ_TLIGHTUD : ~itype(o))
#else
#define itypemap(o)	(tvisnumber(o) ? ~LJ_TNUMX : ~itype(o))
#endif

/* Macros to get tagged values. */
#if LJ_GC64
#define gcval(o)	((GCobj *)(gcrefu((o)->gcr) & LJ_GCVMASK))
#else
#define gcval(o)	(gcref((o)->gcr))
#endif
#define boolV(o)	check_exp(tvisbool(o), (LJ_TFALSE - itype(o)))
#if LJ_64
#define lightudseg(u) \
  (((u) >> LJ_LIGHTUD_BITS_LO) & ((1 << LJ_LIGHTUD_BITS_SEG)-1))
#define lightudlo(u) \
  ((u) & (((uint64_t)1 << LJ_LIGHTUD_BITS_LO) - 1))
#define lightudup(p) \
  ((uint32_t)(((p) >> LJ_LIGHTUD_BITS_LO) << (LJ_LIGHTUD_BITS_LO-32)))
static LJ_AINLINE void *lightudV(global_State *g, cTValue *o)
{
  uint64_t u = o->u64;
  uint64_t seg = lightudseg(u);
  uint32_t *segmap = mref(g->gc.lightudseg, uint32_t);
  lj_assertG(tvislightud(o), "lightuserdata expected");
  if (seg == (1 << LJ_LIGHTUD_BITS_SEG)-1) return NULL;
  lj_assertG(seg <= g->gc.lightudnum, "bad lightuserdata segment %d", seg);
  return (void *)(((uint64_t)segmap[seg] << 32) | lightudlo(u));
}
#else
#define lightudV(g, o)	check_exp(tvislightud(o), gcrefp((o)->gcr, void))
#endif
#define gcV(o)		check_exp(tvisgcv(o), gcval(o))
#define strV(o)		check_exp(tvisstr(o), &gcval(o)->str)
#define funcV(o)	check_exp(tvisfunc(o), &gcval(o)->fn)
#define threadV(o)	check_exp(tvisthread(o), &gcval(o)->th)
#define protoV(o)	check_exp(tvisproto(o), &gcval(o)->pt)
#define cdataV(o)	check_exp(tviscdata(o), &gcval(o)->cd)
#define tabV(o)		check_exp(tvistab(o), &gcval(o)->tab)
#define udataV(o)	check_exp(tvisudata(o), &gcval(o)->ud)
#define numV(o)		check_exp(tvisnum(o), (o)->n)
#define intV(o)		check_exp(tvisint(o), (int32_t)(o)->i)

/* Macros to set tagged values. */
#if LJ_GC64
#define setitype(o, i)		((o)->it = ((i) << 15))
#define setnilV(o)		tv_rawstore((o), ~(uint64_t)0)
#define setpriV(o, x) \
  tv_rawstore((o), (uint64_t)(int64_t)~((uint64_t)~(x)<<47))
#define setboolV(o, x) \
  tv_rawstore((o), (uint64_t)(int64_t)~((uint64_t)((x)+1)<<47))
#else
#define setitype(o, i)		((o)->it = (i))
#define setnilV(o)		((o)->it = LJ_TNIL)
#define setboolV(o, x)		((o)->it = LJ_TFALSE-(uint32_t)(x))
#define setpriV(o, i)		(setitype((o), (i)))
#endif

static LJ_AINLINE void setrawlightudV(TValue *o, void *p)
{
#if LJ_GC64
  TValue tv;
  tv.u64 = (uint64_t)p | (((uint64_t)LJ_TLIGHTUD) << 47);
  tv_rawstore(o, tv.u64);
#elif LJ_64
  o->u64 = (uint64_t)p | (((uint64_t)0xffff) << 48);
#else
  setgcrefp(o->gcr, p); setitype(o, LJ_TLIGHTUD);
#endif
}

static LJ_AINLINE void setforwardV(TValue *o)
{
#if LJ_64
  tv_rawstore(o, LJ_TFORWARD_BITS);
#else
  setnilV(o);
#endif
}

static LJ_AINLINE void setkeylockV(TValue *o)
{
#if LJ_64
  tv_rawstore(o, LJ_TKEYLOCK_BITS);
#else
  setnilV(o);
#endif
}

#if LJ_FR2 || LJ_32
#define contptr(f)		((void *)(f))
#define setcont(o, f)		((o)->u64 = (uint64_t)(uintptr_t)contptr(f))
#else
#define contptr(f) \
  ((void *)(uintptr_t)(uint32_t)((intptr_t)(f) - (intptr_t)lj_vm_asm_begin))
#define setcont(o, f) \
  ((o)->u64 = (uint64_t)(void *)(f) - (uint64_t)lj_vm_asm_begin)
#endif

static LJ_AINLINE void checklivetv(lua_State *L, TValue *o, const char *msg)
{
  UNUSED(L); UNUSED(o); UNUSED(msg);
#if LUA_USE_ASSERT
  if (tvisgcv(o)) {
    lj_assertL(~itype(o) == gcval(o)->gch.gct,
	       "mismatch of TValue type %d vs GC type %d",
	       ~itype(o), gcval(o)->gch.gct);
    /* Copy of isdead check from lj_gc.h to avoid circular include. */
    lj_assertL(!(gcval(o)->gch.marked & (G(L)->gc.currentwhite ^ 3) & 3), msg);
  }
#endif
}

static LJ_AINLINE void setgcVraw(TValue *o, GCobj *v, uint32_t itype)
{
#if LJ_GC64
  TValue tv;
  setgcreft(tv.gcr, v, itype);
  tv_rawstore(o, tv.u64);
#else
  setgcref(o->gcr, v); setitype(o, itype);
#endif
}

static LJ_AINLINE void setgcV(lua_State *L, TValue *o, GCobj *v, uint32_t it)
{
  setgcVraw(o, v, it);
  checklivetv(L, o, "store to dead GC object");
}

#define define_setV(name, type, tag) \
static LJ_AINLINE void name(lua_State *L, TValue *o, const type *v) \
{ \
  setgcV(L, o, obj2gco(v), tag); \
}
define_setV(setstrV, GCstr, LJ_TSTR)
define_setV(setthreadV, lua_State, LJ_TTHREAD)
define_setV(setprotoV, GCproto, LJ_TPROTO)
define_setV(setfuncV, GCfunc, LJ_TFUNC)
define_setV(setcdataV, GCcdata, LJ_TCDATA)
define_setV(settabV, GCtab, LJ_TTAB)
define_setV(setudataV, GCudata, LJ_TUDATA)

static LJ_AINLINE void setnumV(TValue *o, lua_Number x)
{
  TValue tv;
  tv.n = x;
  tv_rawstore(o, tv.u64);
}
#define setnanV(o)		tv_rawstore((o), U64x(fff80000,00000000))
#define setpinfV(o)		tv_rawstore((o), U64x(7ff00000,00000000))
#define setminfV(o)		tv_rawstore((o), U64x(fff00000,00000000))

static LJ_AINLINE void setintV(TValue *o, int32_t i)
{
#if LJ_DUALNUM
  TValue tv;
  tv.i = (uint32_t)i; setitype(&tv, LJ_TISNUM);
  tv_rawstore(o, tv.u64);
#else
  setnumV(o, (lua_Number)i);
#endif
}

static LJ_AINLINE void setint64V(TValue *o, int64_t i)
{
  if (LJ_DUALNUM && LJ_LIKELY(i == (int64_t)(int32_t)i))
    setintV(o, (int32_t)i);
  else
    setnumV(o, (lua_Number)i);
}

#if LJ_64
#define setintptrV(o, i)	setint64V((o), (i))
#else
#define setintptrV(o, i)	setintV((o), (i))
#endif

/* Copy tagged values. */
static LJ_AINLINE void copyTV(lua_State *L, TValue *o1, const TValue *o2)
{
  tv_rawstore(o1, tv_rawload(o2));
  checklivetv(L, o1, "copy of dead GC object");
}

static LJ_AINLINE void copyTVrel(lua_State *L, TValue *o1, const TValue *o2)
{
  tv_rawstore_rel(o1, tv_rawload(o2));
  checklivetv(L, o1, "copy of dead GC object");
}

/* -- Number to integer conversion ---------------------------------------- */

/*
** The C standard leaves many aspects of FP to integer conversions as
** undefined behavior. Portability is a mess, hardware support varies,
** and modern C compilers are like a box of chocolates -- you never know
** what you're gonna get.
**
** However, we need 100% matching behavior between the interpreter (asm + C),
** optimizations (C) and the code generated by the JIT compiler (asm).
** Mixing Lua numbers with FFI numbers creates some extra requirements.
**
** These conversions have been moved to assembler code, even if they seem
** trivial, to foil unanticipated C compiler 'optimizations' with the
** surrounding code. Only the unchecked double to int32_t conversion
** is still in C, because it ought to be pretty safe -- we'll see.
**
** These macros also serve to document all places where FP to integer
** conversions happen.
*/

/* Unchecked double to int32_t conversion. */
#define lj_num2int(n)		((int32_t)(n))

/* Unchecked double to arch/os-dependent signed integer type conversion.
** This assumes the 32/64-bit signed conversions are NOT range-extended.
*/
#define lj_num2int_type(n, tp)	((tp)(n))

/* Convert a double to int32_t and check for exact conversion.
** Returns the zero-extended int32_t on success. -0 is OK, too.
** Returns 0x8000000080000000LL on failure (simplifies range checks).
*/
LJ_ASMF LJ_CONSTF int64_t lj_vm_num2int_check(double x);

/* Check for exact conversion only, without storing the result. */
#define lj_num2int_ok(x)	(lj_vm_num2int_check((x)) >= 0)

/* Check for exact conversion and conditionally store result.
** Note: conditions that fail for 0x80000000 may check only the lower
** 32 bits. This generates good code for both 32 and 64 bit archs.
*/
#define lj_num2int_cond(x, i64, i, cond) \
  (i64 = lj_vm_num2int_check((x)), cond ? (i = (int32_t)i64, 1) : 0)

/* This is the generic check for a full-range int32_t result. */
#define lj_num2int_check(x, i64, i) \
  lj_num2int_cond((x), i64, i, i64 >= 0)

/* Predictable conversion from double to int64_t or uint64_t.
** Truncates towards zero. Out-of-range values, NaN and +-Inf return
** an arch-dependent result, but do not cause C undefined behavior.
** The uint64_t conversion accepts the union of the unsigned + signed range.
*/
LJ_ASMF LJ_CONSTF int64_t lj_vm_num2i64(double x);
LJ_ASMF LJ_CONSTF int64_t lj_vm_num2u64(double x);

#define lj_num2i64(x)		(lj_vm_num2i64((x)))
#define lj_num2u64(x)		(lj_vm_num2u64((x)))

/* Lua BitOp conversion semantics use the 2^52 + 2^51 trick. */
LJ_ASMF LJ_CONSTF int32_t lj_vm_tobit(double x);

#define lj_num2bit(x)	lj_vm_tobit((x))

static LJ_AINLINE int32_t numberVint(cTValue *o)
{
  if (LJ_LIKELY(tvisint(o)))
    return intV(o);
  else
    return lj_num2int(numV(o));
}

static LJ_AINLINE lua_Number numberVnum(cTValue *o)
{
  if (LJ_UNLIKELY(tvisint(o)))
    return (lua_Number)intV(o);
  else
    return numV(o);
}

/* -- Miscellaneous object handling --------------------------------------- */

/* Names and maps for internal and external object tags. */
LJ_DATA const char *const lj_obj_typename[1+LUA_TCDATA+1];
LJ_DATA const char *const lj_obj_itypename[~LJ_TNUMX+1];

#define lj_typename(o)	(lj_obj_itypename[itypemap(o)])

/* Compare two objects without calling metamethods. */
LJ_FUNC int LJ_FASTCALL lj_obj_equal(cTValue *o1, cTValue *o2);
LJ_FUNC const void * LJ_FASTCALL lj_obj_ptr(global_State *g, cTValue *o);

#if LJ_ABI_PAUTH
#if LJ_TARGET_ARM64
#include <ptrauth.h>
#define lj_ptr_sign(ptr, ctx) \
  ptrauth_sign_unauthenticated((ptr), ptrauth_key_function_pointer, (ctx))
#define lj_ptr_strip(ptr) ptrauth_strip((ptr), ptrauth_key_function_pointer)
#else
#error "No support for pointer authentication for this architecture"
#endif
#else
#define lj_ptr_sign(ptr, ctx) (ptr)
#define lj_ptr_strip(ptr) (ptr)
#endif

#endif
