# JIT IO native STOPREQ boundary

Traced `io.write`, `file:write`, `io.flush`, and `file:flush` previously
lowered to direct libc `fputc`/`fwrite`/`fflush` IR calls from
`recff_io_write()` and `recff_io_flush()`. Those calls could block in stdio
without marking the current TG native, unlike the interpreter IO paths that use
`io_native_*()` wrappers and acknowledge STOPREQ on native leave.

For now the recorder treats IO write/flush fast functions as NYI. This
preserves Lua semantics by falling back to the interpreter's native-state
wrappers, trading traced IO throughput for safe shutdown/safepoint behavior.
The raw libc stdio call targets are also absent from generated `jit.vmdef`
metadata, so reintroducing traced stdio requires an explicit IR-call table
change.

`tests/t-jit-io-native-stopreq.lua` heats all four write/flush variants; the
M6 suite captures the generated IR and documents why raw stdio calls while no
native-state JIT helper exists. Trace stitching around the interpreter call
boundary is acceptable.
