# GC2 closure churn: composed optimization and exact-base construction

## Scope and baseline

This note records the closure/upvalue GC2 optimization composed onto
`cd854a9a`. The focused command is:

```sh
BENCH_SCALE=0.10 ./src/luajit aux/bench/bench.lua closures_upval
```

The harness reports the best of five runs and divides by the scaled iteration
count. On the local Linux/x86-64 host, the clean current checkpoint measured
2.76--2.81 microseconds per operation. The composed release build measured
401--404 nanoseconds per operation. The system LuaJIT measured about 39
nanoseconds per operation. Thus this change is roughly a 6.9x improvement over
the current fork, but the remaining roughly 10x gap to stock is not a completed
performance target.

A one-million-iteration probe at the default pause completed 290 cycles,
flushed 2,004,488 pending ownership entries in 290 root batches, completed 2,029
arena sweeps, and ended with no traversable arena waiting for sweep. The steady
trigger was about 332 KiB.

Individually filtered `BENCH_SCALE=0.10` checks found no collateral regression
against clean `cd854a9a`: arithmetic was 0.38 vs 0.38 ns/op, table hash writes
37 vs 52 ns/op, new-key insertion 610 vs 802 ns/op, sunk table allocation 0.43
vs 0.46 ns/op, FFI struct access 0.39 vs 0.39 ns/op, and coroutine switching
28.9 vs 29.2 ns/op. Stock results for the same filtered runs were respectively
0.72, 21, 83, 0.38, 0.38, and 25.8 ns/op. New-key insertion and closure churn
therefore remain conspicuous fork-wide gaps even though this delta improves
both relative to the current checkpoint.

## Changes

- Pending-root pacing keeps the live-size/setpause calculation but caps the
  small-heap trigger at 384 KiB rather than forcing a major cycle at every 32
  KiB accounting flush.
- `GCproto` uses an existing alignment hole for a completed-scan cycle stamp.
  Immutable KGC/chunkname payload is scanned once per cycle even when thousands
  of fresh closures share the prototype. Cycle zero disables deduplication and
  the wrap cycle is forced major, preventing stale stamp aliasing.
- Recycled arena extents validate all lifetime/root/recovery lanes before
  clearing block/mark boundaries a word at a time. Fresh and pre-cleared bump
  windows no longer repeat interior bitmap RMWs for every closure and upvalue.
- One-upvalue closure allocation reserves the adjacent constructor lanes with
  paired packed-state CAS operations, publishes the pair with one pending-stack
  CAS, and commits the two lifetime/root lanes together when they share packed
  words. Every cross-word or contended case retains the ordinary per-object
  fallback.
- The exclusive pre-MT, zero-worker, no-active-JIT sweep path recognizes only a
  structurally proven one-upvalue Lua closure whose exact closed upvalue is the
  immediately following ownership-spine node in the same arena. It
  transitions paired MEMBER/LIVE/WHITE lanes to NONE/FREE/FREEING, splices the
  two-node ownership segment once, and then runs both ordinary destructors.
  Any mark, activation, worker, JIT, type, ownership-order, size, or edge mismatch
  falls back to the general sweep protocol.
- Immediate pre-MT leaf/pair destruction owns `mt_gc_exclusive` across the
  complete bounded root-prune pass. The successful CAS is followed by acquire
  rechecks of `mt_active`, `mt_live`, `mt_entering`, worker, JIT, and main-TG
  identity. An entrant which increments `mt_entering` before the CAS is caught
  by that recheck; one which increments afterward observes the gate and cannot
  enter the VM until release. Immediate helpers are additionally conditioned on
  this call's local successful-CAS token, so a gate held by another subsystem
  cannot authorize destruction. SMR-admission failure and normal completion
  both release the gate and wake every entrant.
- Pending/root validation holds one bounded SMR scope across each chain. A
  validated same-arena allocation is reused as a lookup hint, queued snapshots
  retain exact allocation/type/mark metadata, and Brent cycle detection makes
  the normal acyclic walk single-pass. Cycle repair remains a cold revalidated
  path.
- Fresh constructors now retain an explicit allocator base end to end. Every
  fixed-layout `lj_gc_linkobj_new`, fixed chain, and after-main publication uses
  `base == object`; over-aligned variable cdata uses its true allocation base.
  Small READY/root validation is arena-local and huge validation uses only the
  exact owner HugeTab, so an unrelated exclusive SMR gate cannot make a fresh
  constructor abort. Existing cdata root/sweep base recovery uses the immutable
  variable-header offset rather than mutable CType shape.
- A would-flush specialized bump path performs accounting/assist before it
  reserves CONSTRUCT lanes. Successful post-READY publication is therefore
  safepoint-free and does not fall back to the generic allocator solely because
  it crossed the 32 KiB local accounting boundary.
- Late logical-free provenance observed by the quarantined owner now commits
  the exact FREE/FREEING terminal pair instead of retaining a physically dead
  body for another generation. Automatic sweep checkpoints aggregate their
  arena completions and stop at one 64-arena batch, preserving the documented
  latency bound after removing that old blocker.

## Rejected experiments

Raising the pending-root cap to 512 KiB or more crossed a nonlinear quarantine
threshold on this small-live heap and regressed closure churn to roughly 1.7
microseconds per operation. The 384 KiB cap was restored. No validation, mark,
membership, activation, or grace predicate was removed to obtain the reported
result.

## Coverage and remaining work

Focused coverage includes the deterministic foreign-reclaimer-gate constructor
regression, root-pending races, GC2 recovery including assertion/paranoia mode,
the shared FFI cdata hammer with JIT off and on, closure/upvalue construction,
JIT FNEW bump/accounting, JIT pacing/recursive retention, and the complete
arena GC sweep fixture.

Independent gate review used a bounded runtime probe which completed two
10,000-closure allocation/explicit-collection rounds before starting the first
secondary VM entrant; spawn and timed join completed successfully after the
exclusive opportunities. Recovery passed in default and assertion/paranoia
builds, and the pending-root race fixture passed. A deterministic pause hook at
the gate CAS was not added to production sources: the existing add-before/after
CAS ordering is the same two-publication proof already used by
`threading_gc_enter_counted`, while a useful hook would require exporting the
otherwise-private threading entry transaction. The full threading API fixture
has a pre-existing `lua_gc`/child-futex hang in both the frozen ungated candidate
and this gated build and remains a separate b1.2 blocker.

Disabling both immediate paths in an otherwise identical temporary build raised
closure churn from a 384.02 ns/op median to 484.36 ns/op (about 26%). Keeping
the leaf path while disabling only the pair path measured about 402 ns/op. With
the entry gate added, five contemporaneous runs measured 403.64--404.31 ns/op,
the same range as the frozen candidate's 402.87--404.17 ns/op control runs.

The remaining cost is dominated by maintaining and sweeping exact ownership
entries for two new objects per iteration. Reaching stock-like closure churn
will require a broader publication/liveness representation improvement, not a
larger trigger or weakened validation. The current result is a release-enabling
throughput correction, not evidence that the final performance goal is met.
