# x64 JLOOP live trace entry

## Context

The x64 interpreter-side JLOOP entry and static fallback paths already rejected
missing and pending trace slots before using `TRACE->mcode` or
`TRACE->startins`. They did not verify that the loaded slot still named the
decoded trace number or that the trace body was not already scoped-flushing.

## Change

The VM now preserves the decoded trace number in `TMPRd`, loads the trace slot,
then requires:

- slot value is not `NULL` or `LJ_TRACE_PENDING`,
- `TRACE->traceno` still matches the decoded trace number, and
- `TRACE->retire_epoch == 0`.

Only then does it enter `TRACE->mcode` or read `TRACE->startins`; otherwise it
takes the existing stale-slot interpreter fallback.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m3_vm_safepoint`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
