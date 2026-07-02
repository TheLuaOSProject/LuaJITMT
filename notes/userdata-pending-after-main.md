# Userdata Pending After Main

- Userdata allocation now publishes fresh userdata through
  `lj_gc_linkobj_new_after_main()` instead of directly CAS-inserting after the
  main thread.
- This removes the remaining direct global-root CAS from the normal userdata
  allocation path while preserving LuaJIT's userdata chain ordering: pending
  userdata flushes immediately after the main thread, matching the old list
  shape used by userdata finalizer discovery and shutdown sweep.
- The pending-root test now covers userdata in the after-main queue alongside
  thread objects, asserting that it is not visible in the root chain until
  `lj_gc_flush_root_pending()` publishes it.
- Verification:
  - `make -C src -j2`
  - `tools/ci/lua_test.sh m3_gc_root_pending m2_arena_gcsweep m3_gc2_worker_scheduler`
  - `tools/ci/lua_test.sh m8_weak`
- Baseline failures reproduced on unmodified `HEAD` and kept separate:
  - `tools/ci/lua_test.sh m3_gc2_paranoia` fails at stock-suite teardown with
    `close_state: memory leak of 16598 bytes`.
  - `tools/ci/lua_test.sh m7_ffi_finreg` fails
    `tests/t-ffi-gc-trace.lua` with `expected ffi.gc recorder traces`.
