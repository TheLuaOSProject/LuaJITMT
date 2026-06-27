## StrTabHdr retired_next acquire/release helper

Slice: string table retired-header link discipline.

Changes:
- Added `lj_str_retired_next_acq()` and `lj_str_retired_next_rel()` in
  `src/lj_str.h`.
- Routed string retired-list push, epoch reclaim, final table free, legacy GC
  marking, GC2 marking, and GC2 paranoia raw-root traversal through the helper.
- Updated focused string table tests to read retired list heads with acquire
  loads and retired node links through the helper.
- Extended `tools/ci/m5_strtab_cas.sh` to reject direct `StrTabHdr.retired_next`
  field access in the string retirement path, GC scans, and focused tests.
- Follow-up: added helper coverage for the string table heads and retirement
  epoch too. `lj_str_tabh_*()` now owns current table acquire/release/xchg,
  `lj_str_retired_head_*()` owns the retired-list head acquire/CAS/xchg/init
  stores, and `lj_str_retire_epoch_*()` owns the SMR epoch publication readback.
  `lj_str.c`, legacy GC, GC2, state bootstrap, and focused string/arena tests
  now route through those helpers. The M5 string-table CAS guard rejects raw
  `g->str.tabh`, `g->str.retired`, and `StrTabHdr.retire_epoch` access in the
  covered production/test files.
- Follow-up validation: `git diff --check`, `tools/ci/m5_strtab_cas.sh`,
  `tools/ci/m5_strtab_prep.sh`, `tools/ci/m2_arena_gcmark.sh`,
  `tools/ci/m3_gc2_paranoia.sh`, `tools/ci/m10_generational.sh`, and
  `tools/ci/m5_concurrent_objects.sh` passed.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_strtab_prep.sh`
- `tools/ci/m5_strtab_cas.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- String-table head operations are centralized in `lj_str.h`; helper bodies are
  the only covered raw head-field sites.
