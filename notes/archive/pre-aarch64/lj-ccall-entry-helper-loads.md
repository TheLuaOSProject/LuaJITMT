lj_ccall entry helper loads

- Routed `lj_ccall_func()` through `ctype_info_acq()` and `ctype_size_acq()`
  while stripping pointer wrappers and validating the target function ctype
  before C-call argument setup.
- Refreshed the helper-backed info snapshot after the call for the Windows
  stdcall convention check while leaving the existing declaration patch write
  intact.
- Follow-up lifetime cleanup routes `lj_ccall_func()` entry through waitable
  exact ctype snapshots while stripping function-pointer attributes, and
  re-snapshots the resolved function ctype after the native call before result
  conversion. The x86/Windows stdcall declaration patch remains the only live
  table write in that target-specific branch.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `git diff --check`
