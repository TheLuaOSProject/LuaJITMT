lj_ccall result helper loads

- Routed `ccall_get_results()` through `ctype_info_acq()` and
  `ctype_size_acq()` for return child lookup, return-type dispatch, BE slot
  adjustment, FPR selection, and reference-type assertions.
- Routed the active x86_64/POSIX small struct and complex return macros through
  helper-backed return-size reads.
- Routed the active x86_64/POSIX register argument vector check through a
  helper-backed info load.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_get_l` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
