# Userdata FINREG Active Unlink

## 2026-06-20

Problem:
- Userdata FINREG registration used a GC2 side list for discovery, but clear,
  stale, and already-queued entries only nulled the payload.
- The active list kept tombstone nodes forever, so later discovery and forget
  scans repeatedly traversed entries that could no longer produce work.

Fix:
- `lj_gc2_finreg_udata_unlink()` now first retires stale nodes out of the
  logical active discovery set, then best-effort CAS-splices the physical list.
- Retired nodes are pushed onto a separate retired list and freed only during
  GC2 teardown, preserving safety for any reader that already loaded the old
  active-link chain.
- Explicit userdata finalizer clear and GC discovery both retire dead nodes.
- `collectgarbage("stats").finreg_udata_retired_nodes` exposes the monotonic
  retire count.

Regression:
- `tests/t-gc2-traverse.c` counts active userdata FINREG nodes around explicit
  metatable clear, re-registration, and finalizer discovery. The active list
  must return to its baseline after clear and after queueing.

Verification:
- `tools/ci/m0_source_guard.sh`, `git diff --check`, focused
  `t-gc2-traverse`, `tests/t-gc-stats.lua`, `tools/ci/m8_weak.sh`, and
  `tools/ci/m7_ffi_finreg.sh` passed.
