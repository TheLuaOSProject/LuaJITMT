2026-06-20

- Pauli identified `luaL_loadfilex()` as a remaining native-state gap: raw
  `fopen`, `fread`, and `fclose` could block without marking the TG native.
- Wrapped those file-loader operations in `lj_native_enter` /
  `lj_native_leave`.
- `reader_file()` records STOPREQ actions and stops feeding the parser, but the
  actual STOPREQ throw is deferred until after `lua_loadx()` unwinds so shutdown
  is not converted into a normal `loadfile()` nil/error result.
- Added FIFO STOPREQ coverage for `loadfile(fifo)` and `dofile(fifo)` to the
  existing safepoint handshake fixture.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `loadfile()` smoke via `./src/luajit -e ...`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lib/base`
