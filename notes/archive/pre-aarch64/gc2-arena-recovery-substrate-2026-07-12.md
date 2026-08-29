# GC2 allocation-free arena recovery substrate

Date: 2026-07-12

This note records the arena-side substrate for lossless GC2 work publication
when a bounded SSB or grey queue cannot accept another object. It does not
change `plan/`. The recovery identity is allocation-free, independently
durable from ordinary mark/sweep metadata, and is a reclamation veto until its
exact traversal or terminal reconciliation completes.

## Packed state protocol

Small arena allocation starts and live HugeTab entries use the same four-state
protocol:

- `IDLE`: no recovery identity exists.
- `PENDING`: one counted identity is discoverable and may be claimed.
- `CLAIMED`: one recovery consumer owns the current traversal.
- `REDIRTY`: a producer published more work while that traversal was owned.

The normal transitions are:

```
IDLE -> PENDING -> CLAIMED -> IDLE
                         \-> REDIRTY -> PENDING
```

`PENDING -> CLAIMED` is the worker claim. A producer observing `CLAIMED`
changes it to `REDIRTY`; it does not create a second identity or increment the
count. Completion changes `CLAIMED -> IDLE` only if no redirty won. A
`REDIRTY -> PENDING` completion keeps the existing identity/count and requires
another bounded drain. All loads are acquire and all successful state CASes
are acquire/release.

Only allocation starts may leave `IDLE`. Finding a base/start, proving READY
publication, and retaining the containing arena/table owner through the state
CAS remain GC2 admission responsibilities. An iterator snapshot alone is not
permission to read object bytes: the consumer must first win
`PENDING -> CLAIMED`.

## Exact count contract

GC2's global recovery count is the closure and shutdown authority. Publication
from `IDLE` follows a reserve-before-CAS transaction:

1. increment the exact global recovery count;
2. CAS the side state from `IDLE` to `PENDING`;
3. on CAS loss, immediately roll the reservation back and retry/classify the
   observed state;
4. on success, wake collector progress.

The count may therefore temporarily exceed the number of visible side-state
identities while a producer is between steps 1 and 2. MARK/WEAK/SWEEP closure
must treat a nonzero count as work even if an arena scan is momentarily empty.
Completion decrements exactly once after `CLAIMED -> IDLE`, or after the huge
completion API atomically consumes `CLAIMED` into a terminal sweep handoff.
`REDIRTY -> PENDING` and BUSY requeue do not change the count. Terminal discard
counts each successfully reconciled main, small, or huge identity, compares it
with the global count, and must abort/pin reclamation on disagreement rather
than erase an unaccounted locator.

## Small arenas

`GCArena.recovery[]` is a separate packed two-bit plane: 32 cell states per
64-bit word, 128 words/1024 bytes per 64 KiB arena. Adding it moves
`LJ_AFIRST_CELL` from 232 to 296 (64 fewer 16-byte allocation cells). The
ordinary allocation fast path does not load the plane; sweep/rebuild and
physical-free boundaries do.

A non-`IDLE` start is treated as live independently of the current mark bit:

- sweep bitmap conversion retains its block boundary;
- mark clearing preserves its carried mark;
- free-run scanning and live-cell counting treat it as allocated;
- quarantine apply/rebuild cannot publish it as reusable;
- bin validation, allocation reuse, and free-run publication reject it;
- direct/list teardown retains the arena rather than silently unmapping it.

All logical small frees that meet non-`IDLE` recovery publish the allocation's
`late[]` bit before returning owned. This includes direct free, remote/deferred
free, and the early `lj_arena_quarantine_owns_body()` path used by
`arena_allocf_free()` and `lj_arena_free_deferred()`. Recovery retains the
bytes; `late[]` remembers that they must become reusable after recovery releases
them. `lj_arena_recovery_complete_wake()` is allocation-free and is called only
after a successful `CLAIMED -> IDLE`; it rearms ordinary sweep/late progress
without changing recovery ownership or object bytes.

## Huge mappings

Huge recovery state lives in the entry's atomic 128-bit `{address, metadata}`
slot, because a huge mapping has only `GCAhdr` and no small-arena side planes.
The two recovery bits are metadata bits 11-12. `LJ_HUGEF_DEFER_FREE` is bit 13,
so the packed logical-size shift changes from 11 to 14 and the complete flag
mask remains the exact low 14 bits. Existing practical x64 mapping sizes remain
well below the reduced encoding bound.

Initial `IDLE -> PENDING` requires an exact READY, traversable entry. It rejects
`FREEING`, deferred-free intent, and non-sweep BUSY/realloc ownership. Every
non-`IDLE` target forces `MARK` and clears `RETIRED`, while preserving every
unrelated metadata bit. Clear-marks, retirement, finish-sweep, delete, realloc,
external-free completion, ordinary terminal forget, and table teardown either
preserve or reject recovery state; none silently clears it.

`lj_arena_hugetab_next()` enumerates every live entry and proves an
instantaneous full-slot snapshot with a no-op 128-bit CAS. It is the general
primitive used for the authoritative small-arena registry.
`lj_arena_hugetab_recovery_next()` filters those snapshots to non-`IDLE`
entries. A recovery consumer must still claim the exact state before reading
the mapping.

### Logical free during huge recovery

A HugeTab external free racing any non-`IDLE` recovery state atomically ORs
`LJ_HUGEF_DEFER_FREE` into the same entry before returning a non-claim result.
Thus `arena_allocf_free()` may relinquish logical ownership without overwriting
or leaking the recovery-owned mapping. Duplicate frees observe the same durable
intent. Generic recovery state CAS deliberately refuses a transition to
`IDLE` while `DEFER_FREE` remains, making `IDLE|DEFER_FREE` unrepresentable.

`lj_arena_hugetab_recovery_complete()` consumes a `CLAIMED` identity:

- without `DEFER_FREE`, it returns `COMPLETE_LIVE` after atomically clearing
  the recovery bits;
- with `DEFER_FREE` plus a transient BUSY retire owner, it changes the state
  back to `PENDING` and returns `COMPLETE_REQUEUED`, preserving the same count;
- otherwise it release-publishes `retire_epoch = UINT64_MAX`, atomically clears
  recovery/deferred/mark/retired ownership, sets `FREEING|SWEEP_OLD`, and
  returns `COMPLETE_SWEEP`.

The last case always takes a fresh sweep grace, even if the entry was not
previously `SWEEP_OLD`. Recovery drain itself holds an SMR read lease and other
admitted readers may still name the payload, so immediate unmap is not a valid
runtime completion. `COMPLETE_UNMAP` is reserved defensively but is not emitted
by the current implementation.

### Transfer and terminal reconciliation

Huge-table transfer refuses every source entry with non-`IDLE` recovery and
reports the overall transfer incomplete. An earlier non-recovery prefix may
already have moved through the existing transactional per-entry protocol, but
the recovery-bearing entry is never copied or tombstoned. There is deliberately
no implicit MOVING protocol: copying the recovery bits would create two
apparent owners for one global count. The source remains authoritative for that
mapping until recovery drains, then an ordinary retry can transfer it.

After all mutators and GC workers have joined,
`lj_arena_hugetab_recovery_discard_terminal()` provides the only terminal
override:

- an ordinary recovery identity is atomically cleared (`TERMINAL_CLEARED`) and
  the mapping remains for normal terminal table/freeall teardown;
- a recovery identity carrying `DEFER_FREE` is atomically tombstoned
  (`TERMINAL_UNMAP`) and the caller receives `hi.size` and sole physical-unmap
  ownership;
- a missing/already reconciled identity returns `TERMINAL_LOST`.

This full-slot terminal transition may supersede PENDING, CLAIMED, REDIRTY, or
BUSY only because all publishers/consumers are joined. Ordinary
`hugetab_fini_all()` and small-arena unmap continue to retain non-`IDLE` work
until normal completion or this explicit terminal reconciliation.

## Validated gates

The following x86_64 Linux standalone arena cases pass with
`-O2 -Wall -Wextra -Werror -mcx16`:

- `m2_arena_bitmap`
- `m2_arena_publication`
- `m2_arena_map` (small and huge mapping fixtures)
- `m2_arena_alloc` (alloc, realloc, and lua_Alloc shim)
- `m2_arena_hugetab`
- `m2_arena_sweep`

Focused regressions cover packed small-state transitions, mark-zero sweep
retention, rebuild/unmap vetoes, direct/allocf/deferred small frees setting
`late[]`, huge iterator/state transitions, delete/realloc/transfer/fini vetoes,
actual lua_Alloc huge deferred-free publication, live and fresh-grace
completion, a paused BUSY retirement forcing requeue, and terminal clear versus
tombstone/unmap. `git diff --check` also passes for the arena and focused-test
files.

## Unresolved integration and platform work

The arena substrate and standalone gates do not by themselves prove the full
GC2 recovery feature. Remaining work includes:

- build and run the complete VM with the GC2 count/publication/drain/terminal
  integration in release, assert, and paranoia configurations;
- deterministic OOM injection for SSB rotation and grey-deque growth, including
  the reserve-before-side-CAS window and source-removal ordering;
- deterministic end-to-end `CLAIMED -> REDIRTY -> PENDING` traversal proving a
  newly published edge is observed on the second bounded drain;
- closure tests proving the nonzero reserved count blocks MARK, WEAK, SWEEP,
  and worker-park decisions even before its side-state CAS is visible;
- shutdown reconciliation tests spanning main-thread, small-registry, huge,
  deferred-free, dead-TG transfer, and terminal count equality;
- full GC/JIT/FFI suites, long multithread stress, sanitizer/diagnostic runs,
  and measurement of the extra small-sweep recovery loads and 1 KiB arena
  metadata cost;
- native macOS and Windows validation, plus the requested Darling and Wine
  gates. Only x86_64 Linux standalone arena fixtures have been run for this
  tranche.

Until those gates pass, the recovery substrate should be treated as integrated
work in progress rather than a release-complete proof of lossless GC2 queue
overflow handling.
