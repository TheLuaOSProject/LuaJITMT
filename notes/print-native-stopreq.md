2026-06-20

- Hypatia identified `print()` as a remaining blocking native-output gap:
  stdout `putchar`/`fwrite` could block on a full pipe/tty without marking the
  TG native.
- Replaced raw `putchar`/`fwrite` in `lib_base.c` with native-enter/leave
  wrappers that check STOPREQ after each stdout write.
- Added C fixture coverage that fills a pipe, redirects stdout to it, runs a
  large `print()`, publishes STOPREQ while the write is blocked, drains the
  pipe from a helper thread, and verifies the VM shutdown interruption.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - normal `print()` smoke through `./src/luajit -e ...`
  - `tools/ci/run_stock_tests.sh ./src/luajit --quiet lib/base`
