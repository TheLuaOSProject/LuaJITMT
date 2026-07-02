# traceref bounds guard

## Context

`tracevec_publish()` stores `J->sizetrace` before publishing the new
`TraceVec *`. A concurrent reader can therefore see the larger size mirror and
then load the previous vector pointer. In release builds, `traceref()` used
`check_exp()` for its vector bounds check, but `check_exp()` compiles to the
unchecked expression.

## Change

`traceref()` now snapshots `J->tracev`, checks the requested trace number
against that vector's own `sizetrace`, and returns `NULL` for out-of-range
requests. This makes the helper safe for callers that loop over a separately
loaded size mirror during trace-vector growth.

The `t-jit-tracevec` fixture now simulates the growth-publication window by
temporarily making the size mirror larger while the old vector is still current.

## Verification

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_jit_trace_publish`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_flush_hs`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_util_flush_race`
