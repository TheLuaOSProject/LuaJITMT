2026-06-20

Slice: side-exit metadata reads/writes on published traces and snapshots.

Changes:
- Added release-store helpers for SnapShot.count, SnapShot.topslot, and
  GCtrace.nchild.
- trace_hotside() now acquire-loads the parent trace snapshot base and the
  hot-exit count, and release-stores count increments.
- trace_stop() now acquire-loads the parent snapshot base for side traces,
  release-stores SNAPCOUNT_DONE, updates parent snapshot topslot through
  acquire/release helpers, and release-stores root child-count changes.
- Scoped side-trace flush now acquire-loads and release-stores root nchild
  when unlinking a child trace.
- trace_exittarget_acq/rel now acquire-load the trace exittab pointer before
  loading or storing a slot.

Intentionally left raw:
- Snapshot count/topslot initialization on the current trace before publish.
- Current trace fields still owned by recorder/assembler state.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_token.sh
- tools/ci/m6_jit_mcode_publish.sh
  - First wrapper run timed out in tests/t-jit-mcode-fresh.lua, matching the
    previously observed transient wrapper timeout pattern.
  - Direct rerun of tests/t-jit-mcode-fresh.lua under timeout 120s passed.
  - Full tools/ci/m6_jit_mcode_publish.sh rerun passed.
- tools/ci/m6_jit_flush_hs.sh
