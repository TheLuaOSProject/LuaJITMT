lj_ccall entry helper loads

- Routed `lj_ccall_func()` through `ctype_info_acq()` and `ctype_size_acq()`
  while stripping pointer wrappers and validating the target function ctype
  before C-call argument setup.
- Refreshed the helper-backed info snapshot after the call for the Windows
  stdcall convention check while leaving the existing declaration patch write
  intact.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `git diff --check`
