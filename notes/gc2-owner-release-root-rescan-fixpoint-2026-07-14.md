# GC2 owner-release and executable-root fixpoint repair (2026-07-14)

## Release relevance

The optimized `m5_tab_resize_stress` `gcmark` workload intermittently stopped
making progress. A healthy run completed in a fraction of a second, while a
failing run used one CPU indefinitely with stable RSS and continuously created
GC2 work. This is a liveness/correctness defect, not b1.2.1 performance debt.

The repair is within the b1.2.0 x86-64 Linux GC/JIT correctness scope. It does
not claim that the later performance targets are complete.

## Failure composition

A worker could consume the concrete grey item for a foreign-owned `lua_State`
and replace it with a counted `LJ_GC_NEEDSCAN` owner handoff. The owner could
then release that state and detach before acknowledging the handoff. The global
pending count remained one, but no TG still owned the stack and therefore no
owner-only root acknowledgement could discharge it.

MARK's first root certificate uses `SCAN_ROOTS`, but later retries reused that
certificate with `SCAN_OWNER_ROOTS`. Ordinary coroutines are not necessarily in
the threading-module state registry, so a registry-only repair was insufficient:
the last owner must transfer the exact identity before making the state
claimable.

Repeated snapshots amplified the leak because an executable-root exception
cleared and republished already-set FUNC/PROTO `NEEDSCAN` on every root hit.
That contradicted the invariant that non-table `NEEDSCAN` is retained as a
same-cycle traversal/deduplication token to bound cyclic executable graphs.

## Owner-release handoff

Every ordinary `lj_state_release()` now publishes a short
`tid -> LJ_THREAD_GCSCAN -> 0` interval. While the release sentinel still owns
the state, `lj_gc2_thread_owner_releasing()` preserves any concrete thread-scan
handoff in the exact TLS TG's SSB, or in the MPMC recovery plane when no exact
TLS TG is available. GCSCAN scanner release and GCPREP rollback retain their
existing direct-sentinel release rules.

The x64 coroutine success and fallback fast paths previously bypassed
`lj_state_release()` with raw dirty/hint/owner stores. Both now call a small
result-preserving release helper. The success path restores the already
synchronized resumer base. The fallback path preserves both `RD` and the
current, not-yet-synchronized base across the C call in callee-saved registers.
This keeps the normal fast path's result and `fff_fallback` register contracts.

No ordinary-owner-to-zero store remains outside `lj_state_release()`.

## Exact per-state token and race closure

The old global pending count could not prove which state owned a counted bit: a
stale uncounted hint on one state could steal another state's aggregate count.
`lua_State.scan_needscan_counted` is now the exact per-state state machine:

- `NONE`: no counted handoff;
- `INSTALLING`: the token is concrete, but its aggregate/epoch publication is
  still being installed;
- `COUNTED`: the handoff epoch and one aggregate pending count are published.

The token is installed before the header hint, eliminating the hint-only
pre-token window. Clearing only decrements after winning the exact
`COUNTED -> NONE` CAS. A worker which encounters `INSTALLING` retains/requeues
the concrete state instead of clearing or decrementing it.

After publishing `COUNTED`, the setter performs a same-value CAS on
`thr_owner`. This is the RMW ordering edge with the release CAS: if it succeeds,
a later `tid -> GCSCAN` release must observe the non-`NONE` token; if it loses,
the setter publishes the exact identity directly to MPMC recovery. The release
hook accepts both `INSTALLING` and `COUNTED`, so a release inside the aggregate
publication window also retains concrete work. Duplicate publications are
safe: only one exact clear can decrement the aggregate.

Root recovery uses only an acquired owner snapshot. It republishes absent
tokens and owner-zero/GCSCAN tokens, but does not walk a reclaimable TG list to
classify stale tids and does not rescue GCPREP. Stale-owner takeover remains in
thread traversal under its SMR lease.

## Scan generation and root closure

Thread scan/handoff stamps now use a separate nonzero 64-bit
`thread_scan_cycle`. This removes the ambiguity between the permanent zero
"not scanned" sentinel and a wrapped 32-bit GC cycle. The generation advances
at every MARK start, including preserved-abort restarts.

`gc2_mark_root_snapshot()` performs a full `SCAN_ROOTS` retry whenever the
aggregate thread handoff count is nonzero; owner-only rescans remain available
when no thread handoff is pending.

The FUNC/PROTO force exception was removed. An already-set non-table token stays
deduplicated while grey or SSB work is visible. The conservative empty-frontier
repair remains: a possibly preserved uncounted token may be cleared and
republished once when no concrete frontier is visible.

This executable-token reasoning currently relies on b1.2.0's major-only
physical collector. A future real minor-sweep implementation must
generation-tag uncounted executable membership; that requirement is unchanged.

## Regression proof

`tests/t-gc2-traverse.c` now covers:

1. release publication through exact-TLS SSB, generic MPMC recovery, and the
   process-wide registry fallback;
2. deterministic release before token installation, after `COUNTED`, and while
   paused in `INSTALLING` before the aggregate increment;
3. a stale header hint on one state being unable to decrement a different
   state's exact pending count;
4. preserved-abort/cross-cycle handoff completion with the 64-bit thread scan
   generation;
5. FUNC/PROTO empty-frontier repair followed by bounded deduplication while
   other work remains visible.

Validation after the final owner-release/VM changes:

- strict `LJ_GC2_TEST_HELPERS` build: passed;
- `t-gc2-traverse`: 100/100;
- `t-state-owner`: 100/100;
- `t-thr-substrate`: 100/100;
- `t-threading-lifecycle`: 30/30;
- stock coroutine, coroutine-yield/stack-growth and traceback tests: passed
  with JIT enabled and disabled;
- `t-threading-coroutine`: passed with JIT enabled and disabled.

An independent 500-run `gcmark` batch on an intermediate binary had no hangs
but produced six immediate `not enough memory` errors despite abundant host
memory; the clean production build reproduced one in 23 runs. That separate
allocator contention/false-OOM defect is not represented as a successful run
here and must be revalidated after its own repair.
