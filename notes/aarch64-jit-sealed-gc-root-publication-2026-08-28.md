# ARM64 sealed trace GC-root publication (2026-08-28)

## Scope

This checkpoint removes the last unbounded retry loop from compact trace-body
GC-root publication. It does not yet publish an ARM64 side child or open normal
side-recorder ingress. `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`.

The ordinary `lj_gc_linkobj_new()` path appends a fresh object to its TG-local
pending-root stack with a CAS retry loop because a collector may concurrently
exchange that stack. That lock-free loop is correct for ordinary constructors,
but it cannot run after the first-side transaction's irreversible
`ASM -> PUBLISH` seal: every operation in that suffix needs a finite source-level
bound and must complete without returning to Lua error handling.

## One-shot pending-root arbitration

`TGState.gcroot_pending_owner` is a new tail-only word, so no existing VM, JIT,
event or attachment offset moves. It has three states:

- `IDLE`;
- `FLUSH`; and
- `SEALED_TRACE`.

A pending-root flusher and sealed publisher each attempt exactly one
`IDLE -> owner` CAS. A losing flusher skips that TG and republishes the
conservative pending-root hint; a losing trace publisher remains before the
irreversible seal and aborts its private trace normally. No owner waits or
spins for the other.

The flusher retains the owner only across its two TG-local head exchanges. It
releases before validating and splicing the detached chains into the global
root spine. The sealed publisher therefore excludes precisely the operation
which could invalidate its captured pending head, without serializing unrelated
root traversal or global-spine work.

Terminal TG reclamation now also requires this owner word to be `IDLE`.

## Split fresh-object publication

The new small-arena-only `LJGCNewRootPublishPlan` API has three operations:

1. `lj_gc_linkobj_new_sealed_prepare()` wins `SEALED_TRACE`, publishes the
   already initialized object's READY header, validates its exact
   constructor-owned arena/root lane and captures the stable TG pending head.
   It rejects a huge allocation: HugeTab constructor completion has a valid
   CAS retry loop which cannot enter this sealed suffix.
2. `lj_gc_linkobj_new_sealed_publish()` revalidates that private plan, writes
   the captured `gcw` link, performs one exact pending-head CAS, commits the
   constructor root membership and releases the owner.
3. `lj_gc_linkobj_new_sealed_abort()` releases an unconsumed prepare. READY is
   safe here: the original constructor still owns `CONSTRUCT|LINKING`, so normal
   unpublished abandonment or ordinary fresh-object publication remains valid.

The publish half has no source loop, allocation, free, SMR entry, wait, error,
callback or GC step. Once its pending-head CAS can make the body visible, any
unexpected constructor/owner transition is an internal fail-stop invariant;
there is no partial semantic rollback.

The future side-trace prepare phase must completely initialize the compact body
before calling this GC prepare operation. It will then place
`lj_gc_linkobj_new_sealed_publish()` after the state seal and mcode-top commit,
but before the exact trace-slot and topology CAS sequence.

## Deterministic proof

`tests/t-arm64-jit-sealed-root-publish.c` uses real arena constructors and
proves:

- prepare retains exact `CONSTRUCT|LINKING` ownership while publishing READY;
- a real peer thread calling `lj_gc_flush_root_pending()` cannot exchange a
  seeded non-NULL ordinary head or after-main head while `SEALED_TRACE` is
  held; clearing the global hint before that call and requiring its
  republication proves the peer reached and lost the owner arbitration rather
  than returning at an earlier fast path;
- publish installs the exact object/head edge, reaches `LIVE|MEMBER`, releases
  the owner and remains flushable through the ordinary root spine;
- an already-held `FLUSH` owner and a huge constructor each make prepare fail
  without a partial plan; and
- abort releases the owner while preserving the original constructor, which
  can then be published exactly once by the ordinary path or freed through the
  real unpublished-constructor cleanup path.

`tools/ci/arm64_jit_sealed_root_publish_contract.sh` checks the one-shot source
ordering and forbidden-work set, builds and runs the fixture twice as ordinary
arm64 and twice as arm64e with BTI/PAUTH, and restores the ordinary thin arm64
build. The pre-existing pending-root race and non-race fixtures also pass when
built manually with the required experimental ARM64 flags.

Validated on native Apple Silicon macOS:

```text
t-arm64-jit-sealed-root-publish OK
arm64_jit_sealed_root_publish_contract OK: bounded root publication and peer-flush exclusion ran on ARM64/ARM64e; ordinary ARM64 will be restored
t-gc-root-pending-race OK: construction/lifetime CAS, retries, stable FINREG ownership, and safe unlink verified
arm64_jit_fail_closed_gate OK: dynamic-step FORL stayed interpreted; constrained LOOP/FORL and literal-true JFUNCF entry contracts sound
```

The generic `tools/ci/lua_test.sh m3_gc_root_pending_race` launcher currently
omits this branch's mandatory ARM64 bootstrap/JIT flags and fails in
`lj_arch.h` before compiling its fixture; the same fixture was therefore built
and run directly with the correct flags. The unrelated existing
`lj_ccall.c` unused-helper warning remains.

## Remaining gate

The production first-child path must now consume this plan together with the
prepared mcode, GC catch-up and GDB plans, then prove compact-body ownership
transfer and every exact topology/raw-exit CAS. Retirement still needs its
authenticated inverse transaction. Normal side recording remains closed until
both directions and end-to-end native execution are proven.
