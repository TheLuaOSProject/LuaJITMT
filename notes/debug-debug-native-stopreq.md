2026-06-20

- Hypatia identified `debug.debug()` as a remaining interactive blocking gap:
  prompt/error writes and the stdin `fgets()` loop did not mark native state.
- Wrapped `debug.debug()`'s `fputs` and `fgets` calls in native-enter/leave
  helpers with STOPREQ checks.
- Added a pipe-backed stdin test to `t-safepoint-handshake.c`: the helper waits
  for native state, publishes STOPREQ, writes `cont\n` to unblock the debugger,
  and verifies the shutdown interruption.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `debug.debug()` continuation smoke with `cont\n`
  - direct `tests/stock/test/misc/debug_gc.lua`
  - direct `tests/stock/test/lang/meta/debuginfo.lua`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lang/meta/debuginfo.lua`
