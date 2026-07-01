# FFI CALLXS record gate

`m7_ffi_ccall_native` now attempts an opt-in build with
`XCFLAGS=-DLJ_FFI_RECORD_CALLS=1` and requires the compile-time
`IR_CALLXS native-state protocol` error.

This pins the direct-`IR_CALLXS` safety boundary: broad ordinary FFI C calls
must keep falling back to the interpreted `lj_ccall_func()` path, which enters
native state around `lj_vm_ffi_call()` and performs fresh STOPREQ handling,
until x64 `IR_CALLXS` lowering can preserve ABI results and run the same native
enter/leave protocol.

The narrow integer/pointer GPR trampoline family in
`lj_ccall_jit_{void,i32,ptr}_gpr()` is a separate helper-call bridge for 0, 1,
or 2 exact signed 32-bit integer and pointer arguments, with zero-result void,
signed 32-bit integer, or pointer returns. It traces through `IRCALL`, not
`IR_CALLXS`, and keeps the compile-time `LJ_FFI_RECORD_CALLS` gate intact.
The separate `lj_ccall_jit_num_fpr()` helper traces exact double returns with
0, 1, or 2 exact double arguments through the same native-state bridge.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
