## Table retired record payload/state helpers

Slice: table node/array retired-record payload discipline.

- Added `lj_tab_node_retired_{node,hmask,epoch,armed}_*()` and
  `lj_tab_array_retired_{array,acap,epoch,armed}_*()` helpers in `src/lj_tab.h`.
- Routed retired-record initialization, arming, epoch reclaim, close-time free,
  legacy GC raw-root marking, GC2 memory marking, and GC2 paranoia checks
  through those helpers.
- Updated `t-tab-retire` and `t-tab-array-publish` to inspect retired-record
  payload and state through the helpers.
- Documented the rule that retired-record payload/state access must use the
  helper layer because records are published to reclamation and GC walkers.
  Retirement and array-publication fixtures cover the behavior; CI must not
  enforce helper spelling by source search.

Verification:

- `tools/ci/lua_test.sh m5_tab_retire`
- `tools/ci/lua_test.sh m5_tab_array_publish`
- `tools/ci/lua_test.sh m5_concurrent_objects`
- `tools/ci/lua_test.sh m3_gc2_scaffold`
- `git diff --check`
