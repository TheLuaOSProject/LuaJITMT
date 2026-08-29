# Rooted JIT event-session substrate (2026-07-19)

## Scope

This tranche adds the storage, ownership and GC-lifetime substrate for future
lockless JIT VM-event delivery. It does **not** invoke a VM-event handler yet.
In particular, it does not claim that TRACE event grammar, attachment
replacement, handler-error unwinding, nested suppression or pending terminal
delivery is complete.

The implementation is intentionally at the tail of `TGState`, after the
existing native-FFI frame publication. No pre-existing TG/VM offset moves. Each
TG has two recyclable event slots, one active publication selector and an
even/odd publication sequence. A slot contains immutable owner identity, event
and attachment generations, an explicit owner-thread GC root, an optional exact
published-trace pin, a retained root vector and a copied raw frozen view. The
view is never presented to GC as a `GCtrace` and must never grow a fake GC
header.

## Explicit ownership and edge-proof matrix

`owner_mode` removes the ambiguous old idea of an ACTIVE session with an
unexplained JIT owner word:

| Events | Owner mode | Frozen view / edge proof | JIT owner transition |
| --- | --- | --- | --- |
| `TRACE_START`, `RECORD` | `CONTINUATION_LIFECYCLE` | source-less V1 view plus `EXACT_ROOTS` | publish, exact `{tid,0}->{0,tid}`; exact resume before unpublish |
| `TRACE_STOP` | `DETACHED_IMMUTABLE` | V1 view plus `PINNED_SOURCE` | publish, finish/require IDLE mutable J state, exact `{tid,0}->{0,0}` |
| `TRACE_ABORT` | `DETACHED_IMMUTABLE` | V1 plus either `EXACT_ROOTS` or `PINNED_SOURCE` | same detached handoff |
| `TRACE_FLUSH` | `DETACHED_IMMUTABLE` | no view, source or extra roots; `NONE` | same detached handoff |

Continuation begin additionally requires the matching recorder phase: START
for `TRACE_START`, and RECORD/RECORD_1ST for `RECORD`. Detached begin requires
IDLE. `J->L` must be the exact owner at begin.

The raw publish and unpublish operations are private. Public/internal
`begin_l` and `end_l` compose publication with owner-word transitions:

- a continuation cannot yield before its roots/view are ACTIVE, and cannot
  unpublish until high-to-low resume succeeds;
- post-publication handoff failure rolls the slot back while the exact low
  token and `J->L` still exist; an impossible ownership loss is fail-stop;
- detached completion closes by exact TG actor/session generation and never
  changes a peer's owner word;
- detached completion rejects the original TG appearing in either owner-word
  half, catching a leaked same-owner reentrant recorder/lifecycle; and
- stale handle/generation closure leaves the live session unchanged.

Detached storage can coexist with an unrelated recorder owner. That is only a
storage/ownership statement. The later universe-global TRACE stream descriptor
must still prevent a new START from overtaking an undelivered terminal event.
Persistent START-to-terminal pairing and attachment/handler generation cannot
live in either recyclable callback slot.

A detached `TRACE_ABORT` using `EXACT_ROOTS` has no source/native lifetime
lease after low-to-zero, so admission rejects any nonzero mcode address, size,
loop offset or exit-stub address. A continuation may retain that tuple only
while its high-half lifecycle/control exclusion remains live; that production
callback/control wiring is still absent.

## Frozen V1 and GC contract

V1 currently copies bounded IR, snapshot and snapshot-map spans plus the scalar
trace fields needed by traceinfo/mcode consumers: trace/root/link/type,
IR/snapshot counts, start PC/instruction, mcode size/address/loop point and exit
stub geometry. Cheap admission rejects bad span stride/alignment/order, invalid
IR reference bounds, invalid link types, incoherent mcode tuples, invalid loop
offsets and inconsistent exit counts. Alignment is checked on each effective
`data + offset` address, not merely on the relative offset, and both the whole
view and every span reject `uintptr_t` address overflow before any typed or
atomic snapshot access. The same geometry predicate protects unpublished
source specs and retained reader views. A pinned view must match the exact
published source after native pin admission, including byte-for-byte IR and
snapshot-map spans, canonical field-wise snapshot equality and the frozen
scalar header. `SnapShot.count` is changed atomically by runtime exits, so live
snapshot arrays are never whole-structure copied or compared. The canonical
copy zeroes frozen padding, loads every field through its accessor (especially
`snap_count_acq`), and equality compares immutable fields rather than padding
bytes. The copied count is point-in-time event information and is deliberately
excluded from source equality, so a legitimate exit increment between freeze
and pin cannot spuriously reject the event.

`EXACT_ROOTS` currently proves that a retained root vector was supplied and is
scanned; the focused fixture exercises 17 roots so the dynamic path beyond the
eight-entry inline fast path is real. Before production callback wiring, the
builder and decoder must additionally prove equality between every raw V1 GC
edge and this vector, including KGC reference/index binding and canonical
snapshot PC/prototype mapping. Deep IR operand and snapshot semantic validation
also remains follow-up. The current substrate must not be treated as a decoder
for untrusted bytes.

GC2 snapshots add a nonwaiting nested SMR lease and hold it until release. A
reader count prevents retained view/root backing reuse. The owner `lua_State`
is an explicit GC root, not merely a raw identity pointer. Scanner validation
uses immutable slot tid/actor metadata and admits the THREAD body before reading
its universe pointer. It intentionally does not reread mutable `tg_hint` or
state ownership after close: a CLOSED reader may overlap logical detach, mark
an extra ended root, fail its final sequence recheck and retry safely.

Continuation publications have the stricter live-identity contract required by
their potentially unpinned native addresses. A scan accepts them only while the
complete JIT owner word is exactly `{owner_tid, 0}` or `{0, owner_tid}`, `J->L`
still names the published owner state, and the slot actor equals the TG's
current actor. The actor is rechecked on both sides of snapshot release. A
zero/peer owner, cleared `J->L`, actor mismatch, close, or handoff overlap is a
root-scan retry. Detached immutable publications intentionally do not inherit
the owner-word restriction.

Close never waits. With no reader it unpins/cleans immediately; otherwise the
last reader performs cleanup and returns the slot to FREE. Source pinning keeps
a retired trace body, its prototype/KGC/snapshot graph and native code lifetime
available to the scanner until that cleanup.

Before any input root is dereferenced, publication rejects root/view ranges
which overlap either retained allocation in **any** slot. This includes the
slot-0-CLOSED/reader-held plus slot-1-BUILDING case: last-reader cleanup may
zero slot 0 roots while slot 1 is being prepared, so selected-slot-only alias
checks are insufficient.

## Logical detach versus physical lifetime

Secondary logical detach uses a relaxed, stable predicate: publication must be
IDLE and every slot must be terminal FREE, CLOSED or CLEANING. BUILDING, ACTIVE
and unknown states are rejected. CLOSED/FREE readers retain GC2 SMR, so the TG
may publish DEAD while physical finalization keeps returning retry until every
reader is gone and every slot is strict FREE/readers-zero.

Main/universe close and `sessions_fini` remain strict. Future readers not based
on the current GC2 snapshot API will need an equivalent universe-global lease;
the relaxed predicate is not permission to keep arbitrary raw TG pointers.
Public secondary detach is a safe no-op before any mutation if its TG owns a VM
event, C frame, native/FFI/callback scope, generated native frame, high JIT
lifecycle or ACTIVE/BUILDING session. Actor handoff is stricter than logical
detach: before clearing TLS or publishing actor zero it rejects either target
JIT-owner half and requires all slots FREE/readers-zero, so even a retained
CLOSED reader cannot strand the old identity without changing the session
sequence. Synthetic tid-zero fixtures skip owner-half equality so `{0,0}` is
not mistaken for ownership.

Physical secondary finalization is serialized at the TG level before event
slot cleanup. Only the successful LIVE/RETRY-to-BUSY CAS may call strict
`sessions_fini`; a retained CLOSED reader publishes `TG_FINI_RETRY` and leaves
the complete TG authoritative for a later attempt. This prevents competing
reclaim paths from racing CLOSED-to-FREE cleanup.

The same-owner `lua_close` fast path now refuses nonzero JIT owner state or a
nonquiescent session. Broader terminal `os.exit`/error-path event semantics are
adjacent follow-up, not implemented by this tranche.

## FFI auto-attach unwind integration

The public C-frame detach veto exposed a separate callback lifetime bug:
`lj_ccallback_unwind()` used to call public detach before `err_unwind()` had
removed `L->cframe`, so a TLS-less throwing callback leaked its TG and
`mt_live` token. The callback frame now transfers its `auto_detach` bit into a
one-bit runtime debt after popping callback/native roots. Platform unwind code
consumes it only at a cleanup edge which has logically popped the FFI
continuation and is proven to continue into foreign frames: Win64 cleanup,
ARM frame advance, or DWARF `_URC_CONTINUE_UNWIND` after excluding every
`INSTALL_CONTEXT`/rethrow landing which can still use `SAVE_L`.

The special detach path accepts only the exact current secondary TG/state,
depth-zero callback runtime, NULL carrier, zero slot/native-stop snapshot/FFI
call root, and the pending bit exactly one. After clearing that bit it reuses
the unchanged public C-frame/native/JIT/session certificates and common detach
commit. The unwind helper saves and restores the callback's selected
errno/LastError pair across lifecycle cleanup. Refusal is fail-stop because the
physical unwind leaves no lawful retry actor.

An exception destructor cannot be this boundary: LuaJIT's own x64 exception is
not necessarily deleted, while a foreign exception may be deleted before
cleanup has processed the callback continuation. A pre-branch DWARF hook is
also invalid because the ancient handler-frame path may still install
`lj_vm_unwind_rethrow` and use `SAVE_L` after detach. The full design and
platform caveat are in `ffi-callback-auto-detach-unwind-2026-07-19.md`.

The C++ foreign-catch regression performs a real TLS-less throwing callback,
holds SMR only while inspecting the retired TG, and proves TLS/state/callback
roots and `mt_live` are cleared before deterministic reclaim. Its second phase
holds the catch open while main enters `lua_close`; with no auxiliary SMR lease,
close stays blocked on the auto-attach lifetime token and completes only after
the unwind detach releases it.

## Allocation boundary

Retained view and overflow-root backing currently use C
`malloc/realloc/free`, only while a slot is unpublished BUILDING. This follows
the project's explicitly temporary internal-allocator-only policy and ignores
custom `lua_Alloc`. It is not the final allocator-compatible design. Restoring
custom `lua_Alloc` requires exact allocator identity/lifetime metadata or
preallocated retained backing, and the final nonblocking JIT path must keep any
potential allocator contention outside callback execution.

## Focused evidence and remaining wiring

`t-jit-event-session.c` covers malformed geometry (including an unaligned byte
base and an aligned near-`UINTPTR_MAX` overflow base) and source mismatch,
IR/snapshot-map/canonical-snapshot corruption, detached-ABORT native-address
rejection, post-publication rollback, same-slot and cross-slot retained-buffer
alias rejection, source-less V1 with 17 exact roots, direct GC2 marking,
high/low continuation ordering, stale handles, held-reader close/finalization
retry, slot reuse,
detached token-zero STOP/FLUSH, same-owner close rejection, peer-owner
preservation, exact retired source marking/pinning and main-close refusal. It
also injects zero continuation ownership, a cleared `J->L`, and a peer actor to
prove each GC scan retries, while a detached session remains scannable under a
peer owner word. Low/high owner tokens, detached ACTIVE state, and a
CLOSED-reader session each prove actor handoff rejects without mutating TLS or
the actor word. The fixture creates a real heap secondary TG on a pthread,
acquires the snapshot from
the main actor while that TG is live, closes the slot, and performs the public
logical detach while the foreign reader remains. The case proves DEAD/RETIRED
publication does not wait, strict event-session and full-TG finalization both
refuse the CLOSED reader, the full finalizer publishes `TG_FINI_RETRY` after
its serialized `BUSY` attempt, the SMR lease vetoes physical list reclaim, and
last-reader release enables that retry to complete the exact heap-TG reclaim.

Required landing validation on Linux/x86-64:

- `m6_jit_event_session`: clean GC2-helper build with
  `-Wall -Wextra -Werror`, followed by the focused C fixture;
- `m6_dispatch_redispatch`, `m6_jit_token` and
  `m6_jit_gc2_readiness`;
- `m0_matrix`: warning-as-error default and `LUAJIT_DISABLE_JIT` builds,
  including 509 default and 387 no-JIT stock-suite cases; and
- `m3_gc2_no_legacy_runtime`: retired-symbol and GC2-only runtime/close gates
  for both ordinary and amalgamated builds.

The redispatch run initially exposed a real compatibility regression rather
than a harness false positive: synthetic directly attached TG fixtures use
tid zero to mean “no JIT owner”, and the first owner-half gate mistook both
empty halves for ownership. Central/registry detach now skips that meaningless
comparison only for tid zero while still requiring logical session readiness.
The real-secondary subcase closes the previous fixture gap: the relaxed
readiness predicate is now exercised through `lj_threading_detach`, not only as
a direct predicate assertion, and the same retained snapshot covers strict
finalization refusal and eventual physical reclaim in one ordered lifecycle.

Still intentionally absent:

1. VM callback invocation and error/unwind wiring;
2. the persistent universe-global stream/attachment descriptor;
3. production exact-root construction and frozen-view deep decoding;
4. token-wait/control paths deferring correctly around lifecycle ownership;
5. stable control borrowing (reserved slot fields remain zero and unsupported);
6. durable VM-event terminal delivery and its future event-session auto-detach
   retry (not the fixed FFI callback TG leak); and
7. performance/platform validation beyond the later Linux-focused landing
   build (Windows/macOS remain deferred under the current release sequence).
