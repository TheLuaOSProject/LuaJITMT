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
- Follow-up lifetime cleanup refreshes the return child with the ccall-local
  raw-id snapshot helper and carries the resolved `CTypeID` into
  `lj_cconv_tv_ct_l()`. The result path no longer reopens the return type via a
  raw table pointer after the native call or after parser waits.
- Documented why this shared state is owned by the helper surface. Active
  coverage stays in the native ccall and CType metadata snapshot suites; the
  helper comments carry the ordering rationale.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `cd tests/stock/test && ../../../src/luajit test.lua --quiet lib/ffi`
- `git diff --check`
