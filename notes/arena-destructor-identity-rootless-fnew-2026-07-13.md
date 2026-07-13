# Arena destructor identity and rootless hot FNEW

## Status and scope

This tranche removes ownership-spine publication from the exclusive small-
arena bump paths for:

- zero-upvalue Lua closures (`LFUNC0`),
- one-cell-upvalue Lua closures (`LFUNC1`), and
- their closed upvalue cells (`CLOSED_UV`).

It applies to the sole-main-TG C bump helpers and the x86-64 traced numeric
FNEW inline path. Generic closure shapes, active-white construction, attached
secondary mutators, and configured GC2 worker pools retain the existing
generic root-publication path.

This is an intentional performance-oriented divergence from treating the
intrusive ownership spine as the only destructor identity. Semantic liveness
still comes exclusively from ordinary Lua edges and GC2 marking; the new
metadata is physical type/destructor authority, never an implicit root.

## Motivation

The earlier FNEW bump path published both the closure and its fresh upvalue to
the per-TG ownership stack. A traced pair consequently paid lifetime and root
CAS transactions, a pending-stack CAS/retry loop, two pending hints, and later
root detachment/reanchor work. This tranche removes those transactions and
their later ownership-spine work. The completed benchmark shows that doing so
did not eliminate the dominant active-GC cost: closure churn is still roughly
an order of magnitude slower than stock LuaJIT.

Arena mark bits are sufficient for liveness but, on their own, cannot safely
select a type-specific destructor from mutable or stale payload bytes. The
new sidecar supplies that missing immutable identity without manufacturing a
semantic root.

## Representation

Each small arena now contains four 4096-bit destructor-kind planes:

```text
kind(cell) = plane0[cell]
           | plane1[cell] << 1
           | plane2[cell] << 2
           | plane3[cell] << 3
```

Zero means no arena-owned destructor identity. Fifteen nonzero codes remain
available for the full GC object family. Hot classes deliberately use one-hot
codes so generated x86-64 publication is one ordinary `BTS` per object:

```text
1  LFUNC1
2  CLOSED_UV
4  LFUNC0
```

The sidecar costs 2048 bytes per 64 KiB arena and moves `LJ_AFIRST_CELL` from
488 to 616. This reduces payload capacity by 3.125% of an arena. It does not
change the public LuaJIT API or ABI; `GCArena` is internal allocator metadata.

## Publication protocol

Typed bump reservation performs the following operations while the allocation
start is undiscoverable (`block=0`):

1. verify that every destructor-kind plane is clear at each intended start;
2. claim lifetime `FREE -> CONSTRUCT`, leaving root state `NONE`;
3. install the immutable nonzero destructor kind and initialize the complete
   object body, including an inert `nextgc`, while `block` remains zero;
4. install the birth mark and READY bits;
5. release-publish the allocation boundary in `block`;
6. commit lifetime `CONSTRUCT -> LIVE`.

A stale kind is allocation authority, even when `block=0` and the lifetime
lane appears FREE. Reservation therefore fails closed without clearing the
kind or advancing publication over it. The x86-64 pair path performs read-only
`BT` preflights for both exact starts in all four destructor planes before
either lifetime lane is claimed. Every mismatch enters the common terminal
corruption trap; it cannot be mistaken for a bit belonging to the intended
one-hot class.

The C reservation installs the kind before initializing the body. Because of
DynASM's backwards emitter, the x86-64 inline path initializes the body before
its kind `BTS`; both orders are private behind `block=0`, and both complete the
body and kind before READY/block discovery. Kind writes and boundary writes are
single-writer operations; mark and packed-lifetime transactions retain the
required atomic operations. The common closure/upvalue pair claims both
same-word lifetime lanes with one locked CAS and commits them with one locked
CAS. A pair crossing a 16-cell lifetime-word boundary uses exact per-lane
claims, including rollback of the first lane if the second claim loses.

Traced inline FNEW always falls back to the C helper while marking is active,
whether allocation is white or black. Birth-marking alone is insufficient:
the C helper also publishes the proto, environment, and upvalue traversal work.
The C bump helper may still construct a rootless pair during active-black
marking because it executes those barriers before committing the typed
lifetime. Active-white construction continues through the generic rooted
allocator/publication path. Outside active marking the trace may still honor
`alloc_black` for the ordinary mark-bit choice.

Recovery may transiently own a visible constructor as `MUTATING`. A typed
commit accepts that state without waiting; recovery's existing
`CONSTRUCT/MUTATING -> LIVE` repair uses root `NONE` as the completed rootless
target. The generated pair commit takes exact per-lane recovery arms after any
same-word CAS mismatch. Exact `LIVE` and `MUTATING` validators share the
terminal corruption trap with pair claim; undefined lifetime states fail
closed.

The paired transform is larger than a signed-byte retry displacement. Its
generated retry is therefore an explicit near `JNE rel32`; the focused decoder
verifies that it targets the real start of the backward CAS loop. This avoids
silently wrapping a short branch into an unrelated cold arm.

## Mark, sweep, rescue, and destruction

Rootless typed objects are reached and marked only through normal Lua graph
edges (stack, table, closure, upvalue, registry, and so on). The paranoia
reverse-root oracle accepts a marked rootless allocation only when it has a
supported READY descriptor and a readable typed lifetime.

After the ownership-spine bridge has classified ordinary objects, arena scan
handles an unmarked supported descriptor without inspecting its body:

```text
WHITE -> RETIRED
reclaim_deferred += 1
request a full arena grace
```

A racing semantic mark sets the mark bit and rescues `RETIRED -> LIVE`,
settling deferred accounting. After grace, a rescued rootless typed body goes
directly back to `WHITE`; it is not reinserted into the ownership spine because
the immutable sidecar remains its next-cycle physical identity.

An unrescued RETIRED body is validated after grace against all of:

- supported sidecar kind,
- READY and root `NONE`,
- absence of conflicting cdata coverage,
- absence of `FIXED` and `SFIXED` retention flags,
- immutable header shape (`LFUNC0`, `LFUNC1`, or closed upvalue),
- exact block/mark extent, and
- the ordinary queued/freeable allocation validator.

Only then does the normal type-specific destructor acquire `DESTRUCT -> FREE`
and run. Unsupported, inconsistent, or transient descriptors are marked and
retained fail-closed; payload bytes never choose a destructor. In particular,
the sidecar never overrides cdata ownership or permanent-object retention, and
neither disagreement authorizes the competing destructor family.

Quarantine completion clears a dead incarnation in this order:

1. clear READY for dead starts;
2. release-remove their block discovery boundaries;
3. clear their destructor-kind bits;
4. publish the ordinary free-run mark boundaries.

The kind clear is restricted to `old_block & ~live`: quarantine may clear the
identity only for an exact old start whose discovery boundary it removes in
that apply. A nonzero kind at a block-zero cell is malformed or still
constructor-owned, but remains authoritative; quarantine preserves it as a
reuse veto and diagnostic identity. The arena remains sealed throughout.

The direct mapping teardown path is equally conservative. `lj_arena_unmap()`
requires the complete destructor sidecar to be empty in addition to empty
recovery, root, and lifetime planes. A stale kind with otherwise empty state
therefore retains the mapping rather than allowing the address to be reused.
Normal allocation/rebuild preflight likewise requires READY zero, kind zero,
lifetime FREE, root NONE, and recovery IDLE. The legacy bitmap-only sweeper
cannot execute semantic destructors and therefore pins every nonzero kind
instead of treating it as raw storage.

## Validation coverage

The focused tests cover:

- executable x86-64 order: body, kind, READY, block, lifetime commit;
- read-only all-four-plane stale-kind preflight for both FNEW starts before
  the common pair claim, with every failure targeting the terminal trap;
- absence of FNEW root transitions, pending-stack CAS, and pending hints;
- exact LFUNC1/CLOSED_UV pair layout, including cross-word pairs;
- one-CAS same-word pair claim/commit and split-word claim rollback;
- exact `LIVE`/`MUTATING` recovery crossover validators;
- LFUNC0 and standalone closed-upvalue C helpers;
- unconditional traced active-MARK fallback and direct active-black C
  construction through an actual MARK-to-SWEEP transition;
- repeated forced full GC of a reachable rootless traced closure;
- paranoia's reverse-root oracle with a reachable rootless closure;
- legacy bitmap-sweep pinning without reading a synthetic body;
- direct-unmap retention of an otherwise orphaned destructor kind;
- quarantine preservation of a block-zero kind;
- fail-closed cdata-sidecar disagreement and `FIXED|SFIXED` retention, followed
  by reclamation only after the injected disagreement is removed;
- destructor-kind removal after actual closure/upvalue reclamation; and
- the arena-capacity boundary after the larger metadata header.

The mcode audit resolves relative helper calls against the real executable
address returned by `jit.util.tracemc`, not the copied byte-string address.
Using the copy as the rel32 base was a harness false positive and could not
prove the fallback branch target.

Current forced-clean validation has passed `m6_jit_fnew_bump`,
`m2_arena_sweep`, `m2_arena_gcsweep`, `m3_gc2_paranoia` (stock JIT, no-JIT,
and default-build restore), and `m3_gc2_worker_scheduler`. The focused
`m3_gc2_recovery` and `m6_jit_gc2_readiness` runs also pass. The aggregate
scaffold first timed out in its nested normal recovery case; an isolated retry
passed normal recovery and then timed out in paranoia. Repeated standalone
execution reproduced the timeout as an intermittent test-fixture liveness
race, not a collector hang: a one-shot drain could honor the initial
cooperative MARK JIT lease and return before reaching the requested pause. The
fixture now closes the MARK gate before spawning that drain. After the fixture
fix, a forced-clean
`m3_gc2_recovery` passed the normal, paranoia, and default-restore cases; the
exact normal and paranoia binaries then each passed 25/25 standalone runs
within 60 seconds per run.

## Benchmark result

Five-run `BENCH_SCALE=.1 closures_upval` measurements on this container were:

| Mode | Current ns/op (five runs; median) | Stock `/usr/bin/luajit` ns/op (five runs; median) | Ratio |
| --- | --- | --- | --- |
| GC active | 432.76, 432.57, 435.70, 433.63, 432.87; **433.63** | 38.58, 37.44, 37.66, 38.03, 37.96; **37.96** | **11.42x** |
| GC stopped | 97.98, 95.04, 96.19, 99.12, 94.93; **96.19** | 39.95, 38.44, 39.18, 40.16, 40.20; **39.95** | **2.41x** |

The active-GC overhead therefore remains a b1.2.0 release blocker. The sidecar
tranche establishes rootless destructor correctness, but does not by itself
meet the acceptable-performance gate.

## Follow-up

The four-plane encoding is deliberately general. Future work can assign the
remaining kinds to other fixed-layout GC objects and progressively remove
ownership-spine traffic without adding type-local marker schemes. Variable
layout and finalizable objects require their own immutable extent/finalizer
metadata and no-throw publication protocols before they may use this path.

The next closure-churn direction is safe pre-grace semantic destruction when
the current call locally wins the full main-TG/worker/JIT/SMR exclusion proof.
It must treat `LFUNC0`, `LFUNC1`, and `CLOSED_UV` as independent typed starts;
it must not restore an explicit "closure followed by matching upvalue" shape
matcher. Even then, physical block removal, READY/kind clearing, and address
reuse remain post-grace because the mutator gate alone does not drain every
registry/SMR reader.

This tranche does not change the documented temporary custom `lua_Alloc`
omission. Public allocator signatures remain present, but arbitrary callbacks
are currently accepted and ignored: state allocation stays on the internal
arena, `lua_setallocf()` is a no-op, and `lua_getallocf()` reports the active
internal allocator. Rootless typed FNEW is consequently admitted only while
`allocf_arena` identifies that internal ownership domain. This work does not
broaden the b1.2.0 release scope beyond GC2/JIT basic operation and acceptable
performance.
