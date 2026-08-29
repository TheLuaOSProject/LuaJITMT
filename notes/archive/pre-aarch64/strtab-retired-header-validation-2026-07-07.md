# StrTab Retired Header Validation

Retired string-table headers are raw SMR side-list records. Legacy GC, GC2,
GC2 paranoia, reclaim, and close-time free were walking the retired header list
and reading `retired_next` / `retire_epoch` without first proving the header
record was still registered memory.

This slice mirrors the table/mcode/CLib retired-list rule:

- guard legacy and GC2 retired strtab root scans with
  `lj_gc2_mem_registered()` before reading a retired header link;
- guard GC2 paranoia raw-root checks the same way;
- guard `lj_str_reclaim_retired()` and close-time `lj_str_freetab()` before
  dereferencing or freeing detached retired headers;
- update `t-strtab-cas` to assert the current opportunistic reclaim contract:
  active SMR readers make reclaim return without detaching the retired header,
  and a later reclaim after the reader leaves drains it.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m5_strtab_cas m5_strtab_prep m5_strtab_gc_stress`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
