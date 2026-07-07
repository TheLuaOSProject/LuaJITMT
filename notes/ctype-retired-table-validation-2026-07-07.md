# CType Retired Table Validation

Retired CType table headers are raw SMR side-list records. Legacy GC, GC2,
GC2 paranoia, reclaim, and close-time free were walking the retired table list
and reading `retired_next` / `retire_epoch` without first proving the header
record was still registered memory.

This slice mirrors the table/mcode/CLib/string retired-list rule:

- guard legacy and GC2 retired CType table root scans with
  `lj_gc2_mem_registered()` before reading a retired header link;
- guard GC2 paranoia raw-root checks the same way;
- guard `lj_ctype_reclaim_retired()` and close-time `ctype_freeretired()`
  before dereferencing or freeing detached retired headers.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_ctype_tab_retire`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
