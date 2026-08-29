# CLib Cache Next-Link Invariant

## 2026-06-20

- `CLibCacheEntry.next` already routes through `lj_clib_cache_next_acq()` and
  `lj_clib_cache_next_rel()` in `src/lj_clib.h`.
- The shared CLib cache chain is read by normal lookup, FFI recording, and
  collector/retirement paths, so production code must use the helper layer
  rather than spelling the shared link access inline.
- Follow-up: the GC2 retired-entry root now routes through
  `gc2_clib_cache_retired_*()` helpers in `src/lj_obj.h` for the same reason:
  the helper names the acquire/release edge and keeps the nonblocking ownership
  rule beside the field access.

## Validation target

- Use `tools/ci/lua_test.sh m7_ffi_clib_cache`.
