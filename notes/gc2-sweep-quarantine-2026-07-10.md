# GC2 bounded sweep quarantine and ownership bridge (2026-07-10)

This note records the implemented divergence from the draft sweep plan. The
plan files themselves are unchanged.

## Root bridge

The GC ownership spine is pruned in bounded batches. Every old traversable,
non-fixed, non-finalizer header is detached before arena destruction starts.
The detach does not rewrite the object's `gcw`: pre-grace spine readers may
still be following that link. The exact allocation is classified in a two-bit
per-cell sidecar:

- `WHITE`: no detached GC header (raw/fixed storage, or a reanchored survivor),
- `LIVE`: exact detached header requiring post-grace reanchor,
- `RETIRED`: exact detached header whose destructor is pending,
- `FREEING`: destructor/physical-free ownership has won; bitmap reuse remains
  deferred to the arena owner.

`WHITE -> RETIRED` is the death claim. A concurrent mark rescues only
`RETIRED -> LIVE`; `FREEING` is terminal. `reclaim_deferred` is incremented
before RETIRED publication and decremented by the winning rescue or physical
completion, so terminal bitmap publication cannot pass a destructor still in
flight.

After a complete root/SSB handshake and a zero-reader SMR gate, the bounded
reclaimer reconstructs the exact header (including variable-offset cdata),
links each `LIVE` header exactly once, and runs or delegates each `RETIRED`
destructor. Trace headers remain RETIRED until the recorder-token retire path
release-publishes `gct == 0`; removing a trace slot is not physical completion.

## Huge mappings

One huge mapping contains one allocation. Its metadata now has an explicit
`TICKET` bit. `retire_obj` is valid only while TICKET is published:

- unmarked detach publishes `RETIRED|TICKET`,
- marked detach, or a marker winning the retirement CAS, publishes
  `MARK|TICKET`,
- a marker may clear RETIRED but never TICKET,
- sweep finish refuses TICKET,
- post-grace reanchor claims `BUSY`, links the exact root, clears `retire_obj`,
  and atomically clears `BUSY|TICKET`.

This makes the former implicit marked/non-retired `retire_obj` rescue state
explicit. It is not stale destructor payload and cannot be cleared by a losing
retire attempt or by sweep finish.

`BUSY` also closes the complete huge external-free state race, rather than only
the old lookup-to-header-store window. Free/realloc classifies the old address
from the allocator-supplied old size before any header access. A single
128-bit `{address, metadata}` CAS then chooses the prepare-vs-free ordering:

- if `BUSY` wins before `SWEEP_OLD`, that owner is the only table deleter;
- if `SWEEP_OLD` wins first, `FREEING|BUSY` pins the mapping while the fresh
  grace sentinel is release-published, and completion can only hand it to the
  sole sweep deleter;
- a duplicate or stale free which finds BUSY, FREEING, or a tombstone never
  reads the mapping and never unmaps it;
- huge realloc first takes a nonterminal BUSY pin, preserving the same-mapping
  O(1) resize path and protecting replacement copies. It atomically converts
  that pin to terminal FREEING ownership only after the copy.

Retirement now takes BUSY before writing `retire_obj` or `retire_epoch`, too.
Thus a losing retire cannot write through a deleted slot or overwrite an
external free's fresh-grace sentinel. The sole sweep deleter never reads or
unmaps a BUSY entry.

## Small-arena terminal admission

The previous `remote_active == 0` check was not a terminal LP: a producer could
increment immediately afterward and publish after state-to-bitmap conversion.
The owner now CASes `remote_active` from zero to a CLOSED high-bit state after a
read-only readiness check. An ordinary producer either increments before that
CAS (so close fails) or observes CLOSED.

A CLOSED producer:

1. CAS-increments the low active count under CLOSED,
2. deduplicates the allocation start in a separate 512-byte `late[]` bitmap,
3. release-pushes an exact `{start,len}` record to the arena queue,
4. decrements the closed-gate active count.

It does not touch `sweep[]`, `block[]`, or `mark[]` during terminal conversion.
The arena stays mapped and cannot be reused while on the reclaimed stack.
Adoption partitions the late records against the stable committed bitmap. A
record for a cell commit already made free is a duplicate, so adoption clears
its `late[]` bit and drops it. A record for a still-allocated cell is retained,
including after the arena reopens and rejoins the owned list; ordinary queue
drains requeue that size-bearing record without clearing its allocation bit.
An ordinary remote publication checks the persistent `late[]` bit before
changing sweep state or writing its intrusive node, so a duplicate free cannot
rewrite the already-linked node into a queue cycle.
The next sweep resets `sweep[]`, drains the retained record to `FREEING`, and
clears its `late[]` bit before publishing NEEDSWEEP. Root pruning can then
detach any remaining tombstone, and that new cycle's grace covers bitmap reuse.

Adoption can CAS CLOSED to OPEN only with zero admitted late producers, but a
retained record itself does not prevent reopening because its committed
allocation bit pins the body. A producer racing the OPEN CAS either makes it
fail or reloads OPEN and takes the normal remote route. Thus raw/opaque
physical frees are eventually materialized, never receive same-generation
reuse after their physical free began too late for the old grace, and are not
silently leaked.

The rare CLOSED-to-OPEN observation race is retried iteratively in both
quarantine ownership and remote-free publication. This is an ordinary
lock-free retry: every failed attempt observed an owner generation complete,
and no retry grows the C stack or loses the physical-free record. An individual
producer may starve under adversarial repeated full-cycle turnover, just as it
may in the surrounding CAS loops, but the system continues to make progress
and no body is leaked as a fallback.

The extra bitmap moves `LJ_AFIRST_CELL` from 136 to 168, costing 512 bytes per
64 KiB arena (0.78% of arena capacity). It avoids heap allocation, locks, and
unbounded external record objects on the late-free path.

## Remaining work

- **P0: terminal rescue admission.** `remote_active` currently closes
  destructor/remote-free publishers, but a sweep-time mark or
  `RETIRED -> LIVE` rescue does not enter that arena gate. Such a publisher can
  pass the readiness scan and race bitmap commit/sidecar reset, allowing a
  rescued live body to be classified free. Extend terminal admission to every
  mark/rescue publisher, close before the stable readiness check, revalidate
  while closed, and give a CLOSED late rescue a discoverable nonwaiting pin;
  do not drop it. Until this lands, the quarantine is a WIP checkpoint and is
  not a proof of fully thread-safe GC reuse.
- **P0: bitmap memory model and mark generations.** Several `block[]` and
  `mark[]` accesses remain plain C loads/stores while GC workers use atomic bit
  operations. Besides formal data-race UB, a plain word RMW can lose an
  unrelated concurrent mark. Atomic word access is necessary but insufficient:
  mark reset currently runs before the cycle-start barrier/black-allocation
  handshake and can erase a birth/rescue mark. Move reset to a generation-tagged
  or double-buffered lazy initialization protocol, while retaining the proven
  single-observer VM fast-path exception.
- RESET_ALLOC still visits every owned traversable arena at a safepoint. Replace
  this with generation detachment/lazy sidecar initialization to make reset
  O(1) in heap size.
- The huge table is fixed-capacity and linearly scanned for bounded sweep work;
  resizing/segmented indexing remains a scalability task.
- Cross-platform sanitizer/TSAN-style stress and full macOS/Windows runtime
  coverage remain checkpoint validation work.

Focused deterministic fixtures cover both marker/retire CAS orderings for huge
tickets, both prepare-before-free and free-before-prepare terminal orderings,
duplicate/stale huge free and realloc rejection without header access, the
same-mapping realloc fast path, rejection of same-generation CLOSED late-free
reuse, reuse after the next sweep/grace, and late-dedup clearing for a cell
already freed by committed bitmap conversion.
