# GC2 stack admission is retryable, not Boolean

## Problem

The plan files are unchanged. This note records a GC2 implementation correction
for conservative x86-64 stack snapshots.

Remote, native-parked, JIT-owned, and malformed-frame fallback scans widen the
root range to `maxstack`. Those extra slots legitimately contain popped or spill
words, so a collectable-looking TValue must first prove an exact live arena
incarnation and a matching GC type. The old `gc2_tv_gcref_type_match()` returned
only true/false even though its lifetime validator has three outcomes:

- a live, exactly admitted object;
- a terminal stale/type-mismatched word; or
- a transient inability to acquire the arena/huge-reader admission.

The Boolean conversion treated the last case like stale storage. A scan could
skip a real semantic stack root and then publish its cycle/dirty completion
stamp. The next root-close test was therefore allowed to regard an incomplete
snapshot as authoritative.

There was a second validation/marking gap. The old validator released its body
scope before semantic marking. A destructive owner could then close or reuse
the incarnation between the two operations. Conversely, merely retaining the
outer observational scope is insufficient by itself: the existing semantic
marker performs a nested admission, and that second admission can still return
`GC2_MARK_DEAD` transiently.

The same completion rule applies to authoritative roots stored outside the
stack range. Thread environments, thread metatable userdata, open-upvalue
objects, and open-upvalue payloads previously used void marking helpers. A
transient semantic DEAD on any one of those edges could therefore be discarded
before the thread snapshot published its completion stamps.

## Retained protocol

Conservative stack admission now has private `RETRY`, `IGNORE`, and `ADMITTED`
results.

- Non-GC values and exact live matching objects are `ADMITTED`.
- Definitively stale, invalid, or tag/header-mismatched words are `IGNORE`.
- rescue-reader saturation, registry/SMR retry, or huge-reader overflow is
  `RETRY`.

An admitted object keeps its exact `GC2MarkScope` through the semantic mark.
If the nested MARK/WEAK semantic admission nevertheless reports DEAD, the
successful outer observation proves that this is a transient conflict rather
than permission to forget the edge, so the whole thread snapshot is retryable.
SWEEP is different: its public root marker converts a failed admission into an
allocation-free recovery identity or the sticky no-reclaim veto, so that is a
durable representation of the edge and the slot is complete.

Authoritative (non-widened) stack slots do not need the stale-word classifier,
but they now use the same status-returning semantic marker. Their DEAD result is
also retryable; an authoritative semantic slot has no `IGNORE` interpretation.
The scan continues across later stack and non-stack roots after the first retry
so transient contention cannot prevent safe partial progress.

Every non-stack thread root now uses a status-returning marker too. Environment,
metatable, open-upvalue body, and open-upvalue payload failures set the same
snapshot retry result. Successfully observed roots retain their prior marking
and rescan behavior; only transient DEAD changes from an implicit drop to an
explicit retry. SWEEP continues to use its durable public-root/recovery path.

An owner-side retry sets counted `NEEDSCAN` and returns without changing the
`scan_epoch` or `scan_dirty_epoch` completion stamps. It publishes a current
`scan_handoff_epoch` when no counted handoff exists, or preserves the existing
current-cycle handoff without double-counting it. A worker-side retry likewise
publishes no completion stamp; it first drops any GCSCAN claim, then republishes
the concrete thread on the worker queue while the registry SMR lease still pins
its identity. The lease is released only after the retry is durable.
SWEEP worker traversal deliberately retains public-root marking for stack
TValues. The private worker-edge table/NEEDSCAN filters are valid for graph
descendants, not semantic stack roots.

## Frame fallback audit

No separate frame-header retry state is required on the current x86-64 target.
Every precise-frame validation failure returns `maxstack` with conservative
mode enabled. Remote/native/JIT prewalk failures also feed an unconditional
maxstack/conservative branch. With `LJ_FR2`, every real frame function is the
ordinary tagged TValue at `frame - 1` inside that raw range; the excluded dummy
pair below `stack + 2` is not a semantic function frame. Closure discovery then
either schedules a NEW closure graph or forces the ALREADY root rescan. The new
TValue semantic-status path therefore covers transient function/prototype frame
admission without publishing a completion stamp over a missing frame root.

## Deterministic coverage

The two test-only hooks are compiled only with `LJ_GC2_TEST_HELPERS`. One
injects an outer conservative-classifier retry and the other injects a nested
semantic admission retry for one exact GC object. Both record the exact hit and
add no production state or ABI.

The focused traversal fixture covers:

- an attached foreign TG whose native current state first receives counted
  `NEEDSCAN` worker handoff;
- a direct owner-root retry which publishes a fresh counted NEEDSCAN handoff;
- a queued owner retry which preserves that handoff and both completion stamps;
- the final owner scan marking the root, stamping the current cycle, and
  clearing exactly one pending handoff;
- owner and worker retries on a unique authoritative thread environment;
- a nested semantic retry after a widened stack object was already admitted;
- an ownerless/GCSCAN-claimable worker state whose failed scan drops its claim,
  republishes the thread while its registry SMR lease still pins the identity,
  then releases the lease and completes on a later bounded claim; and
- a widened stale slot forged with `LJ_TFUNC` but pointing at a valid table
  allocation, which is terminally ignored without marking or livelock.

The worker fixture intentionally observes the intermediate requeued state
before draining unrelated SSB traffic. Conversion publishes each SSB item above
older direct grey work in the owner-pop order, so the completion phase permits
up to 64 bounded quanta of 64 items. This is a fixture fairness allowance, not a
collector wait or a source liveness workaround.

The M3 scaffold and M8 weak matrices build the library with the same
`LJ_GC2_TEST_HELPERS` definition used by their C fixtures, preventing a test
header/library configuration mismatch.

The fixture does not synthesize a standalone thread-stack SWEEP transition.
SWEEP admissions in these paths are represented by the existing allocation-free
root recovery protocol and never return the MARK/WEAK semantic DEAD injected by
the new hook; dedicated stack-root SWEEP coverage remains useful test debt.
Other useful focused extensions are a worker-side outer-classifier retry, a
precise/non-widened stack semantic retry, direct metatable/open-upvalue body and
payload injections, and a paused ordering test that observes claim drop, retry
publication under SMR, and lease release as three distinct events. The shared
status/requeue paths are covered here, but those exact branch combinations are
not claimed by this checkpoint.

## Adjacent audit finding

The only remaining caller of the old Boolean conservative validator is weak
slot clearing. That path can similarly turn transient observation or
`lj_gc2_ismarked() == -1` into irreversible CLEAR, and its overflow bridge can
currently hide a failed table pass. This is separate weak-cursor/slot-CAS work
and must be fixed in the next correctness checkpoint. Until then, it is an open
release-safety item rather than evidence that the weak protocol is complete.

## Compatibility boundary

This change does not broaden the temporary custom `lua_Alloc` support. Public
allocator API/ABI remains present, but callbacks are intentionally ignored and
GC2 uses the internal arena allocator as documented in
`lua-alloc-temporarily-disabled-2026-07-10.md`. The no-arena compatibility path
therefore retains its existing no-op observation scope. Restoring arbitrary
custom allocator ownership remains mandatory follow-up work, not part of this
stack fix.

## Validation

The final source passed strict production and `LJ_GC2_TEST_HELPERS` object
builds, a complete helper-enabled LuaJIT build, and 20 independent optimized
focused fixture processes. `git diff --check` and both modified Lua suite
manifests' bytecode parses passed. The aggregate M3 run passed the GC2 scaffold,
normal and paranoia recovery, and worker-scheduler checkpoints before stopping
at the older active-collect fixture's worker-attribution assertion. That graph
was completely drained and the cycle reached IDLE; the same assertion fails on
the unmodified `31828d7f` parent because cooperative MARK completion can perform
the drain under the close owner rather than the worker counter. It is unrelated
to this stack change and is being corrected as a separate fixture checkpoint.

`m6_jit_gc2_readiness` and `m6_jit_vmevent_flush` passed. `m6_jit_token` passed
after an immediate retry of one transient secondary-state fixture failure. The
sole compiler warning was the pre-existing GCC inlining false positive around
`gc2_root_rescan_later`/`gc2_table_scan_current` and `la_load32_acq`.
