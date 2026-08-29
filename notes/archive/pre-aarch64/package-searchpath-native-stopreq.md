2026-06-20

- Wrapped `package.searchpath()` readability probes in native state. The raw
  `fopen`/`fclose` path can block on FIFOs or remote filesystems and should
  acknowledge STOPREQ at the native boundary.
- Added FIFO STOPREQ coverage through
  `package.searchpath("lj_stopreq_probe", fifo)`.
- Follow-up cleanup made the readable-file open/close probes check only fresh
  native STOPREQ acknowledgements. A pre-existing sticky STOPREQ no longer
  interrupts a successful `package.searchpath()` probe that did not observe a
  new native stop.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `package.searchpath()` smoke through `./src/luajit -e ...`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lib/contents.lua`
