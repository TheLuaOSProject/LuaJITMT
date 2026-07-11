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

`GCAhdr.remote_active` is now a naturally aligned 64-bit atomic containing
`CLOSED`, `SEALED`, `PENDING`, and a 61-bit publisher count. `remote_free`
remains at offset 64, `GCAhdr` remains 128 bytes, and the allocator block still
starts at offset 128. Exhausting the count is unreachable on supported x64
address spaces and aborts rather than silently dropping a lifetime intent.

Every small-arena mark/rescue and physical-free publisher joins the gate,
including the OPEN fast generation. Before terminal commit, a producer which
observes CLOSED or SEALED atomically publishes count plus PENDING before its
bit/status intent. The owner may clear PENDING only from an exact zero-count
word. Terminal commit is exact `CLOSED|SEALED -> SEALED`; a producer winning
any observation-to-CAS race defeats the commit.

OPEN remote frees retain the existing intrusive exact-size queue. A
terminal/grace-late free instead publishes only the allocation-start bit in
`late[]`; it never overwrites the still-SMR-visible body. That bit pins block1
through the current generation. A later PREPSWEEP consumes it to FREEING before
a fresh grace, after which bitmap reuse is legal. The last counted publisher
release edge wakes the sweep worker.

After the terminal LP, rescue is read-only. Terminal apply release-publishes
block decisions before resetting sidecar states to WHITE. A committed reader
samples state and then block, rejecting either old FREEING or new block0, and
does not write a mark that apply could lose.

Reclaimed adoption and abort restore seal a quiescent generation, take their
exact clean LP before bitmap/bin mutation, build free-run heads privately, and
open only through exact `SEALED -> 0`. A publisher which wins after the clean
LP defeats OPEN; owner-visible staging is rolled back and the arena remains
CLOSED. Restore re-pins every PREPSWEEP block1 FREEING start for a fresh grace,
because the late-bit exchange has already erased provenance. None of these
paths waits for a publisher.

Pointer plus size is not an allocation-generation tag. The terminal protocol
therefore assumes one exact-size deallocation ticket per allocation generation.
Internal retirement producers must prove that ownership or add a generation
tag; FREEING can deduplicate overlapping calls in one generation but cannot
repair an invalid delayed duplicate after address reuse.

The extra bitmap moves `LJ_AFIRST_CELL` from 136 to 168, costing 512 bytes per
64 KiB arena (0.78% of arena capacity). It avoids heap allocation, locks, and
unbounded external record objects on the late-free path.

## Remaining work

- **P0: bitmap memory model and mark generations.** Several `block[]` and
  `mark[]` accesses remain plain C loads/stores while GC workers use atomic bit
  operations. Besides formal data-race UB, a plain word RMW can lose an
  unrelated concurrent mark. Atomic word access is necessary but insufficient:
  mark reset currently runs before the cycle-start barrier/black-allocation
  handshake and can erase a birth/rescue mark. Move reset to a generation-tagged
  or double-buffered lazy initialization protocol, while retaining the proven
  single-observer VM fast-path exception.
- **P0: retained cdata object views.** Variable/over-aligned small cdata may
  have an interior header on a block0 extent, and huge cdata validation can
  race unmap before an exact containing-slot mark. Add bounded small containing-
  start lookup, atomic huge mark-containing publication, and a sweepable
  graphless cdata allocation class.
- **P0: HugeTab lifetime during ownership transfer.** MARK publication and
  source-to-destination slot transfer/delete need one quiescent or MOVING
  protocol so a copied/deleted slot cannot lose a concurrent mark.
- **P0: remaining mark-lifetime callers.** Weak/FINREG and table/raw snapshot
  paths must consume retained tri-state views rather than validate or query a
  mark and then dereference after reuse. The active audit is recorded in
  `notes/gc2-mark-status-lifetime-audit-2026-07-11.md`.
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
same-mapping realloc fast path, rejection of same-generation terminal late-free
reuse, reuse after the next sweep/grace, EOF LIVE rearming, exact terminal
commit/open arbitration, and rollback after a publisher defeats owner OPEN.
