lj_ccallback callback_checkfunc helper loads

- Routed `callback_checkfunc()` through `ctype_info_acq()` and
  `ctype_size_acq()` for callback function-pointer, return-type, and argument
  validation.
- Routed callback argument sibling walks through `ctype_sib_acq()`.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_install` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- `tools/ci/m7_ffi_callback_install.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
