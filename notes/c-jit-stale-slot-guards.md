# C-side JIT stale trace-slot guards

## Context

The x64 VM now validates BC_JLOOP trace slots before using published trace
metadata. The same stale-slot failure mode still existed in C recorder and
assembler paths that read `tracev` entries directly while another thread could
flush or retire traces.

## Change

- Added recorder-side trace-slot validation before using linked/root/parent
  traces for loop recording, side-trace setup, and side-limit checks.
- Added assembler-side validation for the side-trace parent pointer.
- Kept assembler tail-link BC_JLOOP return-PC recovery optional: if the target
  trace slot is missing, stale, or retired, assembly continues from the snapshot
  PC instead of aborting. Recursive/up-recursive traces can legitimately see a
  target that is not a stable published trace yet, so making this lookup fatal
  breaks trace formation.
- The guards check trace number identity and retirement epoch. They retry
  recording/assembly with `LJ_TRERR_RETRY` instead of blocking or adding locks.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_recursive_call_unroll`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LUA=src/luajit tools/ci/lua_test.sh m7_ffi`
