## Table retired record next-link helpers

Slice: table node/array retired-list link discipline.

Changes:
- Added `lj_tab_node_retired_next_acq/rel()` and
  `lj_tab_array_retired_next_acq/rel()` in `src/lj_tab.h`.
- Routed table node and array retired-list push, reserve initialization, epoch
  reclaim, final free, legacy GC marking, GC2 marking, and GC2 paranoia
  raw-root traversal through the helpers.
- Updated `t-tab-retire` and `t-tab-array-publish` to acquire-load retired-list
  heads and traverse record links through the helpers.
- Documented the rule that retired table node/array links are SMR publication
  links and must be accessed through the acquire/release helpers. Retirement,
  marking, paranoia, and focused fixtures cover the observable behavior; CI must
  not enforce the helper spelling by repository text assertion.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m5_tab_retire`
- `tools/ci/lua_test.sh m5_tab_array_publish`
- `tools/ci/m9_gc_stats.sh`

Notes:
- Head operations for `global_State.tab.retired_nodes` and
  `global_State.tab.retired_arrays` now route through
  `lj_tab_node_retired_head_*()` and `lj_tab_array_retired_head_*()` helpers.
  Runtime retirement push, epoch reclaim, close-time free, legacy GC marking,
  GC2 paranoia scans, and focused fixtures use those helpers.
- Retired-list head access follows the same helper discipline. Comments beside
  the helpers describe the allowed raw-field sites; behavior fixtures exercise
  publication, traversal, and reclamation.

Follow-up validation:
- `git diff --check`
- `tools/ci/lua_test.sh m5_tab_retire`
- `tools/ci/lua_test.sh m5_tab_array_publish`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
