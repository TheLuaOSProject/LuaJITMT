# GC2 allocation-span and userdata checkpoint (2026-07-11)

Status: implemented checkpoint, not a claim that GC2 or the lockless runtime is
complete. The plan files are unchanged.

## Live-thread corruption diagnosis

A long worker/JIT stress hang was not a futex or join-state failure. GDB showed
that a live `UDTYPE_THREAD` userdata at arena cell 2687 had a 144-byte physical
extent, but valid strings had subsequently been allocated at cells 2690 and
2693 inside that extent. `threading.join` then interpreted the first string as
the `LJThread` payload and waited on bytes which were no longer a thread state.

The corruption came from the arena boundary map. A sweep/rebuild can coalesce
adjacent state-1 free boundaries into one run. Allocation published the new run
head but did not clear old interior `block[]`/`mark[]` boundaries. A later
rebuild therefore treated an interior suffix of a live allocation as a free
run. The same problem applied to a coalesced run handed to specialized
C/VM/JIT bump constructors: those constructors publish their own object starts,
but the old boundaries were still present before the first publication.

The allocator now normalizes the whole extent before publishing an allocation
start. `arena_set_alloc()` receives `ncells` and clears every interior boundary
for both bin and ordinary bump allocations. `lj_arena_reserve_bump()` clears
the complete private coalesced run before handing it to specialized publishers,
and the closure/upvalue publisher in `lj_func.c` is span-aware too. This adds no
lock, wait, allocation, or global synchronization; the cells are owner-private
at each normalization point.

`tests/t-arena-sweep.c` now covers both forms: adjacent free boundaries must be
coalesced without leaving an interior reusable start, and a reserved bump
window must contain only the starts subsequently installed by its caller.

## All userdata is a GC2 traversable allocation

All internal `GCudata` is now allocated through the unlinked traversable GC
object path. This is an intentional implementation divergence from the earlier
plain-arena classification, and it applies to ordinary userdata as well as
thread, channel, buffer, and FFI-library userdata.

The payload of ordinary userdata remains opaque. The traversed graph consists
of the userdata header roots (`env` and metatable) plus type-specific internal
roots where defined. The reasons for making this universal are structural:

- every userdata header can carry GC edges;
- channel slots, thread child/start roots, buffers, and FFI libraries add
  internal edges which cannot safely depend on synchronous root-side reads;
- huge userdata must have the same exact HugeTab/traversable identity as other
  internal GC objects;
- userdata must participate in normal GC2 sweep and deferred physical reclaim
  rather than survive until terminal shutdown.

Root discovery marks and publishes userdata through the SSB. A GC2 worker owns
payload validation and semantic traversal. `lj_udata_free()` performs the
type-specific destructor first and then uses GC2 deferred object reclaim, with
the arena free fallback retained for shutdown/unsupported contexts. Thread
userdata cannot be freed while its native live-list node is still published.

Validation was tightened with the classification change:

- `sizeof(GCudata) + payload` overflow is rejected;
- the public userdata limit includes the header;
- channel fields are not read until the payload is at least `sizeof(LJChan)`;
- small-object size must fit before the next exact allocation boundary;
- huge-object size is checked against the authoritative live HugeTab entry;
- internal huge userdata no longer bypasses exact traversable registration.

## Native thread-root lifetime

`LJThreadLive` nodes are raw arena roots which a GC2 scanner can hold under a
retained read scope. Threading shutdown now only tombstones/unlinks them while
collector workers may exist. `close_state()` stops and joins the GC2 worker
pool before `lj_threading_live_free_all()` physically releases the nodes. This
ordering is mandatory even though the active list is already empty: freeing a
tombstone before the last reader exits would reintroduce a use-after-free.

## Test evidence and remaining gates

The focused arena sweep regression passes. Repeated ASan worker stress passed
with JIT both disabled and enabled, and repeated suite enumeration passed after
the reserved-bump normalization fixed the second manifestation. The GC2 phase,
mark-bit, no-legacy-runtime, internal-allocator-only, arena-close, arena-sweep,
and arena-phase gates have also passed during this checkpoint. A complete
scaffold, sanitizer, stock, Wine, and Darling pass is still required before
this work can be treated as a release candidate.

Custom `lua_Alloc` callbacks remain deliberately ignored under the temporary
policy documented in `notes/lua-alloc-temporarily-disabled-2026-07-10.md`.
This checkpoint neither restores that API behavior nor reintroduces the old
collector. GC2 remains the only runtime collector.
