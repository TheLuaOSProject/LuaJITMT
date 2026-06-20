2026-06-20

- Added `tests/lib/suite_build.lua` as the shared home for build and C
  fixture helpers.
- Kept compatibility exports in `suite_runtime.lua` while updating direct
  suite imports to use `suite_build` for C/build helpers.
- Verified with module-load coverage and a representative serial run:
  `tools/ci/lua_test.sh m2_arena_bitmap m2_arena_state
  m3_gc2_worker_scheduler m4_thr_substrate m5_nbtab_model
  m5_x64_tget_array_header m6_jit_token m7_ffi_cdef_token m8_weak
  m9_gc_stats`.
- Banach audited tests for source-text assertions after the user clarified
  that generated result matching, including JIT dumps, is acceptable. No
  remaining direct `src/*` source-text assertions were found.
