## Table retired record payload/state helpers

Slice: table node/array retired-record payload discipline.

- Added `lj_tab_node_retired_{node,hmask,epoch,armed}_*()` and
  `lj_tab_array_retired_{array,acap,epoch,armed}_*()` helpers in `src/lj_tab.h`.
- Routed retired-record initialization, arming, epoch reclaim, close-time free,
  legacy GC raw-root marking, GC2 memory marking, and GC2 paranoia checks
  through those helpers.
- Updated `t-tab-retire` and `t-tab-array-publish` to inspect retired-record
  payload and state through the helpers.
- Extended `tools/ci/m5_tab_retire.sh` and `tools/ci/m5_tab_array_publish.sh`
  to reject direct retired-record payload/state access in table retirement
  files and focused tests.

Verification:

- `tools/ci/m5_tab_retire.sh`
- `tools/ci/m5_tab_array_publish.sh`
- `tools/ci/m5_concurrent_objects.sh && tools/ci/m3_gc2_scaffold.sh && tools/ci/m0_source_guard.sh && git diff --check`
