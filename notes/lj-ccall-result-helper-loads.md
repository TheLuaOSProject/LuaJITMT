lj_ccall result helper loads

- Routed `ccall_get_results()` through `ctype_info_acq()` and
  `ctype_size_acq()` for return child lookup, return-type dispatch, BE slot
  adjustment, FPR selection, and reference-type assertions.
- Routed the active x86_64/POSIX small struct and complex return macros through
  helper-backed return-size reads.
- Routed the active x86_64/POSIX register argument vector check through a
  helper-backed info load.
- Documented the invariant formerly checked by `m7_ffi_cdata_get_l`: raw `CType.info` and
  `CType.size` reads in `ccall_get_results()` and the x86_64/POSIX C-call
  macro block.

Verification:

- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
