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
- Extended `tools/ci/m5_tab_retire.sh` and `tools/ci/m5_tab_array_publish.sh`
  to reject direct `ret->next` / `aret->next` access in table retirement files.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_tab_retire.sh`
- `tools/ci/m5_tab_array_publish.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- Head operations for `global_State.tab.retired_nodes` and
  `global_State.tab.retired_arrays` now route through
  `lj_tab_node_retired_head_*()` and `lj_tab_array_retired_head_*()` helpers.
  Runtime retirement push, epoch reclaim, close-time free, legacy GC marking,
  GC2 paranoia scans, and focused fixtures use those helpers.
- `tools/ci/m5_tab_retire.sh` and `tools/ci/m5_tab_array_publish.sh` reject
  raw table retired head access in the implementation, scanner, and focused
  fixtures.

Follow-up validation:
- `git diff --check`
- `tools/ci/m5_tab_retire.sh`
- `tools/ci/m5_tab_array_publish.sh`
- `tools/ci/m3_gc2_paranoia.sh`
