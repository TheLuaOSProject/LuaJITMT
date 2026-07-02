# CLib cache next-link guard

## 2026-06-20

- `CLibCacheEntry.next` already routes through `lj_clib_cache_next_acq()` and
  `lj_clib_cache_next_rel()` in `src/lj_clib.h`.
- Added a focused guard to `tools/ci/m7_ffi_clib_cache.sh` over:
  - `src/lj_clib.c`
  - `src/lj_gc.c`
  - `src/lj_gc2.c`
  - `src/lj_crecord.c`
- The guard intentionally excludes `src/lj_clib.h`, where the helper
  definitions spell the field.
- Follow-up: the GC2 retired-entry root now routes through
  `gc2_clib_cache_retired_*()` helpers in `src/lj_obj.h`, and the same guard
  documents why raw `g->gc2.clib_cache_retired` access in production C files.

## Validation target

- `tools/ci/m7_ffi_clib_cache.sh`
