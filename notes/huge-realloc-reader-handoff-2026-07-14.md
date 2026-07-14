# Huge realloc reader handoff and raw sweep visibility

## Release scope

This checkpoint is a b1.2.0 correctness change.  It adds no waiting, mutex,
condition variable, or retry sleep.  The ordinary uncontended same-map huge
realloc remains the existing constant-time metadata CAS.  A resize only pays
for a replacement mapping when an already-admitted counted reader makes the
old logical geometry immutable.

The work uses only the GC2/HugeTab lifetime protocol.  It does not restore or
fall back to the legacy collector.

## False OOM

`hugetab_claim_realloc()` previously rejected every nonzero HugeTab reader
count.  `arena_allocf_realloc_huge()` translated that transient rejection to
`NULL`, and `lj_mem_realloc()` translated `NULL` to Lua out-of-memory.  Exact
mark-reader admission is valid for plain huge buffers, so a short conservative
GC2 observation could make a resize fail even with ample address space.

The exclusive realloc owner may now add `BUSY` while ordinary readers already
exist.  `BUSY` pins the mapping and blocks new destructive owners.  The owner:

- keeps the in-place fast path only when the claim snapshot has zero readers;
- otherwise allocates and copies a replacement while the old reader tokens
  keep the source mapped;
- clears `BUSY` and publishes `DEFER_FREE` without changing the old size or
  reader field; and
- returns the valid replacement immediately.  The final reader folds the old
  entry into `FREEING|SWEEP_OLD`.

If replacement allocation fails, `hugetab_release_realloc()` now clears
`BUSY` even while readers remain.  With no independent free intent the old
mapping, size, bytes, and reader tokens remain unchanged, which preserves the
normal failed-realloc contract.

Traversable HugeTab allocations remain non-reallocable.  Their READY/cdata
shape, recovery identity, intrusive root state, and retirement tickets are
bound to the exact allocation address.  Production `lj_mem_realloc()` already
aborts attempts to resize them; the HugeTab claim now enforces the same rule
instead of relying solely on that outer check.

## Free versus realloc

An external free which observes realloc `BUSY` has irrevocably relinquished
the allocation and publishes `DEFER_FREE`.  Realloc no longer consumes that
intent.  Its same-map finish or move commit clears `BUSY`, discards any new
replacement, and lets the deferred free fold normally.  This makes the two
paths agree on one terminal owner and prevents a replacement from being
returned after the free linearization point.

## Raw deferred-free leak

`hugetab_fold_deferred_free()` produces `FREEING|SWEEP_OLD` for both typed and
plain huge mappings.  The old `hugetab_sweep_next()` predicate required
`TRAVERSABLE`, so a plain huge free or realloc deferred behind a reader was
invisible to normal GC2 sweep and leaked until terminal `fini_all()`.

Sweep iteration now selects:

```text
SWEEP_OLD && (TRAVERSABLE || FREEING)
```

This keeps ordinary typed sweep entries and additionally admits only terminal
plain entries.  `lj_gc_reclaim_gc2_huge()` already implements their required
sequence: replace the all-ones fresh-grace sentinel with the current epoch,
request a handshake, then delete and unmap after that grace.

Huge mappings created by `arena_allocf_new()` now publish
`GCAhdr.progress_g` from the owning TG before HugeTab insertion.  Consequently
the last reader can wake an active sweep when it makes a terminal plain entry
actionable.  Dead-TG transfer stays within the same `global_State`, so the
published process pointer remains valid across ownership transfer.

## Exact concurrent resize boundary

Two simultaneous reallocations of the exact same raw allocation are still a
bounded conflict: one owns `BUSY`, and another returns `NULL` without reading
the mapping or changing metadata.  This is memory-safe but can surface as an
OOM exception through the current `lua_Alloc`-compatible result ABI.

This boundary is intentional for b1.2.0.  A speculative follower design was
rejected during audit because allocating before lifetime admission permits
address ABA: the old mapping can be unmapped, the virtual address reused, and
a stale follower can mistake a new `BUSY` incarnation for the old one.  Taking
`DEFER_FREE` before allocation instead violates failed-realloc semantics when
replacement allocation fails.  Comparing address, size, and flags is not an
incarnation proof.

The complete later solution needs a non-repeating HugeTab incarnation plus a
preallocated forwarding/result descriptor (or an equivalent richer allocator
result ABI).  It must publish exactly one replacement, let contenders observe
that result without waiting, and ensure `lj_gc_total_adjust()` subtracts the
old size exactly once.  Until that substrate exists, safe rejection is better
than a probabilistic ABA workaround or duplicate successful replacements.

This limitation is separate from the fixed production failure: an unrelated
GC2 reader no longer makes a resize report OOM.

## Deterministic coverage

`tests/t-arena-hugetab.c` now checks:

- a held raw reader plus impossible-size allocation failure leaves the old
  size, bytes, reader count, and nonterminal flags unchanged;
- a held raw reader forces a same-extent resize to move, preserves copied
  bytes, and leaves the reader's old geometry authoritative;
- final reader release creates raw `FREEING|SWEEP_OLD`, and both
  `sweep_next()` and `has_sweep_old()` discover it;
- an allocator-created huge header contains the owning process progress
  pointer;
- traversable huge resize is rejected without mutating its identity;
- a paused exact-buffer resize rejects another resize without touching its
  bytes or metadata;
- the last pre-existing reader can release before a paused owner resumes;
  `BUSY` still pins the old source, the claim snapshot forces a move, and the
  owner then completes direct deletion after copying; and
- a paused resize is safely preempted by an external free, with one terminal
  handoff and no replacement or double unmap.

## Validation

The final source passed:

- the strict optimized `m2_arena_hugetab` fixture, including the deterministic
  owner/reader schedules above;
- 100 consecutive executions of that optimized fixture;
- the same fixture under AddressSanitizer and UndefinedBehaviorSanitizer with
  leak detection and halt-on-error enabled;
- the complete `m2_arena_all` allocator/metadata/GC-mark/GC-sweep/GC-phase
  matrix;
- the focused allocation, publication, and sweep cases;
- a ThreadSanitizer fixture build and run (with GCC's known
  `atomic_thread_fence`-under-TSan warning demoted from `-Werror`);
- a strict production, no-test-helper compilation; and
- `git diff --check`.

The source-level HugeTab state-machine audit and an independent validation
pass found no remaining use-after-free, double-unmap, or accounting blocker in
this checkpoint.  They deliberately do not broaden the claim about the exact
simultaneous-resize boundary documented above.  The raw handoff fixture proves
normal sweep visibility; the existing GC-sweep/phase fixtures exercise the
runtime reclamation machinery, while a single end-to-end forced raw
sentinel-to-grace schedule remains useful future adversarial coverage.
