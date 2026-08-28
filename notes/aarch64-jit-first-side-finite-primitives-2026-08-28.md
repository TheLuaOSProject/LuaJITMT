# AArch64 first-side finite publication primitives (2026-08-28)

## Status

The finite atomic operations required by the sealed first-child suffix are
implemented and pass the native arm64 and arm64e exit contract. They are not
yet wired into side publication; the production side-recorder gate remains
closed.

## Operations

- `la_xchg8_acqrel()` supplies one finite byte exchange.
- `snap_topslot_cas_acqrel()` performs the exact captured topslot transition.
- `snap_count_xchg_acqrel()` atomically dominates concurrent pre-token hot-exit
  increments with `SNAPCOUNT_DONE` and returns the prior count for a fail-stop
  invariant check. It contains no retry loop.
- `trace_nextside_cas_acqrel()` performs the exact root `0 -> child` link.
- `trace_exittarget_arm64_raw_cas_acqrel()` compares and publishes the stored
  exit-slot representation without stripping PAUTH bits.

The raw exit CAS is the required runnable-edge operation. Comparing only the
stripped target address on arm64e would accept a stale or wrong-discriminator
signature which happens to strip to the same code address.

## Proof

`tests/t-arm64-jit-exit.c` exercises success and stale-expected failure for the
topslot and nextside CAS operations, one-shot and already-DONE snapshot-count
exchange, exact raw fallback-to-child exit publication, and rejection of a
same-address fallback signed with a different ARM64e discriminator. It also
performs the exact inverse child-to-fallback CAS needed by retirement. The
existing release/acquire exit-slot race remains in the same fixture.

`tools/ci/arm64_jit_exit_contract.sh` now source-checks these helpers and ran
the complete ordinary arm64 and arm64e+BTI exit, exit-table, PAUTH-negative,
lease-race, and retirement fixtures successfully.
