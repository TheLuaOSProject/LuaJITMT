# CLib Retired Cache Validation

Retired `ffi.C` cache entries are raw SMR side-list nodes. Legacy GC and GC2
were walking `CLibCacheEntry.retired_next`, `name`, and `val` directly after
loading the retired-list head.

This slice mirrors the established retired table/mcode pattern:

- validate each retired cache entry with `lj_gc2_mem_registered()` before
  reading its next link or payload;
- bound legacy, paranoia, and GC2 retired-cache scans with the root-scan limit;
- apply the same validation to the CLib retired reclaim/free loops after their
  list-head exchange.

The payloads are still marked normally once the raw node is known to be a
registered retired-list entry under the caller's SMR read section.

Validation:

- `tools/ci/lua_test.sh m7_ffi_clib_cache`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
