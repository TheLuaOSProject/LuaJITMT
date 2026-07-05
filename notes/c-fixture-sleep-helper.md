# C fixture sleep helper

The C concurrency fixtures now share `tests/lib/test_sleep.h` for the local
`sleep_ns()` retry loop used by STOPREQ, snapshot, scheduler, and resize tests.

The helper intentionally preserves the old fixture behavior: interrupted
`nanosleep()` calls keep waiting with the remaining interval. Keeping this loop
in one test header avoids divergent fixture-local spellings while leaving runtime
source and platform behavior unchanged.

The follow-up pass also routed the table FORWARD/KEYLOCK x64 fixtures and the
M7 traced-ccall probe library through the same helper. Millisecond-based probe
callers keep their existing no-wait behavior for non-positive durations before
calling `sleep_ns()`.

Validation:

- `tools/ci/lua_test.sh m7_ffi_ccall_native m7_ffi_callback_install m7_ffi_carith_l m7_ffi_cdef_token m7_ffi_clib_cache m7_ffi_typeinfo_snapshot m7_ffi_metatype m7_ffi_snap_restore_l`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler m3_vm_safepoint m4_threading_spawn_native m5_tab_resize_copy_helper`
- `tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_forward_filter m5_x64_tget_array_header m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot m7_ffi_ccall_native`
- `git diff --check`
