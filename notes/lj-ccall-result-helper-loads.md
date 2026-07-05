lj_ccall result helper loads

- Routed `ccall_get_results()` through `ctype_info_acq()` and
  `ctype_size_acq()` for return child lookup, return-type dispatch, BE slot
  adjustment, FPR selection, and reference-type assertions.
- Routed the active x86_64/POSIX small struct and complex return macros through
  helper-backed return-size reads.
- Routed the shared default small-struct return copy through
  `ctype_size_acq()`. This covers Windows/x64 in the current release target
  set without changing the ABI-specific register-copy layout.
- Routed the active x86_64/POSIX register argument vector check through a
  helper-backed info load.
- Documented why this shared state is owned by the helper surface. Active
  coverage stays in the native ccall and CType metadata snapshot suites; the
  helper comments carry the ordering rationale.

Verification:

- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `cd tests/stock/test && ../../../src/luajit test.lua --quiet lib/ffi`
- `git diff --check`
