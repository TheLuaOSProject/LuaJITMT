/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TG_H
#define _LJ_TG_H

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_arena.h"
#include "lj_gc2token.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif

/* Type of hot counter. Must match the code in the assembler VM. */
/* 16 bits are sufficient. Only 0.0015% overhead with maximum slot penalty. */
typedef uint16_t HotCount;

/* Number of hot counter hash table entries (must be a power of two). */
#define HOTCOUNT_SIZE		64
#define HOTCOUNT_PCMASK		((HOTCOUNT_SIZE-1)*sizeof(HotCount))

/* Hotcount decrements. */
#define HOTCOUNT_LOOP		2
#define HOTCOUNT_CALL		1

/* This solves a circular dependency problem -- bump as needed. Sigh. */
#define GG_NUM_ASMFF	57

#define GG_LEN_DDISP	(BC__MAX + GG_NUM_ASMFF)
#define GG_LEN_SDISP	BC__MAX
#define GG_LEN_DISP	(GG_LEN_DDISP + GG_LEN_SDISP)

#define TGF_ARENA_INTERNAL	0x01u
#define TGF_HUGETAB		0x02u
#define TGF_DEAD		0x04u
#define TGF_STOPREQ		0x08u
#define TGF_STOPREQ_FRESH	0x10u
#define TGF_HEAP		0x20u
#define TGF_LUA_ALLOC		0x40u
#define TGF_DEFER_FREE		0x80u
#define TG_HUGETAB_BITS		16u
#define TG_GC2_SSB_BYTES	8192u
#define TG_GC2_SSB_SLOTS	(TG_GC2_SSB_BYTES / sizeof(GCRef))
#define TG_GC2_SSB_DYNAMIC	0x01u
#define TG_GC2_SSB_REMEMBERED_SHIFT 1u
#define TG_ROOT_ANCHOR_SLOTS	16u

#if LJ_HASJIT
/* Two retained raw views let an owner start the next event without waiting for
** a reader of the just-closed event. The growable retained root vector uses
** ordinary GCRefs (with eight inline): an unpublished recorder is never
** disguised as a GCtrace merely to reuse the trace traversal. */
#define LJ_JIT_EVENT_SESSION_SLOTS	2u
#define LJ_JIT_EVENT_SESSION_ROOTS	8u
#define LJ_JIT_EVENT_ATTACHMENT_SLOTS	8u

#define LJ_JIT_EVENT_VIEW_FORMAT_NONE	0u
#define LJ_JIT_EVENT_VIEW_FORMAT_TRACE_V1 1u

#define LJ_JIT_EVENT_SLOT_F_VIEW		0x01u
#define LJ_JIT_EVENT_SLOT_F_SOURCE_PIN	0x02u
#define LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT	0x04u

typedef enum LJJitEventKind {
  LJ_JIT_EVENT_TRACE_START = 1,
  LJ_JIT_EVENT_TRACE_STOP,
  LJ_JIT_EVENT_TRACE_ABORT,
  LJ_JIT_EVENT_TRACE_FLUSH,
  LJ_JIT_EVENT_RECORD
} LJJitEventKind;

/* A continuation keeps mutable recorder scratch reserved in the high half of
** jit_owner_word.  A detached immutable event owns only its copied payload and
** exact TG identity, so it may coexist with an unrelated recorder owner. */
typedef enum LJJitEventOwnerMode {
  LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE = 1,
  LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE
} LJJitEventOwnerMode;

/* Proof carried by a frozen view for every GC edge encoded in its raw bytes.
** EXACT_ROOTS is structurally enforced here; decoder equality/range checks are
** a required production-builder follow-up before callback wiring. */
typedef enum LJJitEventEdgeProof {
  LJ_JIT_EVENT_EDGE_NONE = 1,
  LJ_JIT_EVENT_EDGE_PINNED_SOURCE,
  LJ_JIT_EVENT_EDGE_EXACT_ROOTS
} LJJitEventEdgeProof;

typedef enum LJJitEventSlotState {
  LJ_JIT_EVENT_SLOT_FREE,
  LJ_JIT_EVENT_SLOT_BUILDING,
  LJ_JIT_EVENT_SLOT_ACTIVE,
  LJ_JIT_EVENT_SLOT_CLOSED,
  LJ_JIT_EVENT_SLOT_CLEANING
} LJJitEventSlotState;

typedef enum LJJitEventPublicationState {
  LJ_JIT_EVENT_PUBLICATION_IDLE,
  LJ_JIT_EVENT_PUBLICATION_ACTIVE
} LJJitEventPublicationState;

typedef struct LJJitEventFrozenSpan {
  uint32_t offset;
  uint32_t count;
  uint32_t stride;
} LJJitEventFrozenSpan;

#define LJ_JIT_EVENT_FROZEN_TRACE_VERSION 1u

/* Immutable scalar semantics paired with the raw IR/snapshot byte spans. No
** field is a GC header and no pointer here is traversed as a GCtrace. */
typedef struct LJJitEventFrozenTraceHeader {
  uint32_t version;
  uint32_t flags;
  uint32_t traceno;
  uint32_t root;
  uint32_t link;
  uint32_t linktype;
  uint32_t nins;
  uint32_t nk;
  uint32_t nsnap;
  uint32_t nsnapmap;
  uint32_t ir_ref_first;
  uint32_t ir_ref_count;
  uint32_t startpc_pos;
  BCIns startins;
  uint64_t mcode_addr;
  uint32_t szmcode;
  uint32_t mcloop;
  uint64_t exitstub_addr;
  uint32_t nexits;
  uint32_t pad;
} LJJitEventFrozenTraceHeader;

/* This is raw C storage, not a GC object.  `data` owns a byte-for-byte frozen
** recorder view whose three bounded spans are decoded only while a slot reader
** is held.  Reallocation is restricted to an unpublished zero-reader slot. */
typedef struct LJJitEventFrozenView {
  void *data;
  uint32_t capacity;
  uint32_t size;
  uint32_t format;
  uint32_t flags;
  LJJitEventFrozenTraceHeader trace;
  LJJitEventFrozenSpan ir;
  LJJitEventFrozenSpan snap;
  LJJitEventFrozenSpan snapmap;
} LJJitEventFrozenView;

typedef struct LJJitEventSessionSlot {
  uint64_t generation;
  uint32_t state;
  uint32_t readers;
  uint32_t flags;
  uint32_t event;
  uint32_t owner_mode;
  uint32_t edge_proof;
  uint64_t attachment_generation;
  /* Reserved for the later same-owner control-borrow transaction which
  ** temporarily resumes {tid,0} without ending this session. */
  uint32_t control_borrow_state;
  uint64_t control_borrow_generation;
  lua_State *saved_jit_owner_L;
  uint32_t root_count;
  uint32_t owner_tid;
  uint32_t owner_actor;
  lua_State *owner_L;
  GCRef owner_root;  /* Explicit callback-state root; owner_L is identity. */
  struct GCtrace *source;
  uint32_t source_traceno;
  /* Exact VM-event attachment classification. This consumes the original
  ** spare word so every following session/TG offset remains unchanged. */
  uint32_t attachment_state;
  LJJitEventFrozenView view;
  GCRef *root_data;
  uint32_t root_capacity;
  /* Independent 0/1 cardinality for root_data[root_count]. */
  uint32_t callback_root_count;
  /* root_count covers frozen-view proof roots only. The independently flagged
  ** callback handler root is the mandatory reserved lane root_data[root_count]
  ** and is never evidence for LJ_JIT_EVENT_EDGE_EXACT_ROOTS. */
  GCRef root_inline[LJ_JIT_EVENT_SESSION_ROOTS];
} LJJitEventSessionSlot;

typedef struct LJJitEventSessions {
  uint64_t sequence;  /* Even stable, odd owner publication transition. */
  uint64_t next_generation;
  uint64_t active_generation;
  uint32_t state;
  uint32_t active_slot;
  LJJitEventSessionSlot slot[LJ_JIT_EVENT_SESSION_SLOTS];
} LJJitEventSessions;

/* The TRACE event grammar is universe-global even though the immutable event
** payload and its roots are owned by one TG.  This descriptor therefore lives
** in the append-only tail of the embedded main TG; copies in secondary TGs are
** initialized but never consulted.  Every field is accessed atomically under
** the even/odd sequence protocol, so readers refuse rather than wait on an
** in-progress scalar publication. */
typedef enum LJJitTraceStreamPhase {
  LJ_JIT_STREAM_IDLE = 0,
  LJ_JIT_STREAM_OPEN,
  LJ_JIT_STREAM_CONT_CALLBACK,
  LJ_JIT_STREAM_CONT_TERMINAL_PENDING,
  LJ_JIT_STREAM_DETACHED_PENDING,
  LJ_JIT_STREAM_DETACHED_CALLBACK
} LJJitTraceStreamPhase;

typedef struct LJJitTraceStream {
  uint64_t sequence;              /* Even stable, odd scalar publication. */
  uint64_t next_generation;       /* Monotonic; zero is invalid. */
  uint64_t generation;            /* Current stream, or zero when IDLE. */
  uint64_t event_ordinal;         /* Standalone FLUSH is ordinal one. */

  LJTGRegistryKey owner_key;      /* Stable registry slot plus incarnation. */
  uint32_t owner_tid;
  uint32_t owner_actor;
  uint32_t phase;
  uint32_t traceno;

  uint32_t callback_event;
  uint32_t callback_slot;
  uint64_t callback_session_generation;

  uint32_t terminal_event;
  uint32_t terminal_slot;
  uint64_t terminal_session_generation;
  uint32_t terminal_reason;
  uint32_t flags;
} LJJitTraceStream;

/* One universe-global publication clock for each VM-event hash lane.  The
** authoritative array is the copy embedded in g->main_tg; secondary copies
** exist only to keep TG allocation/bootstrap symmetric.  Production
** jit.attach() does not update these dormant clocks yet. */
typedef struct LJJitEventAttachmentClock {
  uint64_t sequence;              /* Even stable, odd scalar publication. */
  uint64_t next_generation;       /* Monotonic; zero is initial-only. */
  uint64_t generation;            /* Exact last publication generation. */
} LJJitEventAttachmentClock;

/* Protected JIT VM-event callbacks are owned per TG.  The owner carries only
** scalar identity: owner_L remains rooted by the exact event session named by
** {session_slot, session_generation}.  Appending this descriptor after every
** previously published TG member preserves all established byte offsets. */
typedef enum LJJitEventCallbackState {
  LJ_JIT_EVENT_CALLBACK_IDLE = 0,
  LJ_JIT_EVENT_CALLBACK_CALLING,
  LJ_JIT_EVENT_CALLBACK_UNWINDING
} LJJitEventCallbackState;

typedef struct LJJitEventCallbackOwner {
  uint64_t sequence;              /* Even stable, odd scalar publication. */
  uint64_t next_generation;       /* Monotonic; zero is initial-only. */
  uint64_t generation;            /* Current callback, or zero when IDLE. */
  uint64_t stream_generation;     /* Exact nonzero JIT TRACE stream. */
  uint64_t session_generation;    /* Exact rooted event session. */
  uint32_t state;                 /* IDLE/CALLING/UNWINDING. */
  uint32_t owner_actor;           /* Exact physical actor. */
  uint32_t event;                 /* LJJitEventKind. */
  uint32_t session_slot;          /* Exact event-session slot. */
  lua_State *owner_L;             /* Comparison identity; rooted by session. */
} LJJitEventCallbackOwner;
#endif

#define TG_FINI_LIVE		0u
#define TG_FINI_BUSY		1u
#define TG_FINI_DONE		2u
#define TG_FINI_RETRY		3u

typedef struct GG_State GG_State;
typedef struct ExitTrampolines ExitTrampolines;
typedef struct TGRootAnchorBlock TGRootAnchorBlock;
struct LexState;

struct TGRootAnchorBlock {
  TGRootAnchorBlock *next;
  TValue slot[TG_ROOT_ANCHOR_SLOTS];
};

struct GC2SSBNode {
  GC2SSBNode *next;
  TGState *owner;
  uint32_t n;
  uint32_t pad;
  GCRef slot[TG_GC2_SSB_SLOTS];
};

struct TGState {
  HotCount hotcount[HOTCOUNT_SIZE];
  ASMFunction dispatch[GG_LEN_DISP];
  uint32_t poll;
  /* SIGPROF publishes this word only. The x64 VM reads {poll,profile_request}
  ** as one aligned qword and enters normal owner context before dispatch is
  ** changed. Keep this field immediately after poll. */
  uint32_t profile_request;
  uint32_t mark_active;
  global_State *gl;
  lua_State *cur_L;
  struct LexState *lexstate;  /* Owner-published parser raw-root descriptor. */
  TValue *jit_base;
  /* Owner-private trace-root state materialized by IR_XSAVE. The future
  ** generic native-enter helper consumes these fields before it release-
  ** publishes a remotely readable native frame. They are deliberately not a
  ** remote publication by themselves.
  */
  TValue *ffi_xsave_root;
  uint32_t ffi_xsave_baseslot;
  uint32_t ffi_xsave_nslots;
  int jit_exitcode;
  int32_t vmstate;
  uint32_t profile_samples;
  int32_t profile_vmstate;
  uint32_t in_native;
  StrTabHdr *strtab_active_hdr;
  uint32_t strtab_active_depth;
  uint64_t strtab_active_epoch;  /* GC2 epoch of outermost table read pin. */
  StrCanonHdr *strq_active_hdr;
  uint32_t strq_active_depth;
  uint64_t strq_active_epoch;  /* GC2 epoch of outermost quarantine pin. */
  uint32_t tab_read_depth;
  uint64_t tab_read_epoch;  /* GC2 epoch of outermost long table-vector read. */
  StrID strid_next;
  StrID strid_end;
  uint32_t strnum_credit;  /* Unused string-count reservations. */
  uint8_t gc_assist;
  uint8_t hookmask_th;
  uint8_t tg_flags;
  uint8_t fini_state;  /* Physical allocator finalization ownership. */
  uint32_t reqmask;
  uint64_t hs_epoch_ack;
  TGAlloc alloc;
  LJArenaAllocD allocd;
  HugeTab huge;
  GC2SSBNode ssb_node[2];
  GC2SSBNode *ssb_active, *ssb_free;
  GCRef *ssb_next, *ssb_end, *ssb_base;
  uint32_t ssb_refs;  /* Published embedded nodes pin this TG. */
  /* Comparison-only active-MARK FNEW certificate. These raw pointers are not
  ** roots: the exact proto/environment pair is retained by the two SSB slots
  ** which precede publication of fnew_cert_cycle. A zero cycle is invalid. */
  GCproto *fnew_cert_pt;
  GCtab *fnew_cert_env;
  uint32_t fnew_cert_cycle;
  GCobj *gcroot_pending;
  GCobj *gcroot_pending_after_main;
  SBuf tmpbuf;
  TValue tmptv, tmptv2;
  TGRootAnchorBlock root_anchor;
  uint32_t root_anchor_top;
  PRNGState prng;
#if LJ_HASFFI
  void *ffi_call_func;
  CCallbackRuntime cb;
#endif
  lua_State *thread_L;
  GCudata *thread_ud;
  uint32_t tid;
  uint32_t actor_id;  /* Physical actor, zero handoff, or terminal sentinel. */
  TGState *next_tg;
  TGState *worker_retire_next;  /* Worker-TG retirement, outside registry. */
  uint64_t local_total;
  uint64_t stack_dirty_epoch;
  ExitTrampolines *exittr;
  /* Dormant until every root writer and phase edge uses the admission gate. */
  LJGC2RootDesc root_desc;
  /* Stable slot storage is external and survives this TG body. The key names
  ** its one non-reused runtime incarnation until legacy reclaim clears it. */
  LJTGRegistryKey registry_key;
  uint8_t registry_shadow_missed;  /* Legacy-only attach after slot OOM. */
#if LJ_HASFFI
  /* Generic frame publication remains after all pre-existing VM-sensitive TG
  ** fields.  New append-only JIT event metadata follows it below. */
  uint64_t ffi_native_seq;	/* Even stable, odd owner transition. */
  uint32_t ffi_native_depth;
  LJFFINativeFrame ffi_native_frame[LJ_FFI_NATIVE_FRAME_MAX];
#endif
#if LJ_HASJIT
  /* Append-only: no pre-existing VM/ABI-sensitive TG offset may move. */
  LJJitEventSessions jit_event_sessions;
  /* Main-TG storage is the one universe-global TRACE grammar descriptor.
  ** Keeping it after the already-landed session substrate preserves that
  ** substrate's offsets as well as every original LuaJIT offset. */
  LJJitTraceStream jit_trace_stream;
  /* Tail-only dormant attachment clocks.  Do not insert new fields before
  ** this array: the preceding stream descriptor now has published offsets. */
  LJJitEventAttachmentClock
    jit_event_attachment[LJ_JIT_EVENT_ATTACHMENT_SLOTS];
#endif
  /* Append-only, universe-global when reached through |g->main_tg|.  The
  ** canonical string is fixed during state bootstrap, so this comparison
  ** pointer is not a separate GC edge.  Secondary TG copies remain NULL. */
  GCstr *vmevent_regkey;
#if LJ_HASJIT
  /* Per-TG and tail-only.  Do not move this before vmevent_regkey: that key's
  ** offset is part of the already published attachment/session substrate. */
  LJJitEventCallbackOwner jit_event_callback_owner;
  /* Main-TG storage is the immutable bootstrap copy used by standalone TRACE
  ** FLUSH delivery.  Secondary TG copies remain NULL.  Keeping it after the
  ** callback owner preserves every published event-substrate offset. */
  GCstr *jit_trace_flush_reason;
#endif
};

LJ_STATIC_ASSERT(sizeof(((GC2SSBNode *)0)->slot) == TG_GC2_SSB_BYTES);
LJ_STATIC_ASSERT((offsetof(TGState, poll) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(TGState, profile_request) ==
		 offsetof(TGState, poll) + sizeof(uint32_t));
#if LJ_HASJIT
LJ_STATIC_ASSERT((offsetof(TGState, jit_event_sessions.sequence) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions, next_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions, active_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions, slot[0].generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions,
			  slot[0].attachment_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions,
			  slot[0].control_borrow_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions, slot[1].generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions,
			  slot[1].attachment_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventSessions,
			  slot[1].control_borrow_generation) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, attachment_state) ==
		 offsetof(LJJitEventSessionSlot, source_traceno) +
		 sizeof(((LJJitEventSessionSlot *)0)->source_traceno));
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, view) ==
		 offsetof(LJJitEventSessionSlot, attachment_state) +
		 sizeof(((LJJitEventSessionSlot *)0)->attachment_state));
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, root_inline) ==
		 offsetof(LJJitEventSessionSlot, callback_root_count) +
		 sizeof(((LJJitEventSessionSlot *)0)->callback_root_count));
LJ_STATIC_ASSERT(sizeof(LJJitEventSessionSlot) ==
		 offsetof(LJJitEventSessionSlot, root_inline) +
		 sizeof(((LJJitEventSessionSlot *)0)->root_inline));
#if LJ_64
/* Published x86-64 substrate offsets: both new scalars consume old spare
** words and the callback sentinel consumes vector capacity, not structure
** bytes. Keep downstream stream/clock offsets byte-for-byte stable. */
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, source_traceno) == 104u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, attachment_state) == 108u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, view) == 112u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, root_data) == 264u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, root_capacity) == 272u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, callback_root_count) == 276u);
LJ_STATIC_ASSERT(offsetof(LJJitEventSessionSlot, root_inline) == 280u);
LJ_STATIC_ASSERT(sizeof(LJJitEventSessionSlot) == 344u);
#endif
LJ_STATIC_ASSERT((offsetof(TGState, jit_trace_stream.sequence) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream, next_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream, generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream, event_ordinal) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream, owner_key.incarnation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream,
			  callback_session_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitTraceStream,
			  terminal_session_generation) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(TGState, jit_trace_stream) >=
		 offsetof(TGState, jit_event_sessions) +
		 sizeof(LJJitEventSessions));
LJ_STATIC_ASSERT(offsetof(TGState, jit_trace_stream) +
		 sizeof(LJJitTraceStream) <= sizeof(TGState));
LJ_STATIC_ASSERT((offsetof(TGState, jit_event_attachment[0].sequence) & 7u) ==
		 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventAttachmentClock,
			  next_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventAttachmentClock, generation) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(TGState, jit_event_attachment) >=
		 offsetof(TGState, jit_trace_stream) +
		 sizeof(LJJitTraceStream));
LJ_STATIC_ASSERT(sizeof(((TGState *)0)->jit_event_attachment) ==
		 LJ_JIT_EVENT_ATTACHMENT_SLOTS *
		 sizeof(LJJitEventAttachmentClock));
LJ_STATIC_ASSERT(offsetof(TGState, jit_event_attachment) +
		 sizeof(((TGState *)0)->jit_event_attachment) <= sizeof(TGState));
LJ_STATIC_ASSERT(offsetof(TGState, vmevent_regkey) >=
		 offsetof(TGState, jit_event_attachment) +
		 sizeof(((TGState *)0)->jit_event_attachment));
LJ_STATIC_ASSERT((offsetof(TGState,
			  jit_event_callback_owner.sequence) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventCallbackOwner,
			  next_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventCallbackOwner, generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventCallbackOwner,
			  stream_generation) & 7u) == 0);
LJ_STATIC_ASSERT((offsetof(LJJitEventCallbackOwner,
			  session_generation) & 7u) == 0);
LJ_STATIC_ASSERT(offsetof(TGState, jit_event_callback_owner) >=
		 offsetof(TGState, vmevent_regkey) +
		 sizeof(((TGState *)0)->vmevent_regkey));
LJ_STATIC_ASSERT(offsetof(TGState, jit_event_callback_owner) +
		 sizeof(LJJitEventCallbackOwner) <= sizeof(TGState));
LJ_STATIC_ASSERT(offsetof(TGState, jit_trace_flush_reason) >=
		 offsetof(TGState, jit_event_callback_owner) +
		 sizeof(LJJitEventCallbackOwner));
LJ_STATIC_ASSERT(offsetof(TGState, jit_trace_flush_reason) +
		 sizeof(((TGState *)0)->jit_trace_flush_reason) <=
		 sizeof(TGState));
#if LJ_64
LJ_STATIC_ASSERT(sizeof(LJJitEventCallbackOwner) == 64u);
#endif
#if LJ_HASFFI
LJ_STATIC_ASSERT(offsetof(TGState, jit_event_sessions) >=
		 offsetof(TGState, ffi_native_frame) +
		 sizeof(((TGState *)0)->ffi_native_frame));
#endif
#endif
LJ_STATIC_ASSERT(offsetof(TGState, vmevent_regkey) +
		 sizeof(((TGState *)0)->vmevent_regkey) <= sizeof(TGState));
#if LJ_HASJIT
LJ_STATIC_ASSERT(sizeof(TGState) -
		 (offsetof(TGState, jit_trace_flush_reason) +
		  sizeof(((TGState *)0)->jit_trace_flush_reason)) <
		 __alignof__(TGState));
#else
LJ_STATIC_ASSERT(sizeof(TGState) -
		 (offsetof(TGState, vmevent_regkey) +
		  sizeof(((TGState *)0)->vmevent_regkey)) <
		 __alignof__(TGState));
#endif

static LJ_AINLINE int32_t lj_tg_vmstate_load_acq(TGState *tg)
{
  return (int32_t)la_load32_acq((uint32_t *)&tg->vmstate);
}

static LJ_AINLINE void lj_tg_vmstate_store_rel(TGState *tg, int32_t vmstate)
{
  la_store32_rel((uint32_t *)&tg->vmstate, (uint32_t)vmstate);
}

static LJ_AINLINE uint32_t lj_tg_in_native_acq(const TGState *tg)
{
  return la_load32_acq(&tg->in_native);  /* 05 section 5.4.3 native ack. */
}

static LJ_AINLINE void lj_tg_in_native_rel(TGState *tg, uint32_t in_native)
{
  la_store32_rel(&tg->in_native, in_native);  /* 05 section 5.4.3. */
}

static LJ_AINLINE void lj_tg_in_native_store_rlx(TGState *tg,
						 uint32_t in_native)
{
  la_store32_rlx(&tg->in_native, in_native);
}

static LJ_AINLINE StrTabHdr *lj_tg_strtab_active_hdr_acq(const TGState *tg)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&tg->strtab_active_hdr);
}

static LJ_AINLINE void lj_tg_strtab_active_hdr_rel(TGState *tg,
						   StrTabHdr *hdr)
{
  la_storeptr_rel((void **)&tg->strtab_active_hdr, hdr);
}

static LJ_AINLINE uint32_t lj_tg_strtab_active_depth_acq(const TGState *tg)
{
  return la_load32_acq(&tg->strtab_active_depth);
}

static LJ_AINLINE void lj_tg_strtab_active_depth_rel(TGState *tg,
						     uint32_t depth)
{
  la_store32_rel(&tg->strtab_active_depth, depth);
}

static LJ_AINLINE uint64_t lj_tg_strtab_active_epoch_acq(const TGState *tg)
{
  return la_load64_acq(&tg->strtab_active_epoch);
}

static LJ_AINLINE void lj_tg_strtab_active_epoch_rel(TGState *tg,
						     uint64_t epoch)
{
  la_store64_rel(&tg->strtab_active_epoch, epoch);
}

static LJ_AINLINE StrCanonHdr *lj_tg_strq_active_hdr_acq(const TGState *tg)
{
  return (StrCanonHdr *)la_loadptr_acq((void *const *)&tg->strq_active_hdr);
}

static LJ_AINLINE void lj_tg_strq_active_hdr_rel(TGState *tg,
						  StrCanonHdr *hdr)
{
  la_storeptr_rel((void **)&tg->strq_active_hdr, hdr);
}

static LJ_AINLINE uint32_t lj_tg_strq_active_depth_acq(const TGState *tg)
{
  return la_load32_acq(&tg->strq_active_depth);
}

static LJ_AINLINE void lj_tg_strq_active_depth_rel(TGState *tg,
						    uint32_t depth)
{
  la_store32_rel(&tg->strq_active_depth, depth);
}

static LJ_AINLINE uint64_t lj_tg_strq_active_epoch_acq(const TGState *tg)
{
  return la_load64_acq(&tg->strq_active_epoch);
}

static LJ_AINLINE void lj_tg_strq_active_epoch_rel(TGState *tg,
							    uint64_t epoch)
{
  la_store64_rel(&tg->strq_active_epoch, epoch);
}

static LJ_AINLINE uint32_t lj_tg_tab_read_depth_acq(const TGState *tg)
{
  return la_load32_acq(&tg->tab_read_depth);
}

static LJ_AINLINE void lj_tg_tab_read_depth_rel(TGState *tg, uint32_t depth)
{
  la_store32_rel(&tg->tab_read_depth, depth);
}

static LJ_AINLINE uint64_t lj_tg_tab_read_epoch_acq(const TGState *tg)
{
  return la_load64_acq(&tg->tab_read_epoch);
}

static LJ_AINLINE void lj_tg_tab_read_epoch_rel(TGState *tg, uint64_t epoch)
{
  la_store64_rel(&tg->tab_read_epoch, epoch);
}

static LJ_AINLINE uint32_t lj_tg_in_native_inc_rel(TGState *tg)
{
  uint32_t depth = lj_tg_in_native_acq(tg);
  if (depth != ~(uint32_t)0)
    depth++;
  lj_tg_in_native_rel(tg, depth);
  return depth;
}

static LJ_AINLINE uint8_t lj_tg_registry_shadow_missed_acq(const TGState *tg)
{
  return la_load8_acq(&tg->registry_shadow_missed);
}

static LJ_AINLINE void lj_tg_registry_shadow_missed_rel(TGState *tg,
						 uint8_t missed)
{
  la_store8_rel(&tg->registry_shadow_missed, missed);
}

static LJ_AINLINE uint32_t lj_tg_in_native_dec_rel(TGState *tg)
{
  uint32_t depth = lj_tg_in_native_acq(tg);
  if (depth != 0)
    depth--;
  lj_tg_in_native_rel(tg, depth);
  return depth;
}

static LJ_AINLINE uint8_t lj_tg_gc_assist_acq(const TGState *tg)
{
  return la_load8_acq(&tg->gc_assist);  /* 05 section 5.11 assist reentry. */
}

static LJ_AINLINE void lj_tg_gc_assist_store_rlx(TGState *tg,
						 uint8_t gc_assist)
{
  la_store8_rlx(&tg->gc_assist, gc_assist);
}

static LJ_AINLINE uint8_t lj_tg_hookmask_load(const TGState *tg)
{
  return la_load8_acq(&tg->hookmask_th);
}

static LJ_AINLINE uint8_t lj_tg_hookmask_update(TGState *tg, uint8_t clear,
						uint8_t set)
{
  uint8_t old = lj_tg_hookmask_load(tg);
  for (;;) {
    uint8_t next = (uint8_t)((old & (uint8_t)~clear) | set);
    if (la_cas8(&tg->hookmask_th, &old, next, LA_ACQ_REL, LA_ACQ))
      return next;
  }
}

static LJ_AINLINE int lj_tg_hookmask_set_if_clear(TGState *tg,
						  uint8_t blocked,
						  uint8_t set)
{
  uint8_t old = lj_tg_hookmask_load(tg);
  for (;;) {
    uint8_t next;
    if ((old & blocked))
      return 0;
    next = (uint8_t)(old | set);
    if (la_cas8(&tg->hookmask_th, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

/* One-shot callback admission.  Unlike the generic helper above, a collision
** is a bounded BUSY result rather than a reason to retry behind a profile
** publication.  PROFILE and a protected VM event are mutually exclusive on
** one TG, while different TGs remain independent. */
static LJ_AINLINE int lj_tg_hookmask_callback_enter_try(TGState *tg)
{
  uint8_t old, next;
  if (!tg)
    return 0;
  old = lj_tg_hookmask_load(tg);
  if (old & (HOOK_ACTIVE|HOOK_VMEVENT|HOOK_PROFILE))
    return 0;
  next = (uint8_t)(old | HOOK_ACTIVE | HOOK_VMEVENT);
  return la_cas8(&tg->hookmask_th, &old, next, LA_ACQ_REL, LA_ACQ);
}

/* Exact owner cleanup is non-refusable. Fetch-and-clear preserves every
** unrelated bit and completes in one atomic operation; false reports an
** internal owner/hook mismatch to the caller. */
static LJ_AINLINE int lj_tg_hookmask_callback_leave_exact(TGState *tg)
{
  uint8_t old;
  if (!tg)
    return 0;
  old = la_and8_acqrel(&tg->hookmask_th,
	(uint8_t)~(HOOK_ACTIVE|HOOK_VMEVENT));
  return (old & (HOOK_ACTIVE|HOOK_VMEVENT)) ==
    (HOOK_ACTIVE|HOOK_VMEVENT);
}

static LJ_AINLINE uint8_t lj_tg_hookmask_combined_load(
  global_State *g, const TGState *tg)
{
  return (uint8_t)((g ? hookmask_load(g) : 0) |
		   (tg ? lj_tg_hookmask_load(tg) : 0));
}

static LJ_AINLINE uint32_t lj_tg_profile_samples_xchg(TGState *tg,
						      uint32_t samples)
{
  return la_xchg32_acqrel(&tg->profile_samples, samples);
}

static LJ_AINLINE void lj_tg_profile_samples_add(TGState *tg,
						 uint32_t samples)
{
  (void)la_add32_rlx(&tg->profile_samples, samples);
}

static LJ_AINLINE int32_t lj_tg_profile_vmstate_load_acq(TGState *tg)
{
  return (int32_t)la_load32_acq((uint32_t *)&tg->profile_vmstate);
}

static LJ_AINLINE void lj_tg_profile_vmstate_store_rel(TGState *tg,
						       int32_t vmstate)
{
  la_store32_rel((uint32_t *)&tg->profile_vmstate, (uint32_t)vmstate);
}

static LJ_AINLINE uint32_t lj_tg_mark_active_acq(const TGState *tg)
{
  return la_load32_acq(&tg->mark_active);  /* 05 section 5.5 barrier mirror. */
}

static LJ_AINLINE void lj_tg_mark_active_rel(TGState *tg,
					     uint32_t mark_active)
{
  la_store32_rel(&tg->mark_active, mark_active);  /* 05 section 5.5. */
}

static LJ_AINLINE uint8_t lj_tg_alloc_black_acq(const TGState *tg)
{
  return la_load8_acq(&tg->alloc.alloc_black);  /* 05 section 5.5 alloc color. */
}

static LJ_AINLINE void lj_tg_alloc_black_rel(TGState *tg,
					     uint8_t alloc_black)
{
  la_store8_rel(&tg->alloc.alloc_black, alloc_black);  /* 05 section 5.5. */
}

#if LJ_HASJIT
static LJ_AINLINE int lj_tg_jit_exitcode_acq(const TGState *tg)
{
  return (int)la_load32_acq((uint32_t *)&tg->jit_exitcode);
}

static LJ_AINLINE void lj_tg_jit_exitcode_rel(TGState *tg, int exitcode)
{
  la_store32_rel((uint32_t *)&tg->jit_exitcode, (uint32_t)exitcode);
}
#endif

#if LJ_HASFFI
static LJ_AINLINE void *lj_tg_ffi_call_func_acq(const TGState *tg)
{
  return la_loadptr_acq((void *const *)&tg->ffi_call_func);  /* 11.5 callback. */
}

static LJ_AINLINE void lj_tg_ffi_call_func_rel(TGState *tg, void *func)
{
  la_storeptr_rel((void **)&tg->ffi_call_func, func);  /* 11.5 callback. */
}
#endif

static LJ_AINLINE uint8_t lj_tg_flags_acq(const TGState *tg)
{
  return la_load8_acq(&tg->tg_flags);  /* 05 section 5.4.1 TG registry. */
}

static LJ_AINLINE void lj_tg_flags_store_rlx(TGState *tg, uint8_t flags)
{
  la_store8_rlx(&tg->tg_flags, flags);
}

static LJ_AINLINE uint8_t lj_tg_flags_or_rlx(TGState *tg, uint8_t flags)
{
  return la_or8_rlx(&tg->tg_flags, flags);  /* 05 section 5.4.1/09 section 9.6. */
}

static LJ_AINLINE uint8_t lj_tg_flags_and_rlx(TGState *tg, uint8_t flags)
{
  return la_and8_rlx(&tg->tg_flags, flags);
}

static LJ_AINLINE int lj_tg_flags_test_acq(const TGState *tg, uint8_t flags)
{
  return (lj_tg_flags_acq(tg) & flags) != 0;
}

static LJ_AINLINE int lj_tg_flags_all_acq(const TGState *tg, uint8_t flags)
{
  return (lj_tg_flags_acq(tg) & flags) == flags;
}

static LJ_AINLINE uint8_t lj_tg_fini_state_acq(const TGState *tg)
{
  return la_load8_acq(&tg->fini_state);
}

static LJ_AINLINE void lj_tg_fini_state_store_rlx(TGState *tg,
						   uint8_t state)
{
  la_store8_rlx(&tg->fini_state, state);
}

static LJ_AINLINE void lj_tg_fini_state_rel(TGState *tg, uint8_t state)
{
  la_store8_rel(&tg->fini_state, state);
}

static LJ_AINLINE int lj_tg_fini_state_cas(TGState *tg, uint8_t *oldp,
					    uint8_t state)
{
  return la_cas8(&tg->fini_state, oldp, state, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE GC2SSBNode *lj_gc2_ssb_next_acq(const GC2SSBNode *node)
{
  return (GC2SSBNode *)la_loadptr_acq((void *const *)&node->next);
}

static LJ_AINLINE void lj_gc2_ssb_next_rel(GC2SSBNode *node,
					   GC2SSBNode *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

static LJ_AINLINE TGState *lj_gc2_ssb_owner_acq(const GC2SSBNode *node)
{
  /* 05 section 5.6.2: SSB owner. */
  return (TGState *)la_loadptr_acq((void *const *)&node->owner);
}

static LJ_AINLINE void lj_gc2_ssb_owner_rel(GC2SSBNode *node, TGState *owner)
{
  /* 05 section 5.6.2: SSB owner. */
  la_storeptr_rel((void **)&node->owner, owner);
}

static LJ_AINLINE uint32_t lj_gc2_ssb_count_acq(const GC2SSBNode *node)
{
  /* 05 section 5.6.2: SSB item count. */
  return la_load32_acq(&node->n);
}

static LJ_AINLINE void lj_gc2_ssb_count_rel(GC2SSBNode *node, uint32_t n)
{
  /* 05 section 5.6.2: SSB item count. */
  la_store32_rel(&node->n, n);
}

/* `pad` keeps the allocation flag in bit zero and the count of the active or
** published node's remembered suffix above it. Idle-generational pushes are a
** contiguous suffix: a phase transition flushes the old active node before
** any active-cycle producer can append to its replacement. Consumers pop SSB
** slots from the end, so the count is an exact per-slot tag without enlarging
** the hot node header or pointer representation. */
static LJ_AINLINE uint32_t
lj_gc2_ssb_remembered_acq(const GC2SSBNode *node)
{
  return la_load32_acq(&node->pad) >> TG_GC2_SSB_REMEMBERED_SHIFT;
}

static LJ_AINLINE void
lj_gc2_ssb_remembered_rel(GC2SSBNode *node, uint32_t n)
{
  uint32_t flags = la_load32_acq(&node->pad) & TG_GC2_SSB_DYNAMIC;
  lj_assertX(n <= TG_GC2_SSB_SLOTS, "SSB remembered suffix overflow");
  la_store32_rel(&node->pad,
	flags | (n << TG_GC2_SSB_REMEMBERED_SHIFT));
}

static LJ_AINLINE uint32_t lj_gc2_ssb_remembered_add(GC2SSBNode *node)
{
  uint32_t old = la_add32_rlx(&node->pad,
			      1u << TG_GC2_SSB_REMEMBERED_SHIFT);
  lj_assertX((old >> TG_GC2_SSB_REMEMBERED_SHIFT) < TG_GC2_SSB_SLOTS,
	     "SSB remembered suffix overflow");
  return old >> TG_GC2_SSB_REMEMBERED_SHIFT;
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_free_acq(const TGState *tg)
{
  return (GC2SSBNode *)la_loadptr_acq(
    (void *const *)&tg->ssb_free);  /* 05 section 5.6.2 SSB. */
}

static LJ_AINLINE uint32_t lj_tg_ssb_refs_acq(const TGState *tg)
{
  return la_load32_acq((uint32_t *)&tg->ssb_refs);
}

static LJ_AINLINE uint32_t lj_tg_ssb_refs_add(TGState *tg, uint32_t n)
{
  return la_add32_rlx(&tg->ssb_refs, n);
}

static LJ_AINLINE uint32_t lj_tg_ssb_refs_sub(TGState *tg, uint32_t n)
{
  return la_sub32_acqrel(&tg->ssb_refs, n);
}

static LJ_AINLINE void lj_tg_ssb_free_store_rlx(TGState *tg,
						GC2SSBNode *node)
{
  la_storeptr_rlx((void **)&tg->ssb_free, node);  /* 05 section 5.6.2 SSB. */
}

static LJ_AINLINE int lj_tg_ssb_free_cas(TGState *tg, GC2SSBNode **oldp,
					 GC2SSBNode *node)
{
  return la_casptr((void **)&tg->ssb_free, (void **)oldp, node,
		   LA_ACQ_REL, LA_ACQ);  /* 05 section 5.6.2 SSB free stack. */
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_free_pop(TGState *tg)
{
  GC2SSBNode *head = lj_tg_ssb_free_acq(tg);
  /* The owner TG is the only popper; GC2 workers/assists only push recycle. */
  while (head != NULL) {
    GC2SSBNode *next = lj_gc2_ssb_next_acq(head);
    GC2SSBNode *oldhead = head;
    if (lj_tg_ssb_free_cas(tg, &oldhead, next)) {
      lj_gc2_ssb_next_rel(head, NULL);
      return head;
    }
    head = oldhead;
  }
  return NULL;
}

static LJ_AINLINE void lj_tg_ssb_free_push(TGState *tg, GC2SSBNode *node)
{
  GC2SSBNode *head = lj_tg_ssb_free_acq(tg);
  do {
    lj_gc2_ssb_next_rel(node, head);
  } while (!lj_tg_ssb_free_cas(tg, &head, node));
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_active_acq(const TGState *tg)
{
  return (GC2SSBNode *)la_loadptr_acq(
    (void *const *)&tg->ssb_active);  /* 05 section 5.6.2 active SSB. */
}

static LJ_AINLINE void lj_tg_ssb_active_rel(TGState *tg, GC2SSBNode *node)
{
  la_storeptr_rel((void **)&tg->ssb_active, node);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_base_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_base);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_base_rel(TGState *tg, GCRef *base)
{
  la_storeptr_rel((void **)&tg->ssb_base, base);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_next_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_next);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_next_rel(TGState *tg, GCRef *next)
{
  la_storeptr_rel((void **)&tg->ssb_next, next);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_end_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_end);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_end_rel(TGState *tg, GCRef *end)
{
  la_storeptr_rel((void **)&tg->ssb_end, end);  /* 05 section 5.6.2. */
}

/* The owner publishes an exact pair by first invalidating cycle, then writing
** both comparison pointers, and finally release-publishing the nonzero cycle.
** Phase activation may remotely invalidate only the cycle while the JIT gate
** is closed. The pointer fields deliberately never carry tracing authority. */
static LJ_AINLINE uint32_t lj_tg_fnew_cert_cycle_acq(const TGState *tg)
{
  return la_load32_acq(&tg->fnew_cert_cycle);
}

static LJ_AINLINE void lj_tg_fnew_cert_reset_rel(TGState *tg)
{
  la_store32_rel(&tg->fnew_cert_cycle, 0);
}

static LJ_AINLINE GCproto *lj_tg_fnew_cert_pt_acq(const TGState *tg)
{
  return (GCproto *)la_loadptr_acq((void *const *)&tg->fnew_cert_pt);
}

static LJ_AINLINE GCtab *lj_tg_fnew_cert_env_acq(const TGState *tg)
{
  return (GCtab *)la_loadptr_acq((void *const *)&tg->fnew_cert_env);
}

static LJ_AINLINE void lj_tg_fnew_cert_publish_rel(TGState *tg,
						    GCproto *pt, GCtab *env,
						    uint32_t cycle)
{
  lj_assertX(cycle != 0, "publishing invalid FNEW certificate cycle");
  lj_tg_fnew_cert_reset_rel(tg);
  la_storeptr_rlx((void **)&tg->fnew_cert_pt, pt);
  la_storeptr_rlx((void **)&tg->fnew_cert_env, env);
  la_store32_rel(&tg->fnew_cert_cycle, cycle);
}

static LJ_AINLINE GCobj *lj_tg_gcroot_pending_acq(const TGState *tg)
{
  return (GCobj *)la_loadptr_acq((void *const *)&tg->gcroot_pending);
}

static LJ_AINLINE void lj_tg_gcroot_pending_hint(TGState *tg, GCobj *head)
{
  global_State *g = tg ? tg->gl : NULL;
  if (head && g)
    lj_gcroot_pending_hint_rel(g, 1);
}

static LJ_AINLINE void lj_tg_gcroot_pending_transition_hint(TGState *tg,
							    GCobj *oldhead,
							    GCobj *head)
{
  /*
  ** The global hint only needs publication when a TG-local pending-root stack
  ** transitions from empty to non-empty. Appends to an already non-empty stack
  ** are covered by the earlier transition hint, while before/after hints around
  ** the transition preserve the race coverage against a concurrent flusher that
  ** clears the hint between the local stack publication steps.
  */
  if (oldhead == NULL)
    lj_tg_gcroot_pending_hint(tg, head);
}

static LJ_AINLINE void lj_tg_gcroot_pending_store_rlx(TGState *tg,
						      GCobj *head)
{
  la_storeptr_rlx((void **)&tg->gcroot_pending, head);
}

static LJ_AINLINE void lj_tg_gcroot_pending_store_transition_rel(TGState *tg,
								 GCobj *oldhead,
								 GCobj *head)
{
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  la_storeptr_rel((void **)&tg->gcroot_pending, head);
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
}

static LJ_AINLINE int lj_tg_gcroot_pending_cas(TGState *tg, GCobj **oldp,
					       GCobj *head)
{
  int ok;
  GCobj *oldhead = *oldp;
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  ok = la_casptr((void **)&tg->gcroot_pending, (void **)oldp, head,
		 LA_ACQ_REL, LA_ACQ);  /* Pending root stack publication. */
  if (ok)
    lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  return ok;
}

static LJ_AINLINE GCobj *lj_tg_gcroot_pending_xchg_acqrel(TGState *tg,
							  GCobj *head)
{
  return (GCobj *)la_xchgptr_acqrel((void **)&tg->gcroot_pending, head);
}

static LJ_AINLINE GCobj *lj_tg_gcroot_pending_after_main_acq(
  const TGState *tg)
{
  return (GCobj *)la_loadptr_acq(
    (void *const *)&tg->gcroot_pending_after_main);
}

static LJ_AINLINE void lj_tg_gcroot_pending_after_main_store_rlx(
  TGState *tg, GCobj *head)
{
  la_storeptr_rlx((void **)&tg->gcroot_pending_after_main, head);
}

static LJ_AINLINE void lj_tg_gcroot_pending_after_main_store_transition_rel(
  TGState *tg, GCobj *oldhead, GCobj *head)
{
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  la_storeptr_rel((void **)&tg->gcroot_pending_after_main, head);
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
}

static LJ_AINLINE int lj_tg_gcroot_pending_after_main_cas(TGState *tg,
							  GCobj **oldp,
							  GCobj *head)
{
  int ok;
  GCobj *oldhead = *oldp;
  lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  ok = la_casptr((void **)&tg->gcroot_pending_after_main, (void **)oldp,
		 head, LA_ACQ_REL, LA_ACQ);
  if (ok)
    lj_tg_gcroot_pending_transition_hint(tg, oldhead, head);
  return ok;
}

static LJ_AINLINE GCobj *lj_tg_gcroot_pending_after_main_xchg_acqrel(
  TGState *tg, GCobj *head)
{
  return (GCobj *)la_xchgptr_acqrel(
    (void **)&tg->gcroot_pending_after_main, head);
}

static LJ_AINLINE uint32_t lj_tg_tid_acq(const TGState *tg)
{
  return la_load32_acq(&tg->tid);  /* 05 section 5.4.1 TG owner id. */
}

static LJ_AINLINE uint32_t lj_tg_actor_acq(const TGState *tg)
{
  return tg ? la_load32_acq(&tg->actor_id) : 0;
}

static LJ_AINLINE void lj_tg_actor_rel(TGState *tg, uint32_t actor)
{
  la_store32_rel(&tg->actor_id, actor);
}

static LJ_AINLINE int lj_tg_actor_cas(TGState *tg, uint32_t *oldp,
				      uint32_t actor)
{
  return la_cas32(&tg->actor_id, oldp, actor, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE int lj_tg_owns_state_acq(const TGState *tg,
					    const lua_State *L)
{
  LJStateOwner owner;
  uint32_t actor;
  if (!tg || !L || !(actor = lj_tg_actor_acq(tg)))
    return 0;
  owner = lj_state_owner_word_acq(L);
  return lj_state_owner_tid(owner) == lj_tg_tid_acq(tg) &&
    lj_state_owner_actor(owner) == actor &&
    lj_tg_actor_acq(tg) == actor;
}

static LJ_AINLINE void lj_tg_tid_rel(TGState *tg, uint32_t tid)
{
  lj_arena_alloc_owner_rel(&tg->alloc, tid);
  la_store32_rel(&tg->tid, tid);  /* Publish allocator owner before TG id. */
}

static LJ_AINLINE TGState *lj_tg_next_acq(const TGState *tg)
{
  return (TGState *)la_loadptr_acq((void *const *)&tg->next_tg);
}

static LJ_AINLINE void lj_tg_next_rel(TGState *tg, TGState *next)
{
  la_storeptr_rel((void **)&tg->next_tg, next);
}

static LJ_AINLINE TGState *lj_tg_worker_retire_next_acq(const TGState *tg)
{
  return (TGState *)la_loadptr_acq(
    (void *const *)&tg->worker_retire_next);
}

static LJ_AINLINE void lj_tg_worker_retire_next_rel(TGState *tg,
						     TGState *next)
{
  la_storeptr_rel((void **)&tg->worker_retire_next, next);
}

static LJ_AINLINE uint32_t lj_tg_poll_acq(const TGState *tg)
{
  return la_load32_acq(&tg->poll);
}

static LJ_AINLINE void lj_tg_poll_store_rlx(TGState *tg, uint32_t poll)
{
  la_store32_rlx(&tg->poll, poll);
}

static LJ_AINLINE void lj_tg_poll_rel(TGState *tg, uint32_t poll)
{
  la_store32_rel(&tg->poll, poll);
}

static LJ_AINLINE void lj_tg_poll_futex_wait(TGState *tg, uint32_t poll,
					     int timeout_ns)
{
  la_futex_wait(&tg->poll, poll, timeout_ns);
}

static LJ_AINLINE void lj_tg_poll_futex_wake(TGState *tg, int n)
{
  la_futex_wake(&tg->poll, n);
}

static LJ_AINLINE uint32_t lj_tg_profile_request_acq(const TGState *tg)
{
  return la_load32_acq(&tg->profile_request);
}

static LJ_AINLINE void lj_tg_profile_request_store_rlx(TGState *tg,
						       uint32_t request)
{
  la_store32_rlx(&tg->profile_request, request);
}

static LJ_AINLINE void lj_tg_profile_request_rel(TGState *tg,
						 uint32_t request)
{
  la_store32_rel(&tg->profile_request, request);
}

static LJ_AINLINE uint32_t lj_tg_profile_request_xchg_acqrel(TGState *tg,
						     uint32_t request)
{
  return la_xchg32_acqrel(&tg->profile_request, request);
}

static LJ_AINLINE uint32_t lj_tg_reqmask_acq(const TGState *tg)
{
  return la_load32_acq(&tg->reqmask);
}

static LJ_AINLINE void lj_tg_reqmask_store_rlx(TGState *tg, uint32_t reqmask)
{
  la_store32_rlx(&tg->reqmask, reqmask);
}

static LJ_AINLINE void lj_tg_reqmask_rel(TGState *tg, uint32_t reqmask)
{
  la_store32_rel(&tg->reqmask, reqmask);
}

static LJ_AINLINE uint32_t lj_tg_reqmask_xchg_acqrel(TGState *tg,
						     uint32_t reqmask)
{
  return la_xchg32_acqrel(&tg->reqmask, reqmask);
}

static LJ_AINLINE uint64_t lj_tg_hs_epoch_ack_acq(const TGState *tg)
{
  return la_load64_acq(&tg->hs_epoch_ack);
}

static LJ_AINLINE void lj_tg_hs_epoch_ack_store_rlx(TGState *tg,
						    uint64_t epoch)
{
  la_store64_rlx(&tg->hs_epoch_ack, epoch);
}

static LJ_AINLINE void lj_tg_hs_epoch_ack_rel(TGState *tg, uint64_t epoch)
{
  la_store64_rel(&tg->hs_epoch_ack, epoch);
}

static LJ_AINLINE int lj_tg_hs_epoch_ack_cas(TGState *tg, uint64_t *oldp,
					     uint64_t epoch)
{
  return la_cas64(&tg->hs_epoch_ack, oldp, epoch, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE uint64_t lj_tg_local_total_xchg_acqrel(TGState *tg,
							 uint64_t bytes)
{
  return la_xchg64_acqrel(&tg->local_total, bytes);  /* 04 section 4.8. */
}

static LJ_AINLINE uint64_t lj_tg_local_total_acq(TGState *tg)
{
  return la_load64_acq(&tg->local_total);  /* 04 section 4.8. */
}

static LJ_AINLINE uint64_t lj_tg_local_total_add_rlx(TGState *tg,
						     uint64_t bytes)
{
  return la_add64_rlx(&tg->local_total, bytes);  /* 04 section 4.8. */
}

static LJ_AINLINE uint64_t lj_tg_stack_dirty_epoch_acq(const TGState *tg)
{
  return la_load64_acq(&tg->stack_dirty_epoch);  /* 05 section 5.7.3. */
}

static LJ_AINLINE uint64_t lj_tg_stack_dirty_epoch_add_rlx(TGState *tg,
							   uint64_t n)
{
  return la_add64_rlx(&tg->stack_dirty_epoch, n);  /* 05 section 5.7.3. */
}

static LJ_AINLINE uint32_t lj_tg_root_anchor_top_acq(const TGState *tg)
{
  return la_load32_acq(&tg->root_anchor_top);
}

static LJ_AINLINE void lj_tg_root_anchor_top_rel(TGState *tg, uint32_t top)
{
  la_store32_rel(&tg->root_anchor_top, top);
}

static LJ_AINLINE TGRootAnchorBlock *lj_tg_root_anchor_next_acq(
  const TGRootAnchorBlock *block)
{
  return (TGRootAnchorBlock *)la_loadptr_acq((void *const *)&block->next);
}

static LJ_AINLINE void lj_tg_root_anchor_next_rel(TGRootAnchorBlock *block,
						  TGRootAnchorBlock *next)
{
  la_storeptr_rel((void **)&block->next, next);
}

static LJ_AINLINE lua_State *lj_tg_load_cur_L(TGState *tg)
{
  return (lua_State *)la_loadptr_acq((void *const *)&tg->cur_L);
}

static LJ_AINLINE void lj_tg_store_cur_L(TGState *tg, lua_State *L)
{
  la_storeptr_rel((void **)&tg->cur_L, L);  /* 05 section 5.7.4 TG root. */
}

static LJ_AINLINE struct LexState *lj_tg_lexstate_acq(const TGState *tg)
{
  return (struct LexState *)la_loadptr_acq((void *const *)&tg->lexstate);
}

static LJ_AINLINE void lj_tg_lexstate_rel(TGState *tg, struct LexState *ls)
{
  la_storeptr_rel((void **)&tg->lexstate, ls);
}

static LJ_AINLINE lua_State *lj_tg_load_thread_L(TGState *tg)
{
  return (lua_State *)la_loadptr_acq((void *const *)&tg->thread_L);
}

static LJ_AINLINE void lj_tg_store_thread_L(TGState *tg, lua_State *L)
{
  la_storeptr_rel((void **)&tg->thread_L, L);  /* 05 section 5.7.4 TG root. */
}

static LJ_AINLINE GCudata *lj_tg_load_thread_ud(TGState *tg)
{
  return (GCudata *)la_loadptr_acq((void *const *)&tg->thread_ud);
}

static LJ_AINLINE void lj_tg_store_thread_ud(TGState *tg, GCudata *ud)
{
  la_storeptr_rel((void **)&tg->thread_ud, ud);
}

static LJ_AINLINE TValue *lj_tg_load_jit_base(TGState *tg)
{
  return (TValue *)la_loadptr_acq((void *const *)&tg->jit_base);
}

static LJ_AINLINE int lj_tg_jit_active_acq(TGState *tg)
{
  return lj_tg_load_jit_base(tg) != NULL || lj_tg_vmstate_load_acq(tg) > 0;
}

static LJ_AINLINE void lj_tg_store_jit_base(TGState *tg, TValue *base)
{
  la_storeptr_rel((void **)&tg->jit_base, base);  /* 08 section 8.7 exit root. */
}

static LJ_AINLINE lua_State *lj_tg_cur_L(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    return lj_tg_load_cur_L(tg);
  if (!g)
    return NULL;
  {
    GCobj *o = gcref_acq(g->cur_L);
    return o ? gco2th(o) : NULL;
  }
}

static LJ_AINLINE void lj_tg_setcur_L(global_State *g, lua_State *L)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_cur_L(tg, L);
  if (!g)
    return;
  setgcrefrel(g->cur_L, obj2gco(L));  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE void lj_tg_clearcur_L(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_cur_L(tg, NULL);
  if (!g)
    return;
  setgcrefnullrel(g->cur_L);  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE TValue *lj_tg_jit_base(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    return lj_tg_load_jit_base(tg);
  if (!g)
    return NULL;
  return mref_acq(g->jit_base, TValue);  /* Transitional mirror for VM asm writes. */
}

static LJ_AINLINE void lj_tg_setjit_base(global_State *g, TValue *base)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_jit_base(tg, base);
  if (!g)
    return;
  setmrefrel(g->jit_base, base);  /* Transitional mirror for VM asm. */
}

LJ_FUNC int lj_tg_any_jit_active(global_State *g);
LJ_FUNC TValue *lj_tg_root_anchor_push(lua_State *L, TGState *tg,
				       cTValue *tv, uint32_t *idxp);
/* Ensure storage for the next root-anchor slot without publishing a root or
** raising an error. This is used immediately before a value-producing
** operation whose result exists only in a C local: the later push is then a
** non-allocating publication. */
LJ_FUNC int lj_tg_root_anchor_reserve_nothrow(lua_State *L, TGState *tg);
LJ_FUNC void lj_tg_root_anchor_pop(TGState *tg, uint32_t idx);
/* Restore an owner TG's lexical anchor stack to a protected checkpoint.
** This is allocation-free and is safe on a nonlocal-error cleanup path. */
LJ_FUNC void lj_tg_root_anchor_rollback(TGState *tg, uint32_t saved_top);
LJ_FUNC TValue *lj_tg_root_anchor_slot_acq(TGState *tg, uint32_t idx);
#if LJ_HASJIT
/* Runtime guard source for recorded pcall/xpcall frames. Traces bake the
** protected frame word, so they may run only at the anchor depth recorded in
** its upper checkpoint word. */
LJ_FUNCA uint32_t lj_tg_root_anchor_top_forjit(lua_State *L);
#endif
#if defined(LJ_TG_ROOT_TEST_HELPERS)
typedef void (*LJTGRootPushHook)(lua_State *L, TGState *tg, uint32_t idx,
				 TValue *slot);
LJ_FUNC void lj_tg_root_test_fail_reserve_after(uint32_t nth);
LJ_FUNC void lj_tg_root_test_set_push_hook(LJTGRootPushHook hook);
#if LJ_HASJIT
LJ_FUNC void lj_tg_root_test_forjit_guard_reset(void);
LJ_FUNC uint32_t lj_tg_root_test_forjit_guard_hits(void);
#endif
#endif

#define TG_DISP2HOT	(-(int)(HOTCOUNT_SIZE*sizeof(HotCount)))
#define TG_OFS(f) \
  ((int)(offsetof(TGState, f) - offsetof(TGState, dispatch)))
#define DISPATCH_TG(f)	TG_OFS(f)

LJ_FUNC void lj_tg_init(GG_State *GG, int alloc_ready, uint32_t tid);
LJ_FUNC void lj_tg_fini(global_State *g);
LJ_FUNC void lj_tg_init_thread(global_State *g, TGState *tg, lua_State *L,
			       int arena_internal);
LJ_FUNC void lj_tg_derive_prng(global_State *g, TGState *tg, uint32_t tid);
LJ_FUNC void lj_tg_fini_ssb(TGState *tg);
LJ_FUNC int lj_tg_fini_thread(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_attach(global_State *g, TGState *tg);
LJ_FUNC int lj_tg_registry_detach_begin(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_detach(global_State *g, TGState *tg);
LJ_FUNC int lj_tg_registry_main_close_begin(global_State *g);
LJ_FUNC void lj_tg_registry_fini(global_State *g);
LJ_FUNC uint32_t lj_tg_reclaim_dead(global_State *g);
LJ_FUNC uint32_t lj_tg_reclaim_dead_terminal(global_State *g);
LJ_FUNC uint32_t lj_tg_reclaim_dead_terminal_orphans(global_State *g);
LJ_FUNC TGState *lj_tg_find_owner(global_State *g, uint32_t owner_tid);
LJ_FUNC TGState *lj_tg_thread_active(global_State *g, lua_State *L);
LJ_FUNC void lj_tg_sync_dispatch_tg(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_sync_dispatch(global_State *g);

#endif
