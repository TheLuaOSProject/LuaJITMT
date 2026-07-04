2026-06-20

- Added `tests/lib/suite_build.lua` as the shared home for build and C
  fixture helpers.
- Kept compatibility exports in `suite_runtime.lua` while updating direct
  suite imports to use `suite_build` for C/build helpers.
- 2026-06-28 follow-up: removed those `suite_runtime` compatibility exports
  after the last suite user moved to `suite_build` directly.
- Verified with module-load coverage and a representative serial run:
  `tools/ci/lua_test.sh m2_arena_bitmap m2_arena_state
  m3_gc2_worker_scheduler m4_thr_substrate m5_nbtab_model
  m5_x64_tget_array_header m6_jit_token m7_ffi_cdef_token m8_weak
  m9_gc_stats`.
- 2026-07-03 follow-up: the Lua harness keeps ordinary artifact readers only.
  `Test:read()` remains for logs, fixture outputs, imported-suite inputs, CSVs,
  opaque bytecode round-trip artifacts, and packaging artifacts. Implementation
  files are not harness artifacts. Implementation rules belong in code comments
  and notes, with observable behavior covered by fixtures or public artifacts.
