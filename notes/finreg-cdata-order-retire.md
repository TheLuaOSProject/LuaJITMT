# Cdata FINREG Ordered Retire

## 2026-06-20

Problem:
- Ordered cdata FINREG nodes stayed on the active ordered scan list after their
  slot was cleared, their generation was disabled, or their object had already
  been queued for finalization.
- Later P_WEAK and close-time scans revisited those nodes as tombstones instead
  of shrinking the active discovery set.

Fix:
- `FinRegOrderNode` now has atomic active/retired state plus a retired-list
  link owned by `CTState`.
- Ordered scans skip inactive nodes, logically retire definite tombstones, and
  retire nodes immediately after they queue their cdata finalizer.
- Retired nodes remain rooted/marked and are freed at FINREG teardown, so
  readers that already loaded an old physical link remain safe.
- `threading.gcstats().finreg_cdata_order_retired` exposes the retire count.
- Follow-up counter helper work routes ordered FINREG telemetry publication
  through `gc2_finreg_cdata_order_*()` and
  `gc2_finreg_cdata_pending_order_hits_*()` helpers. Ordered discovery,
  close-time discovery, pending scans, CTState retire, and GC2 init no longer
  spell direct atomics against the ordered counter fields.

Regression:
- `tests/t-gc2-traverse.c` now checks that a live ordered `ffi.gc` cdata stays
  active across collection, then leaves the active ordered list after queueing.
- The same test clears an ordered registration with `ffi.gc(cd, nil)` and
  verifies a pending ordered scan retires that node.

Verification:
- `tools/ci/m0_source_guard.sh`, `git diff --check`, focused
  `t-gc2-traverse`, `tests/t-gc-stats.lua`, `tools/ci/m8_weak.sh`, and
  `tools/ci/m7_ffi_finreg.sh` passed.
