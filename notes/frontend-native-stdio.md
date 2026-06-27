# Standalone frontend native stdio

Date: 2026-06-20

## Problem

The standalone `luajit` frontend still used raw `fputs`/`fputc`/`fwrite` /
`fflush` / `fgets` for REPL prompts, version/JIT status, usage output, and
normal error reporting. Library-level `print()`, `debug.debug()`, loadfile,
package, IO, and dynamic-loader paths had native-state wrappers, but the
top-level executable could still block on terminal or pipe I/O without marking
the current TG native.

## Fix

`src/luajit.c` now has local frontend stdio wrappers. When a `lua_State *` is
available, they enter native state, perform the stdio call, leave native state,
and deliver STOPREQ actions through `lj_safepoint_checkstop()`. Pre-state fatal
messages and the post-`lua_cpcall()` raw reporter remain raw because there is
no protected Lua boundary for a STOPREQ throw there.

Covered call sites:

- `-h` / usage output once `pmain()` is running
- `-v` / startup version output
- JIT status output
- REPL prompt writes and stdin reads
- REPL trailing newline
- protected script/command error reporting

## Verification

Passed:

- `make -C src clean && make -C src -j$(nproc)`
- `./src/luajit -v`
- `printf 'print(42)\n' | ./src/luajit`
- `./src/luajit -e 'error("frontend smoke")'`
- `tools/ci/lua_test.sh m0_matrix`

## Fresh STOPREQ follow-up

Date: 2026-06-27

The frontend wrappers now use a local fresh STOPREQ helper. A pre-existing
sticky shutdown flag no longer interrupts an otherwise successful prompt,
stdout/stderr, or stdin native stdio call unless the native region
acknowledges a new STOPREQ action or the flag appears during the call.

`tools/ci/m3_safepoint_handshake.sh` now guards the frontend wrappers against
reintroducing direct `lj_safepoint_checkstop(L, lj_native_leave(L))` calls.
