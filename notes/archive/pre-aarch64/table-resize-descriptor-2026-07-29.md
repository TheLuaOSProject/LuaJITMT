# Persistent table-resize descriptor substrate

Date: 2026-07-29

This Linux/x86-64 checkpoint establishes the identity, publication, lifetime,
GC-rooting, table-control, and VM-exclusion substrate required by a future
helpable resize. It deliberately does not switch `lj_tab_resize()` to the new
protocol yet. Actual vector allocation, per-slot migration, live marker
publication, and successor cutover remain on the critical path.

The implementation is distinct from GC2's existing table-rescan descriptor.
That descriptor records GC scanning work. `TabResizeDesc` records one
structural table generation and is intended to become the durable owner of a
resize transaction.

Custom `lua_Alloc` compatibility is intentionally omitted from this tranche.
macOS and Windows validation and fixes remain deferred until b1.2.1 is
otherwise ready for release.

## Identity and marker namespace

Each `global_State` has a cold `TabResizeState` registry and a monotonically
allocated descriptor id. Zero is never issued, ids are not reused within the
Lua universe, and exhaustion fails instead of wrapping.

Four internal lightuserdata marker kinds are reserved:

- source intent;
- destination intent;
- completed move; and
- completed nil move.

The marker payload contains the per-universe descriptor id, not an address.
Kind 7 remains invalid, and resize markers remain disjoint from the existing
generic `FORWARD` and `KEYLOCK` encodings. Registry lookup for such an id is
valid only under an SMR reader and with the exact `{global_State, table, id}`
identity.

## Descriptor and table-control protocol

`GCtab` now exposes one 64-bit structural control word. A stable word contains
the exact `{allocated array capacity, structural owner}` pair; a tagged word
names an installed `TabResizeDesc`. The former `acap` and `struct_owner`
members remain layout-compatible views of the stable pair. Table initialization
publishes both fields in one store.

The descriptor phase machine is:

1. `PREPARED`
2. `INSTALLING`
3. `INSTALLED`
4. `RETIRING`
5. `MIGRATING`
6. `PUBLISHING`
7. `CLEARING`
8. `TERMINATING`
9. `TERMINAL`

The reserve caller exclusively owns a raw `PREPARED` record. The winner of the
`PREPARED -> INSTALLING` CAS is the only authority allowed to initialize the
immutable stable-control snapshot and issue the first table-control CAS.
Registry publication happens before table-control publication, so no marker or
tagged table can name an undiscoverable record. A delayed installer cannot
reinstall the same descriptor after cancellation because every phase is
monotonic and the initial control CAS is issued at most once.

After control names the descriptor, the old array/hash roots and exact
asize/hmask/capacity facts are captured with bounded revalidation. GC root
scanning can help finish or cancel this snapshot. Structural mutation helpers
recognize descriptor control and help terminal phases instead of waiting for a
missing owner.

Discard never directly frees a private record. It claims `PREPARED`, publishes
the record, and terminalizes it through the same registry/reclamation path.
This closes the race where an installer already holds the raw pointer but is
paused before its phase CAS.

## Capacity and weak-state cutover

During descriptor control, allocated array capacity lives in reserved bits of
the colocated `weak_record` word. Semantic weak cycle/state loads, stores, and
CAS operations mask those bits and preserve the capacity shadow.

`CLEARING` uses `cmpxchg16b` over the adjacent structural-control and
weak-record words. It validates that the descriptor still controls the table
while publishing the target capacity shadow and preserving the newest weak
cycle/state. A monotonic cutover flag records exact provenance; seeing the same
capacity in a later stable generation is not accepted as proof that an older
descriptor performed the change. Control is detached only after the
descriptor has entered a terminalizing phase.

The current cutover changes only descriptor control and capacity metadata. It
does not publish replacement array/hash vectors, so it is a substrate test and
not a production resize.

## GC and SMR lifetime

Registry membership owns both the raw descriptor allocation and a semantic
edge to its table. GC2 scans the registry, marks each descriptor, retains its
table, checks the chain, and helps incomplete publication/termination where
safe. Dropping every ordinary Lua reference therefore cannot collect a table
while a descriptor or marker can still name it.

Terminal records remain discoverable until the existing table-retirement epoch
and oldest-reader grace both permit physical reclamation. The exact reclaimer
detaches the registry, validates every raw allocation/table edge, repairs a
missed VM-guard release, and frees eligible records. Shutdown detaches any
remaining descriptor control, releases every guard exactly once, frees the
registry, and asserts that no guard count survives.

Install and discard use a tracked, nonblocking SMR admission. A normal leaf
reader may be counted in an independent Lua universe without replacing the
current thread's tracked universe. Descriptor publication can call deeper
trace/GC readers, so accepting that fallback could make a later nested reader
self-wait behind its own untracked count. Tracked admission instead refuses
the cross-universe case while leaving `PREPARED` caller-owned and retryable.

## Zero-cost stable VM guard

An installed descriptor must invalidate both interpreter-private table
mutation and pre-MT JIT traces that contain raw table stores. Adding a
per-table comparison to every x64 store was measured and rejected.

The selected design packs two meanings into the existing `mt_active` word:

- bit 0 remains the sticky “a secondary Lua thread has existed” latch; and
- bits 1 through 31 form a saturating count of installed descriptor guards.

The x64 VM already tests the raw word before using private table paths.
Consequently a live descriptor reuses the existing branch and adds no
instruction to the zero-word fast path. Semantic thread-latch accessors mask
bit 0, while private-mutation and recorder predicates intentionally test the
raw word.

The sole installer increments the guard before publishing tagged table
control. If the sticky MT latch is still clear, it takes the JIT token only
with a bounded try and flushes all pre-guard traces through a callback-free,
eventless path. It refuses an active recorder, lifecycle-only token ownership,
or a concurrent first-MT transition instead of waiting. Every latch-zero
installer performs this serialization because another installer can be paused
between count publication and its own flush.

The matching decrement occurs only after table control is stable again. A
monotonic per-descriptor release claim makes terminal helpers, the exact
reclaimer, and shutdown idempotent. Overflow refuses installation and
underflow cannot corrupt the sticky latch.

C table mutations, `table.clear`, resize entry, and `TSETM` route through their
shared/helper paths while the raw guard is nonzero. `TSETV`, `TSETS`, `TSETB`,
and `TSETR` retain their previous x64 instruction shape.

## Deterministic coverage

`m5_tab_resize_descriptor` covers:

- all four marker kinds, maximum id, invalid zero/kind encodings, and
  separation from `FORWARD`/`KEYLOCK`;
- packed sticky-latch/count behavior, overlap, saturation, and release;
- sole-install authority, a reentrant competing installer, discard before the
  install claim, and publication/control pause points;
- invalid snapshot cancellation and delayed-control displacement without
  descriptor ABA;
- immutable old-root capture, stale/displaced capacity clear before and after
  the pair CAS, exact cutover provenance, and preservation of semantic weak
  state;
- GC scanner help, structural-entry help, table mutation gates, and
  `table.clear`/resize replay;
- a real pre-MT raw-store trace which is flushed before descriptor control;
- active-recorder and same-TG lifecycle-token refusal without waits or state
  corruption;
- VM `TSETB`/`TSETS` helper routing, overlapping descriptor guard counts, and
  exact cleanup;
- independent-universe tracked-SMR refusal for both install and discard;
- registry retention after all ordinary table roots are dropped; and
- terminal epoch/oldest-reader reclamation, including a deliberately retained
  reader, plus `lua_close()` with descriptor control and one VM guard still
  active.

The strengthened `m5_x64_tset_forward` fixture separately exercises stable
current-generation repair through `TSETB`, `TSETV`, generated `TSETR`, and
`TSETM`, while retaining the retired source generation with a real reader.
Weak-table array and hash terminal classification are both covered in the GC2
traversal fixture.

Final Linux evidence:

- descriptor, thread-substrate, and x64 `FORWARD` fixtures pass;
- structural-owner, array-publication, empty-inline-`TNEW`, weak-resize,
  public weak-window, and weak-metatable bridge gates pass;
- the complete `m8_weak` suite passes in normal and assertion/paranoia
  profiles;
- GCC and Clang assertion/helper builds pass with `-Wall -Werror`; focused C
  fixtures compile with `-Wall -Wextra -Werror`;
- Clang ASan and repository-style UBSan builds pass both the descriptor and
  x64 `FORWARD` fixtures; and
- `git diff --check` passes.

## Performance evidence

An experimental per-table descriptor comparison in the x64 store guard moved a
pinned stable-store median from 2.068344 s to 2.123134 s, a 2.65% regression.
That design was removed in favor of the packed global word.

Core-pinned four-round HEAD/latest/latest/HEAD measurements found:

- stable interpreter array/hash stores: latest median 1.990257 s versus HEAD
  1.999872 s, or -0.48%;
- one million table constructors: 0.367371 s versus 0.370090 s, or -0.73%;
- negative-integer-key growth with repeated resize: 0.531522 s versus
  0.527137 s, or +0.83%; retired instructions increased only 0.136%, which is
  the bounded cold structural cost; and
- the representative JIT table-store trace is identical at 370 bytes and 71
  machine instructions.

Against an isolated build with the descriptor VM guard removed, generated
`lj_vm.S` and the complete `lj_vm.o` are byte-for-byte identical.
`TSETV`/`TSETS`/`TSETB`/`TSETR` therefore pay exactly zero added instructions;
`TSETM` already uses its C helper. The small wins above are treated as noise,
not as a performance improvement claim. The evidence supports no stable-path
regression and a small, cold resize-path cost.

## Residual production work

The paused-resize-owner bug remains open because production `lj_tab_resize()`
still uses the legacy owner-oriented copy path. No runtime code publishes the
new resize marker kinds yet. A source value can therefore still become generic
`FORWARD` while its only payload copy is in the resize owner's C local.

Production enablement should not advance directly from today's `INSTALLED`
phase into retirement. It needs a `READY` boundary after every fallible action
has completed and every resource is GC-visible. Before `READY`, the descriptor
must own at least:

- target array/hash sizes and capacity;
- allocated successor array/hash vectors;
- fixed migration-intent storage and exact completion accounting;
- durable source/destination tokens and any collectable payload roots; and
- cleanup ownership for cancellation.

From `READY` through retirement, migration, cutover, and terminal cleanup must
be allocation-free, bounded, and helpable. Per-slot states need immutable
identity and exact expected-value CAS transitions; a token cannot be reused
after `DONE` or cancellation. GC scanning must retain old and successor
vectors plus intent payloads without accidentally turning weak entries into
strong roots.

The production vector swap also needs a grace proof before dropping the
universe-wide VM guard. Descriptor control exclusion is enough for this
metadata-only checkpoint, but generated/interpreter code may have already
sampled an old vector before a future swap. Old vectors must remain descriptor
owned through the table-reader SMR grace, and native execution may require an
explicit safepoint/VM grace before guard release.

A latch-zero descriptor must not be installed from currently executing native
trace code. The eventual runtime integration must side-exit first or already
have the sticky MT latch. Repeated pre-MT installs may otherwise continue to
use the bounded full-trace flush described above; production integration should
also investigate safe amortization without weakening that invariant.

FINREG remains a separate transaction. Its `KEYLOCK`/`FINCLAIM`, ordered
registry linkage, and finalizer effects cannot be made helpable merely by
finishing the ordinary resize descriptor.
