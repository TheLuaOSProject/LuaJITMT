lj_ccall aggregate classification helper loads

- Routed the x86_64/POSIX aggregate classifier through helper-backed
  `CType.info`, `CType.size`, and `CType.sib` reads for array element walks,
  nested struct classification, field offset accounting, bitfield checks, and
  stack-overflow alignment fallback.
- Routed the default pass-by-value struct alignment macro through
  `ctype_info_acq()` so the common argument stack alignment path no longer
  reads the alignment bits directly from `CType.info`.
- Follow-up small-struct overflow cleanup caches the x86_64/POSIX stack
  alignment before `ccall_struct_arg()` calls the wait-capable
  `lj_cconv_ct_tv_l()` conversion helper. The stack spill path no longer
  rereads the raw destination `CType *` after conversion may have parked on the
  parser token.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw payload reads in the
  x86_64/POSIX C-call aggregate classification helpers.
- `tests/t-ffi-ccall-struct-overflow.c` forces six integer arguments followed
  by a parser-owned small struct argument so SysV x64 spills the converted
  struct to the stack after waiting in native time.

Verification:

- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
