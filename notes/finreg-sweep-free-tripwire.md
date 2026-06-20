# FINREG sweep/free tripwire

Date: 2026-06-20

## Context

Ordered FINREG discovery now handles normal P_WEAK cdata finalizer discovery,
including rootless cdata and registration order. Close-time discovery also uses
the ordered registration list directly. The remaining `lj_cdata_free()` branch
that queues cdata with `LJ_GC_CDATA_FIN` is a defensive missed-publication
rescue.

## Change

- Added an assertion tripwire before the `lj_cdata_free()` rescue path so
  assertion/paranoia test builds fail if a finalizable cdata reaches sweep/free
  without prior ordered publication.
- Kept the release-build rescue behavior intact for this slice.
- Added an end-to-end `t-gc2-traverse` invariant that the FINREG telemetry block
  leaves `finreg_cdata_sweep_queued` unchanged across normal clear,
  ordered P_WEAK, rootless, re-registration, preclaim-overflow fallback,
  close-time, and bulk-finalizer cases.

## Verification

Passed:

- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m9_gc_stats`
- `tools/ci/lua_test.sh m9_m10_gc`
