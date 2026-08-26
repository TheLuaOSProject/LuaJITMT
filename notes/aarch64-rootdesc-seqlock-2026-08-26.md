# Apple ARM64 root-descriptor publication and gate repair

The architecture-aware standalone harness exposed a real ARM weak-memory
failure in `LJGC2RootDesc`: two native runs out of 51 observed an ACTIVE
descriptor whose `old_root` and `aux_root` came from different owner
publications.  The former protocol wrote a reused payload while control was
IDLE and relied on x86 TSO plus a control/payload/control sample.  An ARM reader
could retain the previous ACTIVE control observation while seeing stores from
the next payload.

## Help-free payload publication

The repaired state machine keeps the original three externally visible states:

```text
IDLE(n) -> ACTIVE(n+1) -> IDLE(n+1)
   |            |
   +------------+----------> NO_RECLAIM
```

Before touching the reused payload, the owner executes a successful
acquire-release compare-and-swap from exact `IDLE(n)` to the same `IDLE(n)`
value.  A successful same-value RMW is still a control modification-order
event.  A release fence follows it, then the owner performs relaxed payload
stores and finally release-publishes `ACTIVE(n+1)`.

An ACTIVE reader performs relaxed payload loads, an acquire fence, and an exact
second control load.  If any payload load observes a store from a later owner
publication, the fence pair makes the same-value IDLE RMW happen-before the
reader's final control load.  Atomic write-read coherence therefore forbids
that load from accepting the older ACTIVE word.  If the reader saw only the old
payload, accepting that old ACTIVE word remains valid.

The same-value claim deliberately does not reserve the descriptor.  A closer
can still replace IDLE with sticky NO_RECLAIM while an owner is paused; the
owner's delayed activation then loses and also returns PINNED.  This preserves
close progress without an unhelpable PUBLISHING state.  The protocol still
relies on the existing owner-only publication invariant.

## Descriptor/gate StoreLoad boundary

Root admission has a separate two-object Dekker boundary:

```text
publisher: descriptor = ACTIVE; fence; load root gate
closer:    root gate = CLOSING; fence; load descriptor
```

An acquire-release RMW alone does not provide StoreLoad order on ARM, so both
participants could otherwise miss the other's publication.  Weak targets now
execute paired sequentially consistent fences after successful descriptor
activation and, on every scanning thread, after exact acquire/validation of
CLOSING but before its first descriptor load.  The thread which created
CLOSING cannot lend a fence sequenced after its CAS to another helper merely
through that earlier release operation.

`lj_gc2_rootdesc_snapshot_closing()` centralizes the scanner boundary.  An
ACTIVE view is bound to both the exact activation object and its non-wrapping
close generation; raw views or views from another universe fail closed when a
helper tries to publish coverage.  The descriptor also retains its immutable
owning activation pointer in the existing alignment hole before `coverage`, so
a persistent numeric certificate cannot be replayed against a different
universe with the same generation.  This keeps `LJGC2RootDesc` at 96 bytes.
Passing a bound view to a different tracing thread still requires a real
release/acquire handoff.  x86-64 keeps its existing locked-RMW/TSO ordering
without a redundant `mfence`.

## Validation

Validation on Apple clang 21, targeting macOS 11:

- the native markword/activation/root-descriptor fixture passed 100 consecutive
  optimized runs; the old protocol failed twice in 51 observed runs;
- the optimized ARM64 artifact contains `dmb ish` StoreLoad fences and the
  `dmb ishld` reader fence, and imports no atomic runtime helper;
- ASAN+UBSAN and native ThreadSanitizer passed the complete fixture;
- the exhaustive root-gate/store good model has zero failures and all five
  intentionally broken variants are detected;
- the native helper-thread gate-ordering litmus completed 2,000,000 persistent
  rounds without the forbidden publisher-OPEN/closer-IDLE result;
- the x86-64 cross-built fixture runs under Rosetta, retains inline
  `cmpxchg16b`, and imports no atomic runtime helper.

The exhaustive gate/store model treats publication as one linearization step,
so it proves the abstract gate cutover but not payload-field interleavings or
ARM hardware ordering.  The native repeated snapshot stress covers the payload
interleaving, and the separate ARM gate-ordering litmus covers the cross-object
boundary.

This repairs the descriptor and gate substrate only.  It is not evidence that
the ARM assembler VM already has all acquire/release loads, safepoints, or
TG-local state required for lockless execution.
