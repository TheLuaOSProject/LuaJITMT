lj_ccall entry helper loads

- Routed `lj_ccall_func()` through `ctype_info_acq()` and `ctype_size_acq()`
  while stripping pointer wrappers and validating the target function ctype
  before C-call argument setup.
- Refreshed the helper-backed info snapshot after the call for the Windows
  stdcall convention check while leaving the existing declaration patch write
  intact.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw `CType.info` and
  `CType.size` reads in `lj_ccall_func()`, excluding the Windows stdcall patch
  write.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
