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
- Banach audited tests after the user clarified the implementation-text concern.
  No remaining runnable tests were found that inspect `src/*` implementation
  text for pass/fail.
- 2026-07-03 follow-up: removed the unused repository source enumerator from
  the Lua harness. `Test:read()` remains for logs, fixture outputs,
  imported-suite inputs, CSVs, opaque bytecode round-trip artifacts, and
  packaging artifacts; implementation rules belong in code comments and notes,
  with observable behavior covered by fixtures or public artifacts.
- 2026-07-03 clarification: the same rule covers the old milestone wrapper
  suite and any historical local/release-only checks. Do not move those checks
  into another runner. If a historical check described a real requirement, keep
  the requirement as a code-adjacent explanation and cover only the observable
  behavior or product artifact.
