# C fixture sleep helper

The C concurrency fixtures now share `tests/lib/test_sleep.h` for the local
`sleep_ns()` retry loop used by STOPREQ, snapshot, scheduler, and resize tests.

The helper intentionally preserves the old fixture behavior: interrupted
`nanosleep()` calls keep waiting with the remaining interval. Keeping this loop
in one test header avoids divergent fixture-local spellings while leaving runtime
source and platform behavior unchanged.

Validation:

- `tools/ci/lua_test.sh m7_ffi_ccall_native m7_ffi_callback_install m7_ffi_carith_l m7_ffi_cdef_token m7_ffi_clib_cache m7_ffi_typeinfo_snapshot m7_ffi_metatype m7_ffi_snap_restore_l`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler m3_vm_safepoint m4_threading_spawn_native m5_tab_resize_copy_helper`
- `git diff --check`
