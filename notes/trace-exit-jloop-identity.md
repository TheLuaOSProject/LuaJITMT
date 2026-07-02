# Trace-exit JLOOP identity guard

## Context

`lj_trace_exit()` can redispatch a `BC_JLOOP` from C after leaving compiled
code. The VM-side x64 path already guards missing/stale trace slots, but this C
fallback only checked for a missing slot before reading the target trace
`startins`.

## Change

The fallback now validates that the trace slot still names the requested trace
number and is not retired or scoped-flushing before reading `startins`. A
missing, reused, or retiring slot returns `0` and lets the interpreter
redispatch the current bytecode, matching the existing stale flush behavior.

This does not add a lock or wait. Trace body lifetime is still provided by the
exit-trace handshake; this check only documents and enforces slot identity.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
