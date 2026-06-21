lj_ccall aggregate classification helper loads

- Routed the x86_64/POSIX aggregate classifier through helper-backed
  `CType.info`, `CType.size`, and `CType.sib` reads for array element walks,
  nested struct classification, field offset accounting, bitfield checks, and
  stack-overflow alignment fallback.
- Routed the default pass-by-value struct alignment macro through
  `ctype_info_acq()` so the common argument stack alignment path no longer
  reads the alignment bits directly from `CType.info`.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw payload reads in the
  x86_64/POSIX C-call aggregate classification helpers.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
