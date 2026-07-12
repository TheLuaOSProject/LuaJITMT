# GC2 header-ready publication and retained interior cdata views

Date: 2026-07-12

Status: implemented work in progress. This note records a deliberate extension
of the GC2 arena identity design. It does not edit `plan/`. The implementation
is not yet a release claim: the remaining gates at the end of this note still
need to close before b1.2.0.

## Problem

An arena allocation bit proves that storage belongs to an allocation. It does
not prove that a concurrent reader may decode a Lua GC header from that
storage. This distinction is observable during every constructor which can
allocate, throw, or cross a GC2 safepoint between reserving memory and
publishing a fully initialized object.

Variable-sized cdata adds a second distinction. The allocation base can precede
the `GCcdata` header, while FFI clients and finalizer registries can hold the
header address. Treating an arbitrary interior address as either the allocation
base or a typed GC object admitted false headers and made retained views depend
on an unbounded backwards search.

The old partial implementation conflated these facts:

1. storage is allocated;
2. an address is the authoritative typed-object identity;
3. the GC header and typed prefix are initialized and published;
4. the object is semantically reachable.

GC2 now represents them separately.

## Arena metadata planes

Small arenas carry two additional side planes:

- `cdata[]` covers every cell in the exact allocation span of a published
  cdata object;
- `ready[]` is the terminal header-ready publication bit.

The ordinary allocation bitmap remains the physical allocation-base/boundary
authority. The cdata coverage run plus the immutable four-bit byte tail stored
in `GCcdata.flags` is the independent exact byte extent. Bit zero remains the
mutable callback-release flag and is not part of the extent encoding. The
resulting states are:

| Allocation | Identity | READY | Meaning |
| --- | --- | --- | --- |
| clear | clear | clear | free/unowned storage |
| set | no published coverage | clear | pending, opaque, or pinned raw storage |
| set | exact cdata coverage or ordinary base | set | typed header may be decoded |

No typed validator may upgrade `READY == 0` based on plausible bytes in the
allocation. Constructors initialize the complete required header/prefix and
then release-publish READY. Linking into an intrusive root/ownership structure
is a later operation. Sweep, quarantine, free, and reuse clear both identity and
READY metadata before the storage can acquire another meaning.

Huge allocations encode the same distinction in HugeTab flags. `CDATA` names
the allocation family, `INTERIOR_CDATA` says the typed header is reached via
the published base-prefix offset, and `READY` authorizes typed access. The
HugeTab record supplies the exact byte extent. Cdata lookup hashes only the
bounded exact candidate set; it does not scan an allocation-sized range.

The allocation fast path does not perform a locked read-modify-write for every
new cell. Fresh bump ranges are exclusive to their allocator owner, so the
implementation publishes zeroed side words in batches and uses an exclusive
release store for the common one-universe/no-worker READY transition. Shared
and fallback cases retain atomic compare/exchange publication.

## Constructor protocol

The common protocol is:

1. reserve physical storage with allocation identity set and READY clear;
2. keep a stable pending root which does not require decoding the unfinished
   header;
3. initialize the header and the complete typed prefix required by readers;
4. release-publish the exact base/interior identity and READY;
5. repair the active GC2 mark frontier with a retain-first root barrier;
6. publish the permanent semantic or ownership edge;
7. remove the temporary pending root.

Cancellation frees an unlinked pending allocation explicitly. It must not put a
partial object into an intrusive object list merely to reuse list lifetime.

### Bytecode prototypes

The bytecode reader reserves a stack anchor before allocating a prototype. The
anchor holds the pending object while READY is clear. KGC slots are filled with
release stores, and a release `sizekgc` publication exposes only the initialized
prefix to a concurrent traversal. The loader replaces or removes the anchor on
every success, malformed-bytecode, OOM, and protected-call exit.

All prototype `sizekgc` accesses now use acquire/release helpers. Raw field
access would allow a traversal to observe a length which outran initialized KGC
slots.

### Lua threads

New secondary `lua_State` objects remain READY-clear while their stack geometry
is initialized. A TG root anchor holds the pending thread without inserting a
partial thread into the live thread registry. Stack initialization has a
nothrow form so failure can free the pending state before the caller raises.

After READY and the root barrier, the constructor publishes the permanent
vmthread, callback-carrier, or Lua stack edge and removes the temporary anchor.

### Strings

Interned strings are traversable GC allocations rather than PLAIN raw arena
records. A string publishes READY before its canonical string-table entry.
Failed typed validation no longer falls back to a raw-memory registration test;
that fallback could turn an unfinished or reclaimed allocation into a string.

### Cdata

Variable cdata initializes the allocation base and interior `GCcdata` header,
publishes the interior identity plus READY, and only then links the object.
Retained cdata validation derives the expected payload size and alignment from
the authoritative CType snapshot. Small-arena lookup examines a bounded set of
candidate cells; huge lookup uses the exact HugeTab range record.

Fixed, variable, over-aligned, and huge layouts share the same rule: an address
must match the published header identity and its physical span must remain
allocated. A pre-CTState fixed-cdata exception exists only for the bootstrap
objects created before the CType state is available; it is not a general
plausible-header escape hatch.

Small-cdata extent is now independently authoritative. Validation requires
coverage on every expected cell, rejects an internal block/mark boundary,
requires coverage to stop at the exact end unless the next cell is a distinct
allocation/free-run boundary, and compares the encoded byte tail. Adjacent
cdata allocations are therefore distinguishable even when their coverage bits
are contiguous. CType metadata remains the semantic size source, but it cannot
by itself manufacture physical extent or header admission.

Sweep deliberately retains dead cdata coverage until it has selected the dead
span as a reusable free run. Reuse publication is the metadata-scrub point.
Both ordinary sweep rebuild callbacks now route every run through
`arena_set_free_run()` before linking it into an allocator bin. Previously only
the largest run chosen as a bump window took that route; non-largest runs went
directly through `arena_link_run_head()` and could carry stale READY/coverage
into a later allocation. `t-arena-sweep` fixes the schedule by placing dead
cdata in a non-largest run and checks every coverage bit before reuse. An
assert build also passes the exact `ffi.load("c")`/full-GC churn at 30 and 500
iterations.

## Native temporary semantic roots

Parser `LexState` descriptors now scan both `tokval` and `lookaheadval`, in
addition to their raw backing vectors. This covers numeric-literal INT64/UINT64
cdata which exists only in a native token slot across parser allocation or a
nested reader. The source parser's `chunkname` is a separate Lua-stack root;
the bytecode reader uses its own explicit chunk anchor, so neither is inferred
from the LexState token rule.

Table construction has a rooted API for callers which can cross another
allocation, wait, or C API handoff before publishing the table to a semantic
slot. The table header and side vectors are initialized, a TG anchor is filled,
READY is published, and the active root barrier runs before ownership-chain
publication. Production has no poll/ACK/yield in the earlier READY-clear,
nil-anchor interval. Direct internal call sites must publish their stack,
native, parent, or global handoff before later allocation. The current audit
covers library initialization, `table.pack`, debug active-line tables, parser
templates, bytecode templates, recursive deserialization and JIT unsinking.

Recursive buffer serialization and dictionary preparation now have local
`lj_vm_cpcall` boundaries. They checkpoint the table-generation pin state,
unwind exactly to the caller's depth/epoch on error, and rethrow. This is needed
because Lua's fast `pcall` path does not cross the outer C API wrapper. The
regression catches the serializer error in Lua and inspects pin state before
returning to that wrapper, at both depth zero and one nested outer pin.

Interpreted FFI calls and CLibrary symbol lookup use ordinary Lua-stack roots
for cdata which survives a later conversion, callback, stack growth, or cache
publication. The architecture-derived reservation replaces explicit C-call
shape matching; see `ffi-ccall-temporary-stack-roots-2026-07-12.md`.

The FFI `CTState.miscmap` edge is permanent native-root state, not merely a
module-initializer stack temporary. `luaopen_ffi()` repairs an already-running
cycle after release-publishing the pointer, and every later global-root scan
acquire-loads and retain-first marks the table before scanning its contents.
The metatype/miscmap stress keeps only CType/native references across repeated
full collections and validates the edge under concurrent metatype publication.

## IR constants and traced CNEW

Every current `lj_ir_kvalue()` KINT64 destination was classified. Public/debug
and snapshot destinations publish directly to the Lua stack. Unsinking may
materialize a local KINT64 key, but `lj_tab_set()` publishes/anchors the key
before its missing-key path can allocate or perform an L-aware wait. Optimizer,
recorder, and assembler locals only inspect or emit the materialized value in a
no-safepoint region. This is a proof over the current call graph, not permission
to add a yield-capable call between materialization and destination
publication; such a caller must add an explicit root.

Trace CNEW/CNEWI results can initially exist only in registers or trace spill
slots. Native entry publishes TG-local `jit_base`/positive trace vmstate, and
MARK-to-WEAK plus WEAK-to-SWEEP both reject `lj_tg_any_jit_active()`. Cycle
start performs `EXIT_TRACES` and keeps JLOOP entry closed until IDLE. Trace exit
keeps `jit_base` published through snapshot restoration, so the semantic Lua
stack edge exists before the native-only root disappears. Active-phase
allocations are black. This closes the protocol proof, but a deterministic
race fixture which pauses between CNEW and snapshot restoration remains a
release-quality validation gate.

## Retain-first semantic marking

Strong GC2 barriers and scanners no longer perform an observational type check
and then attempt to retain the object. Sweep can invalidate that ordering.
`lj_gc2_markobj_status()` first admits the physical allocation, then decodes the
published type and reports dead/already/new status. Graph-bearing objects are
queued for semantic traversal even when physical retention reports that the
current mark was already present in SWEEP.

The retain-first rule now covers:

- global and owner-local root barriers;
- table key/value and pair barriers;
- Lua stack, worker stack, upvalue, table-child, and thread-root TValue scans;
- retired trace KGC operands and prototype owners;
- FINREG generation tables and order nodes.

`markobj_nogrey` is terminally restricted to strings and the explicitly
synchronously traversed thread identity. It is not valid for an arbitrary
graph-bearing object.

## HugeTab BUSY/mark intent

A concurrent huge sweep can hold `BUSY` while a range marker discovers the
allocation. Waiting for BUSY would violate the nonblocking requirement, and
reading the payload while BUSY would race unmap/reuse. The ticket therefore has
a `MARK` intent bit:

- a range marker encountering `SWEEP_OLD|BUSY` atomically publishes MARK and
  returns `LJ_ARENA_HUGE_MARK_INTENT` without reading the payload;
- the unique retire owner returns 2 whenever its final TICKET contains MARK and
  discharges a semantic traversal before the mapping can disappear; and
- callers distinguish exact marked, absent/dead, and deferred-intent results.

HugeTab does not encode whether MARK existed before BUSY or arrived during the
opaque window. A pre-BUSY mark therefore requests the same discharge and may
duplicate a completed traversal. That conservative duplicate is preferable to
depending on unencoded provenance.

Deterministic pthread coverage forces the BUSY window and verifies that the
marker neither waits nor dereferences the protected payload.

`SWEEP_OLD|BUSY` without `TICKET` can also describe a raw HugeTab realloc claim.
Production traversable GC allocations are immutable and
`lj_mem_realloc()` rejects them, so this shape cannot hide a GC object graph:
the conservative marker records mapping liveness without reading a header and
no deferred traversal is required. If the claim is retirement instead, the
unique return-2 owner publishes `retire_obj` and discharges the traversal.
`t-arena-hugetab` covers the realloc-shaped mark-only state separately from the
paused retire-owner state.

## Trace retirement and the SMR writer

Retired trace bodies preserve KGC operands, start/snapshot prototype owners,
direct trace links, snapshot storage, and auxiliary exit tables for the stale
bytecode/native-exit grace interval. Ordinary publishers and root scans perform
this walk under a GC2 SMR reader lease.

The retire-list reclaimer is itself the exclusive SMR writer. Re-entering the
reader path from a requeue branch created a writer-self deadlock: the safepoint
leader waited for `smr_reclaiming` to clear while a blocked TG waited for that
leader to consume its acknowledgement. Reclaimer requeues now use a separate
path which asserts both exclusive writer context and the recorder token, and
walks the already detached body without acquiring a reader lease. Every normal
caller retains the reader-protected path.

The regression is covered by both the blocked-TG profiler test and the
concurrent VM-event flush test.

## FINREG raw and semantic roots

FINREG list nodes and tables combine raw allocation lifetime with semantic Lua
object lifetime. Publication and scanning therefore repair both domains:

- a newly allocated finalizer table remains stack-rooted through generation
  allocation and CAS publication;
- FinReg generations and order nodes receive raw marks before and after CAS
  publication;
- scanners retain/mark each raw node before loading `next`, `active`, table, or
  object fields;
- the referenced Lua table/cdata object is then marked semantically.

The same rule applies to GC2 userdata-finalizer active and retired node lists.
Those heads are authoritative raw roots even after a node becomes inactive;
neither `mem_registered()` nor a plausible node body is a retention operation.

## Reallocation boundary

Typed/traversable arena allocations have stable identity metadata. Generic
`lj_mem_realloc()` terminally rejects them: moving such an allocation would
publish a new address without atomically replacing all typed identities and
semantic roots. Callers which need movable storage must use a raw allocation
kind with an explicit publish/retire protocol.

Arbitrary user `lua_Alloc` callbacks remain intentionally disabled under
`LJ_GC2_INTERNAL_ALLOCATOR_ONLY`. The public ABI remains present, but
`lua_newstate()` ignores the callback and `lua_setallocf()` is a no-op while the
internal arena is the only enabled ownership domain. The exact temporary
behavior and re-enable gates are documented in
`notes/lua-alloc-temporarily-disabled-2026-07-10.md`.

This temporary exception does not enable the old collector. GC2 is the only
runtime collector.

## Current verification and performance

The combined implementation has passed clean static/dynamic builds and focused
Linux tests for arena publication, arena allocation, HugeTab BUSY intent,
variable/fixed/huge cdata allocation and lifetime, prototype dump/load, thread
construction, FINREG interpreted/JIT/trace/finalizer paths, blocked-TG profile
delivery, and concurrent JIT VM-event flush behavior. Cross/sanitizer gates are
rerun for each coherent commit rather than inferred from an earlier object
archive.

The final focused assertion checkpoint for this tranche passed
`t-arena-sweep`, `t-arena-hugetab`, the exact 30/500 iteration CLibrary churn,
the C and Lua parser-root fixtures, `t-tab-retire`, `t-tab-cas-store`,
`t-gc2-traverse`, the four-thread/120-metatype miscmap stress, the traced CNEW
worker fixture, and a library/table/recursive-buffer handoff smoke. The FFI
temporary-root slice separately passed Linux, Wine/Win64, and Darling/macOS;
the final combined tree still requires the cross-platform rerun named below.

On the focused 300,000-unique-string allocation benchmark, the READY/TLS fast
paths measured about 0.508--0.517 seconds versus 0.547--0.549 seconds for the
clean comparison build, roughly 6--7% faster. With eight allocator threads and
GC stopped, the candidate and comparison build are at parity (approximately
174--189 ms). With default GC scheduling the current candidate remains noisier
and slower (representative median about 684 ms versus 473 ms). That remaining
delta follows collector scheduling/sweep pressure, not per-allocation READY
publication. It is acceptable for the b1.2.0 functional gate only if it does not
degrade into pathological slowdown, but it remains a post-gate performance
target.

## Remaining project gates

Sweep-time semantic publication is no longer an open item.  The current
`lj_gc2_trace_sweep_root()` path falls back from SSB/grey publication to the
allocation-free recovery identity, and a classification failure makes reclaim
fail closed.  `m3_gc2_recovery` exercises the exact SWEEP/free/recovery schedule
in normal and assertion/paranoia builds; see
`gc2-no-drop-recovery-2026-07-12.md`.

1. Bound HugeTab tombstone accumulation with a nonblocking rebuild/rehash
   protocol. Long churn must not turn exact/range lookup into a table-sized
   probe.
2. Add the deterministic trace-CNEW/register-only race described above and
   keep the `lj_ir_kvalue()` destination audit mechanically current as new
   materialization call sites are added.
3. Re-audit persistent FFI side vectors/lists beyond the repaired interpreted
   call temporaries and CLibrary cache handoff for retain-before-dereference
   ordering, especially callback and CType growth/retirement structures.
4. Finish the exhaustive pending-constructor/OOM schedule audit. Tables,
   functions, prototypes, userdata, threads, strings and cdata now have local
   protocols, but the final project claim requires every traversable allocation
   site to be classified and fault-injected rather than inferred from type
   coverage.
5. Add/finish forged-header, weak fixed/VLA/over-aligned/huge cdata,
   pre-CTState, late-root, and pending-constructor/sweep adversarial schedules.
6. Complete Linux sanitizer/stress gates and the scoped Wine and Darling cross
   artifact/runtime gates on each final coherent release tree.
7. Restore arbitrary custom `lua_Alloc` only through the separately documented
   body-SMR/registry gates. Its current omission is explicitly temporary and
   does not permit the old collector to run.

The focused JIT/cdata race remains a `b1.2.0` blocker.  On 2026-07-12 exhaustive
FFI-side-structure coverage, every-allocation-site fault injection, bounded
HugeTab maintenance, and custom-allocator restoration were sequenced into
`b1.2.1`; see `b1.2.0-release-gate-2026-07-12.md`.
