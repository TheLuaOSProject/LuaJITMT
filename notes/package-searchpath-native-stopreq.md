2026-06-20

- Wrapped `package.searchpath()` readability probes in native state. The raw
  `fopen`/`fclose` path can block on FIFOs or remote filesystems and should
  acknowledge STOPREQ at the native boundary.
- Added FIFO STOPREQ coverage through
  `package.searchpath("lj_stopreq_probe", fifo)`.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `package.searchpath()` smoke through `./src/luajit -e ...`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lib/contents.lua`
