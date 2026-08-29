# Arena transient-gate false OOM fix

Date: 2026-07-14

## Release relevance

The b1.2.0 `gcmark` table-resize stress case intermittently raised
`not enough memory` even though neither the host nor the cgroup was under
memory pressure.  On the clean `f8ce402f` baseline, one representative batch
failed 6 of 500 runs and another reproduced on run 23 of 50.  The failure was
an allocator protocol result incorrectly escaping as resource exhaustion.

This checkpoint is correctness-first.  It adds no waiting, retry spin, lock, or
global serialization.  A temporarily unavailable arena is skipped in favour
of another owner-local candidate or one fresh arena.  Ordinary performance
tuning remains b1.2.1 work unless a later measurement exposes catastrophic
amplification.

## Root cause

Plain arenas use `remote_active` `SEALED|PENDING` as their exact short-lived
body-writer generation.  `arena_set_alloc()` correctly refused to consume a
private bump or free-run candidate while that generation was closed, but
`lj_arena_alloc()` returned the refusal as `NULL`.  `lj_mem_realloc()` then
converted that transient ownership collision into Lua's OOM error.

Three related paths had the same semantic mismatch:

- a bin candidate could change ownership after validation and make the whole
  allocation call fail;
- a requested prefix could already be committed before preparation of its
  unused suffix lost a root/recovery race, yet the valid prefix was discarded
  as a failed allocation; and
- a plain in-place shrink could lose its arena writer generation and return
  `NULL` instead of using ordinary allocate/copy/free realloc semantics.

## Nonwaiting fallback

`arena_set_alloc()` now distinguishes the plain writer-generation veto from
other candidate-local descriptor vetoes.  The allocation loop treats both as
candidate loss rather than memory exhaustion:

1. A vetoed active bump cursor is detached.
2. Owner-local bins are searched, with at most one reclaimed-arena adoption
   and one opportunistic remote-free drain per allocation call.
3. Vetoed bin records remain represented in arena bitmap state for later
   rebuild, while the loop continues to another candidate.
4. If no immediately usable candidate remains, a fresh arena is mapped and
   allocation proceeds there.

The bounded adoption/drain policy prevents a persistent descriptor owner from
turning one allocation call into an unbounded retry loop.  After those
alternatives are exhausted, only failure to map or register a fresh arena
reaches `NULL` on this path.

If the allocation prefix has already committed, failure to canonicalize its
unused suffix no longer invalidates the prefix.  The caller receives the valid
allocation; descriptor-free segments may be rebuilt immediately, while each
owner-covered segment remains unavailable until that owner clears.

For plain realloc shrink, failure to acquire or retain the in-place writer
generation moves the requested prefix to an independent allocation and
logically frees the old body.  An admitted reader keeps the old bytes pinned
through the existing late-free protocol.  Traversable/managed shrink remains
retryable on a suffix descriptor veto: moving it and then freeing the complete
old extent would encounter the same durable veto after terminal ownership.
Normal Lua allocation already rejects resizing traversable GC bodies, but the
direct arena API must still preserve this fail-closed contract.

## Capacity rediscovery

Simply clearing a private bump cursor is safe from premature reuse, but an
all-zero tail has no `block=0,mark=1` free-run boundary.  It can otherwise be
folded into the apparent extent of the preceding live allocation until that
allocation dies, causing unbounded arena mapping amplification under repeated
gate collisions.

When a body/bin write is forbidden, the allocator now publishes only atomic
mark-plane free-boundary sentinels.  It retains the suffix start and every
currently observed block/READY/root/recovery/destructor/lifetime boundary that
could split a rebuild.  This does not touch the free span's bytes, descriptors,
or a closed plain generation.  If rebuild consumes a free prefix while an
interior owner remains active, that owner's retained mark emerges as the next
free boundary when it clears.  The existing `lj_arena_scan_free_runs()` owner
rebuild/sweep path can therefore rediscover and canonicalize the remaining
segments without relying on the already-consumed first sentinel.  New
descriptors cannot legitimately claim the detached `block=0` span after
candidate validation.

The apparent check/write gap in ordinary bump-tail bin publication remains
safe under the documented allocator invariant: the TG allocator owner is the
sole `block[]` and bin structural writer; remote paths publish only side state,
and candidate reuse rechecks `SEALED|PENDING`.  The closed-generation branch
does not write a bin node at all.

## Regression coverage

`tests/t-arena-hugetab.c` now deterministically covers:

- allocation while a plain late publisher holds the writer generation,
  requiring an immediate allocation from a different arena;
- bitmap discovery of the gated bump tail after that generation opens;
- a committed managed prefix whose suffix preparation is vetoed, rebuilding
  and consuming the free prefix while the interior descriptor remains active,
  then exact recovery of the remainder after that descriptor clears;
- plain shrink fallback under both an admitted reader and an unrelated writer;
  and
- managed shrink suffix veto preserving the original bytes, extent, and LIVE
  lifetime instead of moving into an abort.

Validation performed for this checkpoint:

- strict standalone arena fixtures and ASan/UBSan hugetab fixture: pass;
- `m2_arena_hugetab`, `m2_arena_alloc`, `m2_arena_sweep`, and
  `m2_arena_publication`: pass;
- the previously flaky `gcmark` case on the final audited code: 500/500 passes
  with JIT and 500/500 with `-joff`, zero OOMs and zero timeouts;
- the complete 15-case table-resize/GC stress matrix: pass with JIT enabled and
  with `-joff`;
- `m5_state_owner`, `m4_thr_substrate`, `m4_threading_lifecycle`, and threading
  coroutine handoff in both JIT modes: pass; and
- clean production and GC2-helper `-Werror` builds: pass.

Huge allocation-table realloc contention has a separate retryability question:
`hugetab_claim_realloc()` can still return a transient collision as `NULL`.
That path was not implicated in this small-allocation `gcmark` failure and is a
follow-up correctness item rather than being silently described as solved here.
