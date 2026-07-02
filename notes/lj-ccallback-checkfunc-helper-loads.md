lj_ccallback callback_checkfunc helper loads

- Routed `callback_checkfunc()` through `ctype_info_acq()` and
  `ctype_size_acq()` for callback function-pointer, return-type, and argument
  validation.
- Routed callback argument sibling walks through `ctype_sib_acq()`.
- Documented the invariant formerly checked by `m7_ffi_callback_install`: raw `CType.info`,
  `CType.size`, and `CType.sib` reads in `callback_checkfunc()`.

Verification:

- `tools/ci/m7_ffi_callback_install.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
