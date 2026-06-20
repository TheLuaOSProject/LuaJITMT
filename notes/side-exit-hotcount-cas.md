2026-06-20

Slice: side-exit hot counter update discipline.

Changes:
- Added snap_count_cas_acqrel() around la_cas8() for SnapShot.count.
- trace_hotside() now increments hot-exit counts with CAS instead of
  acquire-load plus release-store, avoiding lost concurrent increments.
- Threshold behavior stays aligned with the previous bridge: below threshold,
  a successful CAS returns immediately; at/above threshold, the counter is only
  advanced after the recorder token is acquired. If another recorder publishes
  SNAPCOUNT_DONE while this thread holds the token, the token is released and
  no duplicate side-trace attempt starts.
- tests/t-jit-token.c now directly checks the SnapShot.count CAS success and
  failure paths before the existing recorder-token and secondary side-trace
  coverage.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_token.sh
- tools/ci/m6_jit_flush_hs.sh
