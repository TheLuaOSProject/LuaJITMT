# trace startins live guards

## Context

Two C-side trace metadata readers could still consume `GCtrace.startins` from a
trace that was identity-valid but already marked for scoped flush retirement:

- `lj_trace_exit()` when redispatching a `BC_JLOOP` from C.
- `string.dump` bytecode unpatching through `bcwrite_unpatch_jitins()`.

## Change

Both readers now require `retire_epoch == 0` in addition to trace-number
identity before reading `startins`. Scoped-flushing traces are treated as
unavailable: trace exit redispatches through the interpreter, and bytecode
dumping retries from live bytecode or raises the existing protected flush error.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh m5_bcdump_compat`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
