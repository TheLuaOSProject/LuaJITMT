2026-06-20

- Pauli identified `luaL_loadfilex()` as a remaining native-state gap: raw
  `fopen`, `fread`, and `fclose` could block without marking the TG native.
- Wrapped those file-loader operations in `lj_native_enter` /
  `lj_native_leave`.
- Follow-up cleanup wrapped the remaining `feof` and `ferror` FILE-state
  probes too. Their action masks are folded into the same deferred
  `reader_file()`/post-parse delivery path so parser cleanup and file close
  still run before a STOPREQ throw.
- `reader_file()` records STOPREQ actions and stops feeding the parser, but the
  actual STOPREQ throw is deferred until after `lua_loadx()` unwinds so shutdown
  is not converted into a normal `loadfile()` nil/error result.
- `tools/ci/m3_safepoint_handshake.sh` now guards against reintroducing raw
  `feof`/`ferror` calls in `lj_load.c`.
- Added FIFO STOPREQ coverage for `loadfile(fifo)` and `dofile(fifo)` to the
  existing safepoint handshake fixture.
- Follow-up fresh STOPREQ cleanup: `luaL_loadfilex()` now snapshots the sticky
  shutdown flag once before opening/reading and carries it through
  `reader_file()`. Pre-existing sticky STOPREQ no longer stops the reader,
  converts open failure into shutdown, or throws after the close; STOPREQ
  observed during loader native operations is still deferred until parser
  cleanup and file close complete. The safepoint handshake fixture covers
  sticky successful `loadfile()` and sticky missing-file behavior.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `loadfile()` smoke via `./src/luajit -e ...`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lib/base`
