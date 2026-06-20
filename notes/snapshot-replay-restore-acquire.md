Snapshot replay/restore acquire slice
=====================================

Changes:
- Added acquire helpers for snapshot top slots, snapshot PC entries, and next
  snapshot map offsets.
- Converted rename/regsp snapshot helpers to acquire trace IR bases, trace
  instruction counts, snapshot bases, snapshot map bases, and map entries.
- Converted side-trace snapshot replay to snapshot acquired parent trace
  IR/snapshot/map metadata and acquired snapshot map entries.
- Converted trace-exit snapshot restore and unsink paths to acquired
  IR/snapshot/map metadata, including frame-link map reads and cdata/table
  unsinking.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
