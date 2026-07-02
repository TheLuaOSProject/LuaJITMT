# FINREG sweep/free invariant

Date: 2026-06-20

## Context

Ordered FINREG discovery handles normal P_WEAK cdata finalizer discovery,
including rootless cdata and registration order. Close-time discovery uses the
ordered registration list directly. A cdata object with `LJ_GC_CDATA_FIN`
reaching `lj_cdata_free()` means the ordered FINREG protocol missed its owner
edge.

## Change

- Replaced the `lj_cdata_free()` rescue path with a fatal invariant helper.
  Release builds now fail closed instead of re-whitening, marking finalized, and
  queueing a finalizer from the sweep/free destructor path.
- `finreg_cdata_sweep_queued` remains as a last-ditch invariant counter before
  abort; normal tests assert it is unchanged. Follow-up notification helper
  work now updates this telemetry through
  `lj_gc2_finreg_cdata_note_sweep_queued()`.
- Documented the `lj_cdata_free()` fail-closed invariant; the old static guard
  for rescue calls is obsolete under the no-source-guards policy.
- Added `tests/t-ffi-finreg-free-invariant.c`, which forks a child, sets
  `LJ_GC_CDATA_FIN` on a cdata object, calls `lj_cdata_free()`, and verifies
  the child exits by `SIGABRT`.
- Added an end-to-end `t-gc2-traverse` invariant that the FINREG telemetry block
  leaves `finreg_cdata_sweep_queued` unchanged across normal clear,
  ordered P_WEAK, rootless, re-registration, preclaim-overflow fallback,
  close-time, and bulk-finalizer cases.

## Verification

Passed:

- `tools/ci/m7_ffi_finreg.sh`
- `tools/ci/m8_weak.sh`
- `tools/ci/m9_gc_stats.sh`
- `tools/ci/m9_m10_gc.sh`
