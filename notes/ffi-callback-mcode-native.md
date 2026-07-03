# FFI Callback Mcode Native Boundary

`luaopen_ffi()` allocates and publishes the callback trampoline page before any
callback can be installed. On x86_64 Linux that path uses `mmap()` followed by
`mprotect()` while a valid Lua state is available, so both calls must run as
native regions.

The native boundary lets a concurrent STOPREQ handshake acknowledge the loading
thread if libc or the kernel stalls during callback mcode setup. If a fresh
STOPREQ is observed before `CTState.cb.mcode` is published, the unpublished page
is discarded and `require("ffi")` exits with the normal VM-shutdown error. A
sticky STOPREQ bit that was already present before callback mcode setup began is
not treated as a new failure by this lower-level helper.

`tests/t-ffi-callback-mcode-native.c` verifies this behavior by wrapping the
generated runtime `mmap()` and `mprotect()` calls and pausing only while the TG
is already native. This keeps the behavioral path focused and avoids legacy wrapper
assertions.
