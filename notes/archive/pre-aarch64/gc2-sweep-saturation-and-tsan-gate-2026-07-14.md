# GC2 saturated Huge admission and TSAN gate repair (2026-07-14)

The plan files are unchanged. This checkpoint adds the last combined stress
case requested by the independent review of SWEEP-stable Huge admission and
repairs a stale boundary assertion exposed by the post-integration TSAN run.

## Saturated Huge admission during SWEEP

`tests/t-gc2-recovery.c` now drives all of the bounded fallback mechanisms to
their limit in one deterministic process-isolated fixture:

- an unmarked Huge userdata has already supplied the SWEEP writer with a stale
  retirement ticket;
- all 65,535 representable Huge body-reader tokens are held;
- the 1,024-entry production SSB is full and its only spare node is withheld;
- a real physical SWEEP reclaimer holds `LJ_GC2_SMR_SWEEP_STABLE`; and
- the allocation page is changed to `PROT_NONE` before the late semantic mark.

The late marker can still atomically publish the exact Huge-slot `MARK`, but it
cannot obtain a body reader or enqueue the object. It must return without
reading either the GC header or payload, set the sticky recovery-failure latch,
and pin the activation in `LJ_GC2_ACT_NO_RECLAIM`. The fixture verifies that no
recovery locator was fabricated, releases every reader, and only then wakes the
stale writer. Durable `MARK` remains sufficient for the stale ticket to lose;
the object never enters `FREEING`.

This is deliberately a correctness proof, not a realistic load. The bounded
reader counter and SSB are both exhausted to prove that their simultaneous
failure remains non-dropping and fail-closed.

## Thread-id sentinel boundary

The first post-integration `m4_tsan_drivers` run stopped in
`tests/t-thr-substrate.c` before TSAN reported any race. The fixture still
treated `LJ_THREAD_GCSCAN - 1` as the last process-issued owner id. Since the
addition of the distinct `LJ_THREAD_GCPREP` sentinel, the last valid owner is
instead `LJ_THREAD_GCPREP - 1`.

The runtime allocator already enforced the correct boundary. The fixture now
uses `LJ_THREAD_GCPREP` consistently for both sequential and concurrent
saturation, and explicitly proves that neither reserved sentinel can be passed
to `lj_thr_create` as an owner id.

## Verification

- `m3_gc2_recovery`: passed in the normal helper build and the
  assertion/paranoia helper build, including the `PROT_NONE` saturated case.
- `m4_tsan_drivers`: passed after the fixture correction; both the thread/TG
  substrate and bounded MPMC channel driver ran against a fully TSAN-instrumented
  LuaJIT target with `halt_on_error=1`.
- `m7_ffi_jit_cnew`: passed, including real CNEW, CNEWI, large CNEW traces and
  the spawned-thread collection path.
- `m6_jit_flush_thread_stress`: passed at 3 workers, 16 rounds and 32
  short-lived threads.
- `m6_jit_flush_thread_heavy_stress`: passed at 4 workers, 96 rounds and 192
  short-lived threads.

The wider post-integration ASAN/UBSAN and stress matrix remains a separate
b1.2.0 release-gate activity. Ordinary performance differences remain b1.2.1
work unless they are catastrophic (roughly 100x, runaway resource use, or an
effective hang).
