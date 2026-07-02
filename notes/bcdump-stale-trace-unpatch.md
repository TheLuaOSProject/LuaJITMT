# Bytecode dump stale trace unpatching

## Context

`string.dump` copies prototype bytecode, then rewrites internal JIT opcodes back
to ordinary bytecode before emitting the dump. The JIT opcodes carry trace
numbers, so the writer needs trace metadata to recover the original
instruction for `JFORL`, `JITERL`, and `JLOOP`.

## Change

- The bytecode writer now decodes copied instructions as `BCIns` instead of
  editing endian-selected bytes.
- Trace-backed unpatching validates that the trace slot still names the
  requested trace number before reading immutable `startins`.
- If the copied instruction names a trace that has already been cleared by a
  concurrent flush, the writer retries from the live prototype bytecode. A
  cleared trace slot is only valid after root bytecode has been unpatched.
- If that invariant is violated, dumping fails under the existing protected
  writer call instead of emitting malformed bytecode.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_bcdump_compat`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
