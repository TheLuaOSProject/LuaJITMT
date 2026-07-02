2026-07-02

Slice: x64 stale trace-slot guards for JLOOP entry and static fallback.

Changes:
- x64 `BC_JLOOP` now validates `J->tracev` and the selected trace slot before
  dereferencing `TRACE->mcode`.
- x64 static fallback paths that recover `TRACE->startins` for a replaced
  `BC_JLOOP` now validate the vector and slot first.
- If a direct stale `BC_JLOOP` sees a cleared or pending slot, it reloads the
  current bytecode from `PC-4` and dispatches that live instruction. If the
  stale entry came from a JIT opcode that already performed its interpreter
  work (`JFORL`, `JITERL`, `JFUNCF`, etc.), it continues with `ins_next`.
- If static fallback sees a missing slot, it static-dispatches the current
  bytecode from `PC-4`; a still-stale `BC_JLOOP` advances with `cont_nop`
  rather than recursively redispatching the same missing trace.
- `lj_trace_exit()` now treats a missing `BC_JLOOP` trace body as a normal
  interpreter redispatch instead of dereferencing a null trace.
- `m6_jit_flush_hs` now source-guards the x64 VM and C trace-exit shapes.

Reasoning:
- Full trace flush unpatches bytecode before clearing trace slots, but another
  thread can already have decoded a stale `BC_JLOOP` before the slot is cleared.
  That thread must not dereference `NULL` or `LJ_TRACE_PENDING`.
- The fix stays local to the trace-entry recovery path. It does not add locks,
  sleeps, or alternate temporary runtime paths.

Validation:
- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m3_vm_safepoint`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LUA=src/luajit tools/ci/lua_test.sh m7_ffi`
