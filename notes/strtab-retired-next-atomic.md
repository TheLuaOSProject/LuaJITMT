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

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_strtab_prep.sh`
- `tools/ci/m5_strtab_cas.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- `global_State.str.retired` head operations stay as explicit acquire/CAS/xchg
  sites. This slice centralizes the per-node retired link only.
