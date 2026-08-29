2026-06-20

Subject: Shared Lua thread-test harness.

Standalone `tests/t-*.lua` scripts now get `tests/lib/?.lua` in `LUA_PATH`
through `suite_runtime.luajit_script()`. This enables behavior tests to share
Lua helpers without embedding package-path setup in each test.

Added `tests/lib/thread_harness.lua` for common thread-test operations:

- numeric argument/env/default parsing;
- ready/start channel creation and release;
- worker join assertions;
- repeated full-GC calls.

Initial migration covers the M7 cdata allocation/get/set tests. A follow-up
migrated cdef token, clib cache, and arithmetic conversion. The clib/cache and
arithmetic tests still wait for each worker's pre-barrier FFI setup before
spawning the next worker, matching their previous setup serialization.
A later pass migrated callback install/runtime with the same serialized setup
preserved.
A later pass migrated metatype miscmap, ctype intern race, and duplicate-cdef
stack growth. The duplicate-stack migration extends `wait_ready()` with an
optional label so per-round timeout context is preserved.
A later pass migrated `ffi.pin` and mcode-fresh thread barriers. The mcode test
keeps its env-only knobs and progress polling local, but now shares ready/start
and ordered join plumbing.
A later pass migrated `t-ffi-gc-finreg.lua`, preserving its per-worker
pre-barrier waits and all finalizer count validation while sharing barrier,
join, and full-GC plumbing.
A later pass migrated small join-only loops in threading smoke, threading
allocation, and OS reentrancy tests after `join_each()` grew support for extra
worker return values.

Validation:

- `tools/ci/lua_test.sh m7_ffi_cdata_alloc m7_ffi_cdata_get_l m7_ffi_cdata_set_l`
- `tools/ci/lua_test.sh m7_ffi_cdef_token m7_ffi_clib_cache m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi_callback_install m7_ffi_callback_runtime`
- `tools/ci/lua_test.sh m7_ffi_cdef_dup_stack m7_ffi_ctype_ticket_intern m7_ffi_metatype`
- `tools/ci/lua_test.sh m7_ffi_pin m6_jit_mcode_publish`
- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/lua_test.sh m4_threading_smoke m5_threading_alloc m5_os_reentrant`
