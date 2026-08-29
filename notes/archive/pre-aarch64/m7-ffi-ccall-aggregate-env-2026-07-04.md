# M7 FFI ccall Aggregate Environment

Date: 2026-07-04

The full `m7_ffi` aggregate reproduced a late `m7_ffi_ccall_native` failure
where `t-ffi-ccall-struct-overflow` ran without
`LJ_M7_FFI_CCALL_STRUCT_SO`. The focused `m7_ffi_ccall_native` case already
passed because its command transcript carried the shared-library environment
and timeout.

The ccall native case now keeps env-sensitive C fixtures as explicit
`build_and_run_c()` calls. This makes the required library environment and
timeout visible at the callsite and keeps the aggregate path aligned with the
focused case.

The aggregate path still reproduced the loss after a long run. The common C
fixture helper now snapshots run options, including `env`, before it performs
any clean build or compile command. That prevents later helper activity from
mutating or losing the child process environment for the final fixture launch.

The suite runner also keeps its own control-plane Lua interpreted. The child
LuaJIT processes still run with each test's requested JIT mode; only the harness
that builds command lines, environment tables, and rebuilds the VM avoids
depending on in-process JIT stability.

Verification:

- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi`
