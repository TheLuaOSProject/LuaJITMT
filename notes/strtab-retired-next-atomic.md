## StrTabHdr retired_next acquire/release helper

Slice: string table retired-header link discipline.

Changes:
- Added `lj_str_retired_next_acq()` and `lj_str_retired_next_rel()` in
  `src/lj_str.h`.
- Routed string retired-list push, epoch reclaim, final table free, legacy GC
  marking, GC2 marking, and GC2 paranoia raw-root traversal through the helper.
- Updated focused string table tests to read retired list heads with acquire
  loads and retired node links through the helper.
- Documented the rule that `StrTabHdr.retired_next` is a shared retired-list
  publication link and must use the helper. String retirement, GC scans, and
  focused fixtures cover the observable behavior; CI must not enforce helper
  spelling by repository text assertion.
- Follow-up: added helper coverage for the string table heads and retirement
  epoch too. `lj_str_tabh_*()` now owns current table acquire/release/xchg,
  `lj_str_retired_head_*()` owns the retired-list head acquire/CAS/xchg/init
  stores, and `lj_str_retire_epoch_*()` owns the SMR epoch publication readback.
  `lj_str.c`, legacy GC, GC2, state bootstrap, and focused string/arena tests
  now route through those helpers. Comments beside the helper layer document
  the allowed raw-field sites for `g->str.tabh`, `g->str.retired`, and
  `StrTabHdr.retire_epoch`.
- Follow-up validation: `git diff --check`,
  `tools/ci/lua_test.sh m5_strtab_cas`,
  `tools/ci/lua_test.sh m5_strtab_prep`,
  `tools/ci/lua_test.sh m2_arena_gcmark`,
  `tools/ci/lua_test.sh m3_gc2_paranoia`,
  `tools/ci/lua_test.sh m10_generational`, and
  `tools/ci/lua_test.sh m5_concurrent_objects` passed.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m5_strtab_prep`
- `tools/ci/lua_test.sh m5_strtab_cas`
- `tools/ci/lua_test.sh m9_gc_stats`

Notes:
- String-table head operations are centralized in `lj_str.h`; helper bodies are
  the only covered raw head-field sites.
