# FFI CALLXS record gate

`m7_ffi_ccall_native` now attempts an opt-in build with
`XCFLAGS=-DLJ_FFI_RECORD_CALLS=1` and requires the compile-time
`IR_CALLXS native-state protocol` error.

This pins the direct-`IR_CALLXS` safety boundary: broad ordinary FFI C calls
must keep falling back to the interpreted `lj_ccall_func()` path, which enters
native state around `lj_vm_ffi_call()` and performs fresh STOPREQ handling,
until x64 `IR_CALLXS` lowering can preserve ABI results and run the same native
enter/leave protocol.

The narrow `int f(int)` trampoline in `lj_ccall_jit_i32_i32()` is a separate
helper-call bridge. It traces through `IRCALL`, not `IR_CALLXS`, and keeps the
compile-time `LJ_FFI_RECORD_CALLS` gate intact.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
