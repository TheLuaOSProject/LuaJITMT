# jit.util trace reader guards

## Context

The `jit.util` trace reflection API is user-visible and can be called while
another thread is flushing traces. Its shared trace-number gate used
`traceref()` directly, so helpers such as `traceinfo`, `traceir`, `tracek`,
`tracesnap`, `tracemc`, and `traceexitstub` could observe a stale, flushing, or
reused trace slot.

## Change

`jit_checktrace()` now snapshots the current trace vector once, bounds-checks
against that same vector, and returns a trace only when:

- the requested trace number is in range,
- the trace slot still names the same trace number, and
- the trace body is not retired or in scoped-flush retirement.

Callers keep the existing nil-return behavior for unavailable traces. This adds
no lock or wait; it just turns stale/racy inspection into "trace unavailable"
instead of exposing retired metadata.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_util_flush_race`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit`
